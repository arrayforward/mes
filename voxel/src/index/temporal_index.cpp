// ============================================================================
// 文件: temporal_index.cpp
// 模块: stmb_index(索引层,依赖 core)
// 用途: 实现 TemporalIndex 的插入、删除、范围查询与过期清理。
// 设计思路:
//   1. insert / remove 是对「时间戳 -> id 集合」桶的普通增删,remove 后空桶即擦除;
//   2. queryRange 利用 std::map 的有序性:lower_bound(start) 找到区间起点,
//      顺序遍历直到键超过 end 为止,沿途把各桶的 id 拼进结果;
//   3. expireBefore 用 lower_bound(ts) 找到第一个「未过期」的桶,
//      把它之前的所有桶整体收集 id 后一次性 erase,实现 O(过期桶数) 的截断。
// 架构角色: TemporalIndex 的唯一实现文件。
// ============================================================================
#include "temporal_index.h"

namespace stmb {

// 伪代码:
//   1. 在 buckets_ 中找到(不存在则自动创建)时间戳 ts 对应的 id 集合;
//   2. 把 id 插入该集合。
void TemporalIndex::insert(TimeStamp ts, BlockId id) {
    buckets_[ts].insert(id);
}

// 伪代码:
//   1. 在 buckets_ 中查找时间戳 ts 的桶,找不到直接返回;
//   2. 从桶中删除 id;
//   3. 若桶变空,把整个桶从 map 中擦除,避免空桶影响范围遍历。
void TemporalIndex::remove(TimeStamp ts, BlockId id) {
    auto it = buckets_.find(ts);
    if (it == buckets_.end()) {
        return;
    }
    it->second.erase(id);
    if (it->second.empty()) {
        buckets_.erase(it);
    }
}

// 伪代码:
//   1. 用 lower_bound(range.start) 定位第一个不落空于区间左侧的桶;
//   2. 从该位置开始顺序遍历,键超过 range.end 即停止;
//   3. 沿途把每个桶的 id 集合追加到结果 vector;
//   4. 返回结果(时间升序排列)。
std::vector<BlockId> TemporalIndex::queryRange(const TimeRange& range) const {
    std::vector<BlockId> result;
    for (auto it = buckets_.lower_bound(range.start);
         it != buckets_.end() && it->first <= range.end; ++it) {
        result.insert(result.end(), it->second.begin(), it->second.end());
    }
    return result;
}

// 伪代码:
//   1. 用 lower_bound(ts) 找到第一个时间戳 >= ts 的桶(即第一个未过期的桶);
//   2. 遍历 [begin, 该位置) 之间的全部桶,把所有 id 收集到结果列表;
//   3. 用 erase(begin, 该位置) 一次性删除这些过期桶;
//   4. 返回被移除的 BlockId 列表,供服务层清理空间索引与存储。
std::vector<BlockId> TemporalIndex::expireBefore(TimeStamp ts) {
    std::vector<BlockId> removed;
    auto firstAlive = buckets_.lower_bound(ts);
    for (auto it = buckets_.begin(); it != firstAlive; ++it) {
        removed.insert(removed.end(), it->second.begin(), it->second.end());
    }
    buckets_.erase(buckets_.begin(), firstAlive);
    return removed;
}

// 伪代码:
//   1. 直接返回 buckets_ 的大小(非空时间戳桶数)。
std::size_t TemporalIndex::bucketCount() const {
    return buckets_.size();
}

}  // namespace stmb
