// ============================================================================
// 文件: spatial_grid.h
// 模块: stmb_index(索引层,依赖 core)
// 用途: 声明 SpatialGridIndex —— 空间格子索引。
//       内部结构为 unordered_map<CellCoord, unordered_set<BlockId>> 的倒排表:
//       每个格子映射到「区域覆盖该格子」的所有记忆块 id。
// 设计思路:
//   1. 写入/删除时,把 AABB 按 cellSize 离散化为它覆盖的所有格子,逐格增删 id;
//   2. 查询时,同样把查询区域离散化为格子集合,取这些格子 id 集合的并集作为候选;
//   3. 索引只做粗筛,精确相交判定由服务层用 AABB::intersects 完成;
//   4. 索引中只存 BlockId,不存块数据,数据唯一副本在 BlockStore。
// 架构角色: 索引层的一半(空间维度),由 StmbService 持有并维护一致性。
// 与其他模块关系: 依赖 core 的 AABB / CellCoord / BlockId;线程安全由服务层
//                 的统一互斥锁保证,本类自身不加锁。
// ============================================================================
#pragma once

#include "types.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stmb {

class SpatialGridIndex {
public:
    // 构造:指定空间格子边长(必须与生成 BlockKey 时使用的 cellSize 一致)
    explicit SpatialGridIndex(double cellSize);

    // 把 id 登记到 region 覆盖的所有格子中
    void insert(BlockId id, const AABB& region);

    // 把 id 从 region 覆盖的所有格子中移除(空格子一并销毁)
    void remove(BlockId id, const AABB& region);

    // 粗筛查询:返回 region 覆盖格子中出现过的所有候选 BlockId(并集)
    std::unordered_set<BlockId> queryRegion(const AABB& region) const;

    // 返回当前非空格子数量(测试/观测用)
    std::size_t cellCount() const;

private:
    // 计算 region 覆盖的全部格子坐标
    std::vector<CellCoord> cellsCovered(const AABB& region) const;

    double cellSize_;  // 格子边长
    std::unordered_map<CellCoord, std::unordered_set<BlockId>, CellCoordHash> cells_;  // 格子 -> 块 id 集合
};

}  // namespace stmb
