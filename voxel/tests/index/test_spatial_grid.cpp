// ============================================================================
// 文件: tests/index/test_spatial_grid.cpp
// 模块: stmb_tests(SpatialGridIndex 单元测试)
// 覆盖范围:
//   1. insert / queryRegion 基本命中与未命中;
//   2. 跨区域块:AABB 覆盖多个格子时,在其覆盖的每个格子都能被粗筛命中,
//      且 cellCount 与覆盖格子数一致;
//   3. remove 后不再命中,空格子被销毁(cellCount 回落);
//   4. 空区域查询(索引内无任何块的区域)返回空集合;
//   5. 粗筛语义:查询区域与块同格但不一定精确相交时,候选可以偏多
//      (精过滤是服务层职责,本层只保证不漏)。
// 测试思路: 每个主题一个测试函数,逐条 assert;#undef NDEBUG 保证
//           Release 构建下断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "spatial_grid.h"

#include <cassert>
#include <iostream>

namespace {

using namespace stmb;

// 伪代码:
//   1. 构造格子边长 10 的索引,插入两个相距很远的单格块 id1、id2;
//   2. 查询 id1 所在格:断言含 id1 不含 id2;
//   3. 查询 id2 所在格:断言含 id2 不含 id1;
//   4. 断言此时 cellCount 为 2。
void testInsertAndQuery() {
    SpatialGridIndex index(10.0);
    index.insert(1, AABB{{1.0, 1.0, 1.0}, {5.0, 5.0, 5.0}});          // 格子 (0,0,0)
    index.insert(2, AABB{{100.0, 100.0, 100.0}, {105.0, 105.0, 105.0}});  // 格子 (10,10,10)

    const auto hit1 = index.queryRegion(AABB{{0.0, 0.0, 0.0}, {9.0, 9.0, 9.0}});
    assert(hit1.count(1) == 1 && hit1.count(2) == 0);
    const auto hit2 = index.queryRegion(AABB{{99.0, 99.0, 99.0}, {106.0, 106.0, 106.0}});
    assert(hit2.count(2) == 1 && hit2.count(1) == 0);
    assert(index.cellCount() == 2);
    std::cout << "[PASS] spatial_grid: insert / queryRegion 基本命中\n";
}

// 伪代码:
//   1. 插入一个覆盖 [5,15]^3 的大块 id1(每轴跨格子 0 和 1,共 8 格);
//   2. 断言 cellCount == 8;
//   3. 分别查询格子 (0,0,0) 与 (1,1,1) 内的小区域,断言都能粗筛命中 id1;
//   4. 查询只覆盖部分格子的细长区域,断言候选仍包含 id1(不漏)。
void testMultiCellBlock() {
    SpatialGridIndex index(10.0);
    index.insert(1, AABB{{5.0, 5.0, 5.0}, {15.0, 15.0, 15.0}});  // 跨 2x2x2 = 8 格
    assert(index.cellCount() == 8);

    assert(index.queryRegion(AABB{{0.0, 0.0, 0.0}, {9.0, 9.0, 9.0}}).count(1) == 1);
    assert(index.queryRegion(AABB{{11.0, 11.0, 11.0}, {19.0, 19.0, 19.0}}).count(1) == 1);
    assert(index.queryRegion(AABB{{16.0, 0.0, 0.0}, {18.0, 2.0, 2.0}}).count(1) == 1);
    std::cout << "[PASS] spatial_grid: 跨区域块命中多个 cell\n";
}

// 伪代码:
//   1. 插入块 id1、id2;
//   2. 删除 id1 后查询其原区域:断言不再命中;
//   3. 断言空格子被销毁,cellCount 回落到 id2 独占的格子数;
//   4. 删除 id2 后断言 cellCount 归零。
void testRemove() {
    SpatialGridIndex index(10.0);
    const AABB r1{{1.0, 1.0, 1.0}, {5.0, 5.0, 5.0}};
    const AABB r2{{100.0, 100.0, 100.0}, {105.0, 105.0, 105.0}};
    index.insert(1, r1);
    index.insert(2, r2);
    assert(index.cellCount() == 2);

    index.remove(1, r1);
    assert(index.queryRegion(AABB{{0.0, 0.0, 0.0}, {9.0, 9.0, 9.0}}).empty());
    assert(index.cellCount() == 1);

    index.remove(2, r2);
    assert(index.cellCount() == 0);
    std::cout << "[PASS] spatial_grid: remove 后不再命中且空格子销毁\n";
}

// 伪代码:
//   1. 对空索引直接查询:断言返回空集合;
//   2. 插入块后查询与其完全无关的远方区域:断言返回空集合;
//   3. 重复删除同一个不存在的 id/区域组合:断言不会崩溃且状态不变。
void testEmptyRegionQuery() {
    SpatialGridIndex index(10.0);
    assert(index.queryRegion(AABB{{0.0, 0.0, 0.0}, {9.0, 9.0, 9.0}}).empty());

    index.insert(1, AABB{{1.0, 1.0, 1.0}, {5.0, 5.0, 5.0}});
    assert(index.queryRegion(AABB{{50.0, 50.0, 50.0}, {60.0, 60.0, 60.0}}).empty());

    index.remove(99, AABB{{500.0, 500.0, 500.0}, {510.0, 510.0, 510.0}});
    assert(index.cellCount() == 1);
    std::cout << "[PASS] spatial_grid: 空区域查询与无效删除\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用四个测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testInsertAndQuery();
    testMultiCellBlock();
    testRemove();
    testEmptyRegionQuery();
    std::cout << "ALL TESTS PASS (spatial_grid)\n";
    return 0;
}
