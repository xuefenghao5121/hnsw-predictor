// csr_cache.h - CSR 分页缓存 (P3: CSR 上磁盘)
//
// 将 delta+varint 压缩的 CSR 邻接表数据存储在磁盘上,
// 按需分页加载到内存, 替代全量常驻。
//
// 内存: byte_offsets_ (N+1 × 4B) 常驻 + page cache (可配置)
// 磁盘: compact CSR 数据文件
//
// 设计: P3-ARCH-001, P3-ARCH-002, P3-BEH-001
// 线程安全: spinlock 保护 page cache, 读命中无锁快路径可选

#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <unordered_map>
#include <mutex>
#include <cstring>
#include <iostream>
#include <atomic>
#include <shared_mutex>

class CSRCache {
public:
    // 从已有内存数据构建磁盘文件
    CSRCache(const std::vector<uint8_t>& compact_data,
             const std::vector<uint32_t>& byte_offsets,
             size_t cache_bytes,
             uint32_t page_size = 4096)
        : byte_offsets_(byte_offsets)
        , page_size_(page_size)
        , cache_bytes_(cache_bytes)
    {
        std::string tmp_path = "/tmp/diskhnsw_csr_pages.bin";
        int wfd = open(tmp_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (wfd < 0) throw std::runtime_error("CSRCache: cannot create temp file");
        ssize_t written = write(wfd, compact_data.data(), compact_data.size());
        close(wfd);
        if (written != (ssize_t)compact_data.size())
            throw std::runtime_error("CSRCache: short write");

        total_bytes_ = compact_data.size();
        num_pages_ = (total_bytes_ + page_size_ - 1) / page_size_;
        max_cached_pages_ = std::min((size_t)num_pages_, cache_bytes_ / page_size_);

        fd_ = open(tmp_path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("CSRCache: cannot open temp file");

        // 预分配缓存槽
        page_slots_.resize(max_cached_pages_);
        slot_map_.reserve(max_cached_pages_ * 2);

        total_reads_ = 0;
        cache_hits_ = 0;
        cache_misses_ = 0;

        std::cout << "[CSRCache] Init: " << total_bytes_ / (1024.0*1024) << "MB on disk, "
                  << num_pages_ << " pages, cache=" << cache_bytes_ / (1024.0*1024)
                  << "MB (" << max_cached_pages_ << " pages, "
                  << (100.0 * max_cached_pages_ / num_pages_) << "% coverage)" << std::endl;
    }

    ~CSRCache() {
        if (fd_ >= 0) close(fd_);
    }

    struct CSRBytes {
        const uint8_t* data;
        size_t length;
    };

    // 获取节点的 CSR 原始字节 (用于 varint 解码)
    CSRBytes getNodeBytes(uint32_t node_id) {
        uint32_t byte_start = byte_offsets_[node_id];
        uint32_t byte_end = byte_offsets_[node_id + 1];
        size_t length = byte_end - byte_start;
        if (length == 0) return {nullptr, 0};

        uint32_t page_start = byte_start / page_size_;
        uint32_t page_end = (byte_end - 1) / page_size_;

        if (page_start == page_end) {
            // 单页 - 最常见路径
            const uint8_t* page_data = getPage(page_start);
            uint32_t offset_in_page = byte_start % page_size_;
            return {page_data + offset_in_page, length};
        }

        // 跨页 - 拼接到临时缓冲区
        thread_local std::vector<uint8_t> tl_buf;
        tl_buf.clear();
        tl_buf.reserve(length);
        for (uint32_t pg = page_start; pg <= page_end; pg++) {
            const uint8_t* pd = getPage(pg);
            uint32_t cs = (pg == page_start) ? (byte_start % page_size_) : 0;
            uint32_t ce = (pg == page_end) ? ((byte_end - 1) % page_size_ + 1) : page_size_;
            tl_buf.insert(tl_buf.end(), pd + cs, pd + ce);
        }
        return {tl_buf.data(), length};
    }

    void printStats() const {
        std::cout << "[CSRCache] reads=" << total_reads_
                  << " hits=" << cache_hits_
                  << " misses=" << cache_misses_
                  << " hit_rate=" << (total_reads_ > 0 ? (100.0 * cache_hits_ / total_reads_) : 0) << "%"
                  << " cached=" << num_cached_pages_.load() << "/" << num_pages_
                  << std::endl;
    }

    double getHitRate() const {
        return total_reads_ > 0 ? (double)cache_hits_ / total_reads_ : 0;
    }

private:
    // 获取一个页, 优先从缓存, 未命中则 pread
    const uint8_t* getPage(uint32_t page_id) {
        total_reads_++;

        // 快路径: 查缓存 (shared lock)
        {
            std::shared_lock lock(mtx_);
            auto it = slot_map_.find(page_id);
            if (it != slot_map_.end()) {
                page_slots_[it->second].access_epoch = ++access_counter_;
                cache_hits_++;
                return page_slots_[it->second].data.data();
            }
        }

        // 慢路径: 未命中 - pread (无锁)
        std::vector<uint8_t> buf(page_size_);
        off_t offset = (off_t)page_id * page_size_;
        ssize_t n = pread(fd_, buf.data(), page_size_, offset);
        if (n <= 0) throw std::runtime_error("CSRCache: pread failed page " + std::to_string(page_id));
        if ((size_t)n < page_size_) buf.resize(n);

        cache_misses_++;

        // 插入缓存 (exclusive lock)
        {
            std::unique_lock lock(mtx_);

            // 双检查: 可能已被其他线程加载
            auto it = slot_map_.find(page_id);
            if (it != slot_map_.end()) {
                page_slots_[it->second].access_epoch = ++access_counter_;
                // cache_hits_++; // 不计, 避免重复
                return page_slots_[it->second].data.data();
            }

            // 找空槽或淘汰 LRU
            int slot = findFreeSlot();

            page_slots_[slot].page_id = page_id;
            page_slots_[slot].data = std::move(buf);
            page_slots_[slot].access_epoch = ++access_counter_;
            page_slots_[slot].valid = true;
            slot_map_[page_id] = slot;
            num_cached_pages_++;

            return page_slots_[slot].data.data();
        }
    }

    int findFreeSlot() {
        // 找空闲槽
        for (int i = 0; i < (int)page_slots_.size(); i++) {
            if (!page_slots_[i].valid) return i;
        }

        // 淘汰 LRU
        int lru_slot = 0;
        uint64_t lru_time = UINT64_MAX;
        for (int i = 0; i < (int)page_slots_.size(); i++) {
            if (page_slots_[i].access_epoch < lru_time) {
                lru_time = page_slots_[i].access_epoch;
                lru_slot = i;
            }
        }
        slot_map_.erase(page_slots_[lru_slot].page_id);
        page_slots_[lru_slot].valid = false;
        page_slots_[lru_slot].data.clear();
        page_slots_[lru_slot].data.shrink_to_fit();
        num_cached_pages_--;
        return lru_slot;
    }

    // 数据
    int fd_ = -1;
    const std::vector<uint32_t>& byte_offsets_;
    size_t total_bytes_;
    uint32_t page_size_;
    uint32_t num_pages_;
    size_t cache_bytes_;
    size_t max_cached_pages_;

    // 页缓存 (slot-based LRU)
    struct PageSlot {
        uint32_t page_id = 0;
        std::vector<uint8_t> data;
        uint64_t access_epoch = 0;
        bool valid = false;
    };
    std::vector<PageSlot> page_slots_;
    std::unordered_map<uint32_t, int> slot_map_;  // page_id -> slot
    std::atomic<int> num_cached_pages_{0};

    // 线程安全
    std::shared_mutex mtx_;
    std::atomic<uint64_t> access_counter_{0};

    // 统计
    std::atomic<uint64_t> total_reads_{0};
    std::atomic<uint64_t> cache_hits_{0};
    std::atomic<uint64_t> cache_misses_{0};
};
