// io_uring_wrapper.h - Minimal io_uring wrapper using raw syscalls
//
// No external liburing dependency required. Uses io_uring_setup(2),
// io_uring_enter(2) syscalls directly via syscall().
//
// Supports:
//   - Fixed-size SQ/CQ ring setup
//   - IORING_OP_READ (async file read)
//   - Batch submit + reap completions
//   - Registered buffers (optional, for zero-copy O_DIRECT)
//
// Kernel requirement: Linux 5.1+ (we have 6.17)

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/io_uring.h>

// ============================================================
// io_uring UAPI constants (from linux/io_uring.h)
// ============================================================

#ifndef IORING_SETUP_CLAMP
#define IORING_SETUP_CLAMP  (1U << 4)
#endif

#ifndef IORING_ENTER_GETEVENTS
#define IORING_ENTER_GETEVENTS  (1U << 0)
#endif

#ifndef IORING_OFF_SQ_RING
#define IORING_OFF_SQ_RING   0ULL
#endif

#ifndef IORING_OFF_CQ_RING
#define IORING_OFF_CQ_RING   0x8000000ULL
#endif

#ifndef IORING_OFF_SQES
#define IORING_OFF_SQES      0x10000000ULL
#endif

// ============================================================
// IoUring: minimal io_uring wrapper
// ============================================================

class IoUring {
public:
    explicit IoUring(unsigned entries = 128, int flags = 0)
        : ring_fd_(-1), sq_entries_(0), cq_entries_(0)
    {
        memset(&params_, 0, sizeof(params_));

        // Setup io_uring
        ring_fd_ = (int)syscall(__NR_io_uring_setup, entries, &params_);
        if (ring_fd_ < 0) {
            perror("io_uring_setup failed");
            throw std::runtime_error("IoUring: io_uring_setup failed: " +
                                     std::string(strerror(errno)));
        }

        sq_entries_ = params_.sq_entries;
        cq_entries_ = params_.cq_entries;

        // mmap SQ ring
        size_t sq_ring_sz = params_.sq_off.array + sq_entries_ * sizeof(unsigned);
        sq_ring_ptr_ = mmap(nullptr, sq_ring_sz, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_POPULATE, ring_fd_,
                             IORING_OFF_SQ_RING);
        if (sq_ring_ptr_ == MAP_FAILED) {
            close(ring_fd_);
            throw std::runtime_error("IoUring: mmap SQ ring failed");
        }

        // mmap CQ ring
        size_t cq_ring_sz = params_.cq_off.cqes + cq_entries_ * sizeof(struct io_uring_cqe);
        cq_ring_ptr_ = mmap(nullptr, cq_ring_sz, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, ring_fd_,
                            IORING_OFF_CQ_RING);
        if (cq_ring_ptr_ == MAP_FAILED) {
            munmap(sq_ring_ptr_, sq_ring_sz);
            close(ring_fd_);
            throw std::runtime_error("IoUring: mmap CQ ring failed");
        }

        // mmap SQE array
        size_t sqes_sz = sq_entries_ * sizeof(struct io_uring_sqe);
        sqes_ptr_ = mmap(nullptr, sqes_sz, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, ring_fd_,
                         IORING_OFF_SQES);
        if (sqes_ptr_ == MAP_FAILED) {
            munmap(cq_ring_ptr_, cq_ring_sz);
            munmap(sq_ring_ptr_, sq_ring_sz);
            close(ring_fd_);
            throw std::runtime_error("IoUring: mmap SQEs failed");
        }

        // Setup ring pointer aliases
        sq_head_  = (unsigned*)((char*)sq_ring_ptr_ + params_.sq_off.head);
        sq_tail_  = (unsigned*)((char*)sq_ring_ptr_ + params_.sq_off.tail);
        sq_mask_  = *(unsigned*)((char*)sq_ring_ptr_ + params_.sq_off.ring_mask);
        sq_array_ = (unsigned*)((char*)sq_ring_ptr_ + params_.sq_off.array);

        cq_head_  = (unsigned*)((char*)cq_ring_ptr_ + params_.cq_off.head);
        cq_tail_  = (unsigned*)((char*)cq_ring_ptr_ + params_.cq_off.tail);
        cq_mask_  = *(unsigned*)((char*)cq_ring_ptr_ + params_.cq_off.ring_mask);
        cqes_     = (struct io_uring_cqe*)((char*)cq_ring_ptr_ + params_.cq_off.cqes);

        sqes_ = (struct io_uring_sqe*)sqes_ptr_;

        inflight_ = 0;

        // Allocate aligned buffers for O_DIRECT reads
        // Pool size = sq_entries (can have that many in-flight)
        buffer_pool_size_ = sq_entries_;
        buffer_size_ = 0;  // set by setBufferSize()
        aligned_buffers_.clear();
    }

    ~IoUring() {
        if (sq_ring_ptr_ && sq_ring_ptr_ != MAP_FAILED) {
            size_t sq_ring_sz = params_.sq_off.array + sq_entries_ * sizeof(unsigned);
            munmap(sq_ring_ptr_, sq_ring_sz);
        }
        if (cq_ring_ptr_ && cq_ring_ptr_ != MAP_FAILED) {
            size_t cq_ring_sz = params_.cq_off.cqes + cq_entries_ * sizeof(struct io_uring_cqe);
            munmap(cq_ring_ptr_, cq_ring_sz);
        }
        if (sqes_ptr_ && sqes_ptr_ != MAP_FAILED) {
            size_t sqes_sz = sq_entries_ * sizeof(struct io_uring_sqe);
            munmap(sqes_ptr_, sqes_sz);
        }
        if (ring_fd_ >= 0) {
            close(ring_fd_);
        }
        // Free aligned buffers
        for (void* buf : aligned_buffers_) {
            if (buf) free(buf);
        }
    }

    // Non-copyable
    IoUring(const IoUring&) = delete;
    IoUring& operator=(const IoUring&) = delete;

    // Set buffer size and pre-allocate aligned buffer pool
    // Must be called before submitRead
    void setBufferSize(size_t size) {
        buffer_size_ = size;
        aligned_buffers_.clear();
        free_list_.clear();
        for (size_t i = 0; i < buffer_pool_size_; i++) {
            void* buf = nullptr;
            int ret = posix_memalign(&buf, 512, size);
            if (ret != 0 || !buf) {
                throw std::runtime_error("IoUring: posix_memalign failed for buffer " +
                                         std::to_string(i));
            }
            aligned_buffers_.push_back(buf);
            free_list_.push_back(i);
        }
    }

    // Get a free aligned buffer index from the pool
    // Returns -1 if pool is exhausted
    int allocBuffer() {
        if (free_list_.empty()) return -1;
        int idx = free_list_.back();
        free_list_.pop_back();
        return idx;
    }

    // Return a buffer to the pool
    void freeBuffer(int idx) {
        if (idx >= 0 && (size_t)idx < aligned_buffers_.size()) {
            free_list_.push_back(idx);
        }
    }

    // Get buffer pointer by index
    void* getBuffer(int idx) {
        if (idx < 0 || (size_t)idx >= aligned_buffers_.size()) return nullptr;
        return aligned_buffers_[idx];
    }

    // Submit an async read request
    // fd:       file descriptor (opened with O_DIRECT if needed)
    // offset:   file offset (must be 512-aligned for O_DIRECT)
    // nbytes:   bytes to read (must be 512-aligned for O_DIRECT)
    // buf_idx:  buffer pool index (from allocBuffer)
    // user_data: arbitrary value (block_id typically)
    // Returns: 0 on success, -1 on error (no SQE available)
    int submitRead(int fd, off_t offset, size_t nbytes, int buf_idx, uint64_t user_data) {
        unsigned tail = *sq_tail_;
        unsigned idx = tail & sq_mask_;

        struct io_uring_sqe* sqe = &sqes_[idx];
        memset(sqe, 0, sizeof(*sqe));
        sqe->opcode = IORING_OP_READ;
        sqe->fd = fd;
        sqe->addr = (unsigned long long)aligned_buffers_[buf_idx];
        sqe->len = nbytes;
        sqe->off = offset;
        sqe->user_data = user_data;

        // 写入 SQ array (内核通过 sq_array 查找 SQE)
        sq_array_[idx] = idx;

        // Memory barrier before updating tail
        __sync_synchronize();
        *sq_tail_ = tail + 1;

        inflight_++;
        return 0;
    }

    // Submit all pending SQEs to the kernel
    // Returns number of SQEs submitted
    int submit() {
        // Calculate pending SQEs
        unsigned tail = *sq_tail_;
        unsigned head = *sq_head_;
        unsigned pending = tail - head;
        if (pending == 0) return 0;

        int ret = (int)syscall(__NR_io_uring_enter, ring_fd_, pending, 0, 0, nullptr);
        if (ret < 0) {
            perror("io_uring_enter submit failed");
            return -1;
        }
        return ret;
    }

    // Submit and wait for at least 'wait_nr' completions
    int submitAndWait(unsigned wait_nr) {
        int ret = (int)syscall(__NR_io_uring_enter, ring_fd_,
                               0,           // to_submit=0 means use already-queued SQEs
                               wait_nr,     // min completions to wait for
                               IORING_ENTER_GETEVENTS, nullptr);
        if (ret < 0) {
            perror("io_uring_enter submitAndWait failed");
            return -1;
        }
        return ret;
    }

    // Reap completed CQEs (non-blocking)
    // Fills the provided vector with (user_data, res, buf_idx) tuples
    // Returns number of completions reaped
    struct CqeResult {
        uint64_t user_data;  // block_id typically
        int32_t  res;        // bytes read, or negative error
        // Note: buf_idx must be tracked by the caller via user_data mapping
    };

    int reapCompletions(std::vector<CqeResult>& results) {
        int count = 0;
        unsigned head = *cq_head_;

        while (head != *cq_tail_) {
            unsigned idx = head & cq_mask_;
            struct io_uring_cqe* cqe = &cqes_[idx];

            CqeResult r;
            r.user_data = cqe->user_data;
            r.res = cqe->res;
            results.push_back(r);

            // Memory barrier before updating head
            __sync_synchronize();
            *cq_head_ = head + 1;
            head = *cq_head_;

            count++;
            inflight_--;
        }

        return count;
    }

    // Wait for at least one completion (blocking)
    int waitCompletion() {
        if (inflight_ == 0) return 0;
        return (int)syscall(__NR_io_uring_enter, ring_fd_, 0, 1,
                            IORING_ENTER_GETEVENTS, nullptr);
    }

    // Get number of in-flight requests
    unsigned inflight() const { return inflight_; }

    // Get SQ capacity
    unsigned sqCapacity() const { return sq_entries_; }

private:
    int ring_fd_;
    struct io_uring_params params_;
    unsigned sq_entries_;
    unsigned cq_entries_;

    void* sq_ring_ptr_ = nullptr;
    void* cq_ring_ptr_ = nullptr;
    void* sqes_ptr_ = nullptr;

    // SQ ring pointers
    unsigned* sq_head_ = nullptr;
    unsigned* sq_tail_ = nullptr;
    unsigned  sq_mask_ = 0;
    unsigned* sq_array_ = nullptr;

    // CQ ring pointers
    unsigned* cq_head_ = nullptr;
    unsigned* cq_tail_ = nullptr;
    unsigned  cq_mask_ = 0;
    struct io_uring_cqe* cqes_ = nullptr;

    // SQE array
    struct io_uring_sqe* sqes_ = nullptr;

    // Buffer pool for O_DIRECT
    std::vector<void*> aligned_buffers_;
    std::vector<int> free_list_;
    size_t buffer_pool_size_ = 0;
    size_t buffer_size_ = 0;

    // In-flight counter
    unsigned inflight_ = 0;
};
