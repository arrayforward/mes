// ============================================================================
// 文件: spatial_grid.cpp
// 模块: stmb_index(索引层,依赖 core)
// 用途: 实现 SpatialGridIndex 的插入、删除与区域粗筛查询。
// 设计思路:
//   1. 核心是一个私有辅助函数 cellsCovered:对 AABB 的每个轴,
//      用 floor(min/cellSize) .. floor(max/cellSize) 得到覆盖的格子编号区间,
//      三重循环枚举出全部格子;
//   2. insert / remove / queryRegion 都只是「先离散化、再对倒排表做集合操作」;
//   3. 格子编号区间采用闭区间,max 恰好落在格子边界时会多算一个相邻格子,
//      这对粗筛是安全的(只会多给候选,不会漏),精过滤由服务层完成。
// 架构角色: SpatialGridIndex 的唯一实现文件。
// ============================================================================
#include "spatial_grid.h"

#include <cmath>

namespace stmb {

// 伪代码:
//   1. 保存 cellSize 到成员变量(构造函数体为空,仅做成员初始化)。
SpatialGridIndex::SpatialGridIndex(double cellSize)
    : cellSize_(cellSize) {}

// 伪代码:
//   1. 对 x/y/z 三个轴,分别计算 floor(min/cellSize) 与 floor(max/cellSize),
//      得到该轴覆盖的格子编号闭区间 [lo, hi];
//   2. 三重循环枚举三个轴区间内的所有组合,生成 CellCoord;
//   3. 把所有格子坐标放入 vector 返回。
std::vector<CellCoord> SpatialGridIndex::cellsCovered(const AABB& region) const {
    std::vector<CellCoord> result;
    std::int64_t lo[3];
    std::int64_t hi[3];
    for (std::size_t axis = 0; axis < 3; ++axis) {
        lo[axis] = static_cast<std::int64_t>(std::floor(region.min[axis] / cellSize_));
        hi[axis] = static_cast<std::int64_t>(std::floor(region.max[axis] / cellSize_));
    }
    for (std::int64_t x = lo[0]; x <= hi[0]; ++x) {
        for (std::int64_t y = lo[1]; y <= hi[1]; ++y) {
            for (std::int64_t z = lo[2]; z <= hi[2]; ++z) {
                result.push_back(CellCoord{x, y, z});
            }
        }
    }
    return result;
}

// 伪代码:
//   1. 调用 cellsCovered 得到 region 覆盖的全部格子;
//   2. 对每个格子,在 cells_ 中取出(不存在则自动创建)对应的 id 集合;
//   3. 把 id 插入每个集合。
void SpatialGridIndex::insert(BlockId id, const AABB& region) {
    for (const CellCoord& cell : cellsCovered(region)) {
        cells_[cell].insert(id);
    }
}

// 伪代码:
//   1. 调用 cellsCovered 得到 region 覆盖的全部格子;
//   2. 对每个格子:在 cells_ 中查找,找不到则跳过;
//   3. 从该格子的 id 集合中删除 id;
//   4. 若集合因此变空,把整个格子从 map 中擦除,避免空格子堆积。
void SpatialGridIndex::remove(BlockId id, const AABB& region) {
    for (const CellCoord& cell : cellsCovered(region)) {
        auto it = cells_.find(cell);
        if (it == cells_.end()) {
            continue;
        }
        it->second.erase(id);
        if (it->second.empty()) {
            cells_.erase(it);
        }
    }
}

// 伪代码:
//   1. 调用 cellsCovered 得到查询区域覆盖的全部格子;
//   2. 准备一个空的结果集合;
//   3. 对每个格子:在 cells_ 中查找,若存在则把其 id 集合整体并入结果集;
//   4. 返回并集(候选集合,可能含不相交的块,精过滤由服务层负责)。
std::unordered_set<BlockId> SpatialGridIndex::queryRegion(const AABB& region) const {
    std::unordered_set<BlockId> result;
    for (const CellCoord& cell : cellsCovered(region)) {
        auto it = cells_.find(cell);
        if (it != cells_.end()) {
            result.insert(it->second.begin(), it->second.end());
        }
    }
    return result;
}

// 伪代码:
//   1. 直接返回 cells_ 的大小(非空格子数)。
std::size_t SpatialGridIndex::cellCount() const {
    return cells_.size();
}

}  // namespace stmb
