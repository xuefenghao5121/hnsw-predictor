# Conflict: FLAG_VEC_ONLY 定义位置与使用不一致

## 发现 {#CONFLICT-001}
<!-- ndf: kind=info layer=L2 status=draft since=0.1 source=observed -->

**冲突描述**: `FLAG_VEC_ONLY = 0x02` 在 `common.h:79` 定义为 BlockHeader flags 字段的位标志。但在 `block_cache.cpp:401-403` 的 `parseBlock()` 中，用 `uint32_t` 读取 base+12 处的 flags 字段并检查 `FLAG_VEC_ONLY` 位：

```cpp
uint32_t veconly_flags;
std::memcpy(&veconly_flags, base + 12, sizeof(uint32_t));
bool is_veconly = (veconly_flags & FLAG_VEC_ONLY) != 0;
```

**矛盾点**: BlockHeader 的 flags 字段定义为 `uint8_t flags`（`common.h:71`），位于结构体偏移 16。但 `parseBlock()` 在 offset 12 处读 `uint32_t` 作为 flags check，这意味着读取的是 `adj_offset` 字段（u32 at offset 12）的高位字节。

**推测**: VecOnlyHeader 格式可能不同于 BlockHeader——它的 layout 是 `[block_id:u32][node_count:u32][data_offset:u32][flags:u32]`（16 字节头），而非 BlockHeader 的 `[block_id][node_count][data_offset][adj_offset][flags:u8][pad:7]`（24 字节头）。`parseBlock()` 先检测是否为 vec-only，再 fallback 到标准 BlockHeader 解析。这个双路径设计本身是正确的，但 flags 字段的语义重叠（`FLAG_VEC_ONLY` 值与标准 BlockHeader 的 `adj_offset` 低 2 位重叠）导致缺乏明确性。

**影响**: 如果标准 BlockHeader 的 `adj_offset` 字段低 2 位恰好为 `0x02`，会被误判为 vec-only block。在 SIFT1M 数据上，vec-only 文件 `adj_offset` 通常不为 0x02，但这是在数据上的巧合性正确而非设计保证。

**建议**: VecOnly 应该用独立的 magic number 或更明确的 header 格式区分，而非依赖 flags 位检测。参见 `spec/open/conflict-vec-only-detection.md`。
