// ============================================================================
// 文件: temporal_index.h
// 模块: stmb_index(索引层,依赖 core)
// 用途: 声明 TemporalIndex —— 时间索引。
//       内部结构为 std::map<TimeStamp, unordered_set<BlockId>>,利用 map 的
//       有序性支持高效的时间范围查询与「过期清理」(删除某时刻之前的全部条目)。
// 设计思路:
//   1. 同一时间戳可能对应多个块,故 value 是 id 集合;
//   2. 范围查询用 lower_bound / upper_bound 定位区间,遍历拼接结果;
//   3. expireBefore 直接截断 map 前缀,并返回被移除的全部 BlockId,
//      供服务层同步清理空间索引与存储;
//   4. 索引中只存 BlockId,不存块数据。
// 架构角色: 索引层的另一半(时间维度),由 StmbService 持有并维护一致性。
// 与其他模块关系: 依赖 core 的 TimeStamp / TimeRange / BlockId;线程安全由
//                 服务层的统一互斥锁保证,本类自身不加锁。
// ============================================================================
#pragma once

#include "types.h"

#include <map>
#include <unordered_set>
#include <vector>

namespace stmb {

class TemporalIndex {
public:
    // 在时间戳 ts 的桶中登记 id
    void insert(TimeStamp ts, BlockId id);

    // 从时间戳 ts 的桶中移除 id(空桶一并销毁)
    void remove(TimeStamp ts, BlockId id);

    // 返回时间范围 range 内的全部 BlockId(按时间升序拼接)
    std::vector<BlockId> queryRange(const TimeRange& range) const;

    // 删除时间戳严格小于 ts 的全部条目,返回被移除的 BlockId 列表
    std::vector<BlockId> expireBefore(TimeStamp ts);

    // 返回当前不同时间戳桶的数量(测试/观测用)
    std::size_t bucketCount() const;

private:
    std::map<TimeStamp, std::unordered_set<BlockId>> buckets_;  // 时间戳 -> 块 id 集合(按时间有序)
};

}  // namespace stmb
