# Conflict: BLOCKS_FILE_HEADER_SIZE 语义不一致

## 发现 {#CONFLICT-002}
<!-- ndf: kind=info layer=L2 status=draft since=0.1 source=observed -->

**冲突描述**: `BLOCKS_FILE_HEADER_SIZE` 定义为 `4096`（`common.h:31`），注释为 "Blocks 文件头部保留大小（4096 字节，O_DIRECT 对齐）。实际 BlocksFileHeader 只有 16 字节，但文件中保留 4096 字节"。

**但代码中**:
- `disk_hnsw.cpp:1725` 中 Fine Rerank vecblocks 偏移计算用硬编码 `4096ull`：
  ```cpp
  uint64_t off = 4096ull + (uint64_t)b * vec_block_size_ + ...
  ```
- `block_cache.cpp:350` 用 `BLOCKS_FILE_HEADER_SIZE`：
  ```cpp
  off_t offset = (off_t)BLOCKS_FILE_HEADER_SIZE + (off_t)block_id * block_size_;
  ```
- `block_cache.cpp:276` 的 `getHeaderSize()` 也返回 `BLOCKS_FILE_HEADER_SIZE`

**矛盾点**: Fine Rerank 路径硬编码 `4096ull`，而 BlockCache 路径使用 `BLOCKS_FILE_HEADER_SIZE` 常量。值相同（都是 4096），但在语义上：如果未来改变 BLOCKS_FILE_HEADER_SIZE，Fine Rerank 代码不会自动跟随。

**影响**: 低风险。当前值一致，但代码脆弱——硬编码 4096 在多处重复出现。

**建议**: Fine Rerank 的偏移计算应引用 `BlockCache::getHeaderSize()` 或定义为独立常量 `VEC_BLOCKS_FILE_HEADER_SIZE`。

> **已知技术债务**: 此硬编码违反 [[CHR-005]] #5 声称的"每个优化 MUST 有
> benchmark 数据支撑"的设计纪律。纪律与实现的差距已确认，记录为待修复债务，
> 不影响当前功能正确性（值一致），但在 `BLOCKS_FILE_HEADER_SIZE` 变更时会触发。
