// ============================================================================
// 文件: types.cpp
// 模块: stmb_core(核心数据类型,无依赖)
// 用途: 实现 types.h 中声明的几何判定、相等比较、哈希与 BlockKey 计算。
// 设计思路:
//   1. 几何判定采用标准区间/AABB 算法,边界一律按「闭区间」处理(相接即相交);
//   2. 哈希使用 boost 风格的 hash_combine,把多个整型字段混合成一个 size_t;
//   3. 格子/时间槽编号使用向下取整除法(std::floor),保证负数坐标也正确分桶。
// 架构角色: stmb_core 的唯一实现文件,被所有上层模块链接。
// ============================================================================
#include "types.h"

#include <cmath>

namespace stmb {
namespace {

// 伪代码:
//   1. 把 v 以 size_t 形式混入种子 seed( boost hash_combine 经典公式 );
//   2. 通过引用直接更新 seed,供连续多次调用。
void hashCombine(std::size_t& seed, std::size_t v) {
    seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
}

}  // namespace

// 伪代码:
//   1. 比较 t 是否满足 start <= t <= end;
//   2. 返回比较结果(闭区间,边界算作包含)。
bool TimeRange::contains(TimeStamp t) const {
    return start <= t && t <= end;
}

// 伪代码:
//   1. 两个区间不相交当且仅当一个完全在另一个左侧(start > other.end)
//      或完全在右侧(end < other.start);
//   2. 对「不相交」取反即得「相交」。
bool TimeRange::overlaps(const TimeRange& other) const {
    return !(start > other.end || end < other.start);
}

// 伪代码:
//   1. 依次检查 p 在 x/y/z 三个轴上的分量;
//   2. 任一分量小于 min 或大于 max,立即返回 false;
//   3. 三个轴都通过则返回 true。
bool AABB::contains(const std::array<double, 3>& p) const {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (p[axis] < min[axis] || p[axis] > max[axis]) {
            return false;
        }
    }
    return true;
}

// 伪代码:
//   1. 对 x/y/z 三个轴分别做区间相交判定;
//   2. 任一轴上本盒 max < 对方 min 或本盒 min > 对方 max,则两盒分离,返回 false;
//   3. 三个轴都有重叠则返回 true(边界相接视为相交)。
bool AABB::intersects(const AABB& other) const {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (max[axis] < other.min[axis] || min[axis] > other.max[axis]) {
            return false;
        }
    }
    return true;
}

// 伪代码:
//   1. 对 x/y/z 三个轴分别取 (min + max) / 2;
//   2. 组装成数组返回。
std::array<double, 3> AABB::center() const {
    std::array<double, 3> c{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        c[axis] = (min[axis] + max[axis]) / 2.0;
    }
    return c;
}

// 伪代码:
//   1. 逐字段比较 x、y、z;
//   2. 全部相等返回 true,否则 false。
bool CellCoord::operator==(const CellCoord& other) const {
    return x == other.x && y == other.y && z == other.z;
}

// 伪代码:
//   1. 以 0 为初始种子;
//   2. 依次把 x、y、z 混入种子;
//   3. 返回最终种子作为哈希值。
std::size_t CellCoordHash::operator()(const CellCoord& c) const {
    std::size_t seed = 0;
    hashCombine(seed, static_cast<std::size_t>(c.x));
    hashCombine(seed, static_cast<std::size_t>(c.y));
    hashCombine(seed, static_cast<std::size_t>(c.z));
    return seed;
}

// 伪代码:
//   1. 逐字段比较 cellX、cellY、cellZ、timeSlot;
//   2. 全部相等返回 true,否则 false。
bool BlockKey::operator==(const BlockKey& other) const {
    return cellX == other.cellX && cellY == other.cellY &&
           cellZ == other.cellZ && timeSlot == other.timeSlot;
}

// 伪代码:
//   1. 以 0 为初始种子;
//   2. 依次混入 cellX、cellY、cellZ、timeSlot;
//   3. 返回最终种子作为哈希值。
std::size_t BlockKeyHash::operator()(const BlockKey& k) const {
    std::size_t seed = 0;
    hashCombine(seed, static_cast<std::size_t>(k.cellX));
    hashCombine(seed, static_cast<std::size_t>(k.cellY));
    hashCombine(seed, static_cast<std::size_t>(k.cellZ));
    hashCombine(seed, static_cast<std::size_t>(k.timeSlot));
    return seed;
}

// 伪代码:
//   1. 取区域中心点 center;
//   2. 中心点各分量除以 cellSize 并向下取整,得到格子坐标(兼容负坐标);
//   3. 时间戳除以 timeSlotMs 并向下取整,得到时间槽编号;
//   4. 组装成 BlockKey 返回。
BlockKey makeBlockKey(const AABB& region, TimeStamp timestamp,
                      double cellSize, TimeStamp timeSlotMs) {
    const std::array<double, 3> c = region.center();
    BlockKey key;
    key.cellX = static_cast<std::int64_t>(std::floor(c[0] / cellSize));
    key.cellY = static_cast<std::int64_t>(std::floor(c[1] / cellSize));
    key.cellZ = static_cast<std::int64_t>(std::floor(c[2] / cellSize));
    key.timeSlot = static_cast<std::int64_t>(
        std::floor(static_cast<double>(timestamp) / static_cast<double>(timeSlotMs)));
    return key;
}

// 伪代码:
//   1. 逐字段比较 sx、sy、sz、tBucket;
//   2. 全部相等返回 true,否则 false。
bool ShardKey::operator==(const ShardKey& other) const {
    return sx == other.sx && sy == other.sy &&
           sz == other.sz && tBucket == other.tBucket && level == other.level;
}

// 伪代码:
//   1. 以 0 为初始种子;
//   2. 依次混入 sx、sy、sz、tBucket、level;
//   3. 返回最终种子作为哈希值。
std::size_t ShardKeyHash::operator()(const ShardKey& k) const {
    std::size_t seed = 0;
    hashCombine(seed, static_cast<std::size_t>(k.sx));
    hashCombine(seed, static_cast<std::size_t>(k.sy));
    hashCombine(seed, static_cast<std::size_t>(k.sz));
    hashCombine(seed, static_cast<std::size_t>(k.tBucket));
    hashCombine(seed, static_cast<std::size_t>(k.level));
    return seed;
}

// 伪代码:
//   1. 取区域中心点 center;
//   2. 中心点各分量除以 shardCellSize 并向下取整,得到空间分片坐标;
//   3. 时间戳除以 shardTimeSpanMs 并向下取整,得到时间桶编号;
//   4. 附上层级维度,组装成 ShardKey 返回。
ShardKey makeShardKey(const AABB& region, TimeStamp timestamp,
                      double shardCellSize, TimeStamp shardTimeSpanMs, int level) {
    const std::array<double, 3> c = region.center();
    ShardKey key;
    key.sx = static_cast<std::int64_t>(std::floor(c[0] / shardCellSize));
    key.sy = static_cast<std::int64_t>(std::floor(c[1] / shardCellSize));
    key.sz = static_cast<std::int64_t>(std::floor(c[2] / shardCellSize));
    key.tBucket = static_cast<std::int64_t>(
        std::floor(static_cast<double>(timestamp) / static_cast<double>(shardTimeSpanMs)));
    key.level = level;
    return key;
}

// 伪代码:
//   1. 按枚举值 switch 返回对应字符串字面量;
//   2. 兜底返回 "Unknown"(防御未来新增枚举值)。
const char* toString(ChangeType type) {
    switch (type) {
        case ChangeType::Emergence: return "Emergence";
        case ChangeType::Vanishing: return "Vanishing";
        case ChangeType::Mutation:  return "Mutation";
        case ChangeType::Seasonal:  return "Seasonal";
        case ChangeType::Transient: return "Transient";
        case ChangeType::Conflict:  return "Conflict";
    }
    return "Unknown";
}

// 伪代码:
//   1. 按枚举值 switch 返回对应字符串字面量;
//   2. 兜底返回 "Unknown"(防御未来新增枚举值)。
const char* toString(BlockState state) {
    switch (state) {
        case BlockState::Pending:  return "Pending";
        case BlockState::Stable:   return "Stable";
        case BlockState::Changing: return "Changing";
    }
    return "Unknown";
}

// 伪代码:
//   1. 计算 elapsed = now - lastUpdate;
//   2. 若 halfLifeMs <= 0(不衰减)或 elapsed <= 0(未经过时间),直接返回 base;
//   3. 否则返回 base * 0.5^(elapsed/halfLifeMs)(std::pow 计算指数衰减)。
double decayedConfidence(double base, TimeStamp lastUpdate, TimeStamp now,
                         std::int64_t halfLifeMs) {
    const TimeStamp elapsed = now - lastUpdate;
    if (halfLifeMs <= 0 || elapsed <= 0) {
        return base;
    }
    return base * std::pow(0.5, static_cast<double>(elapsed) /
                                static_cast<double>(halfLifeMs));
}

}  // namespace stmb
