// ============================================================================
// 文件: tests/core/test_types.cpp
// 模块: stmb_tests(core 模块单元测试)
// 覆盖范围:
//   1. TimeRange::contains —— 区间内、两端边界(闭区间)、区间外;
//   2. TimeRange::overlaps —— 部分重叠、端点相接、包含、完全分离;
//   3. AABB::contains / intersects —— 盒内/边界/盒外点,相交/面接触/分离,
//      以及负坐标区域的几何判定;
//   4. makeBlockKey —— 格子坐标与时间槽的划分,重点是负坐标/负时间戳下
//      std::floor 的向下取整行为(不能退化为向零取整);
//   5. MemoryBlock 字段 —— 默认值与字段赋值往返;
//   6. BlockKey / CellCoord 的 == 与哈希可直接用于 unordered 容器。
// 测试思路: 每个主题一个测试函数,逐条 assert;Release 构建会定义 NDEBUG
//           使 assert 失效,故在包含 <cassert> 前先 #undef NDEBUG。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "types.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <unordered_map>

namespace {

using namespace stmb;

// 伪代码:
//   1. 构造闭区间 [100,200];
//   2. 断言两个端点与中点被包含,端点两侧紧邻值不被包含;
//   3. 断言与右侧重叠、端点相接、被包含的区间均 overlaps;
//   4. 断言完全在左/右两侧的区间不 overlaps。
void testTimeRange() {
    const TimeRange r{100, 200};
    assert(r.contains(100) && r.contains(150) && r.contains(200));
    assert(!r.contains(99) && !r.contains(201));

    assert(r.overlaps(TimeRange{150, 250}));    // 部分重叠
    assert(r.overlaps(TimeRange{200, 300}));    // 端点相接(闭区间)
    assert(r.overlaps(TimeRange{0, 1000}));     // 被包含
    assert(!r.overlaps(TimeRange{201, 300}));   // 完全在右
    assert(!r.overlaps(TimeRange{0, 99}));      // 完全在左
    std::cout << "[PASS] core: TimeRange contains/overlaps 边界\n";
}

// 伪代码:
//   1. 构造正向盒 [0,10]^3,断言内部点、边界点被包含,外部点不被包含;
//   2. 断言相交盒、面接触盒 intersects 为真,分离盒为假;
//   3. 构造负坐标盒 [-10,-5]^3,重复 contains / intersects 判定,
//      验证负坐标下几何逻辑一致。
void testAabb() {
    const AABB box{{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}};
    assert(box.contains({5.0, 5.0, 5.0}));
    assert(box.contains({0.0, 10.0, 10.0}));    // 边界点算包含
    assert(!box.contains({-0.1, 5.0, 5.0}));
    assert(!box.contains({10.1, 5.0, 5.0}));

    assert(box.intersects(AABB{{5.0, 5.0, 5.0}, {15.0, 15.0, 15.0}}));   // 相交
    assert(box.intersects(AABB{{10.0, 0.0, 0.0}, {20.0, 10.0, 10.0}}));  // 面接触
    assert(!box.intersects(AABB{{10.5, 0.0, 0.0}, {20.0, 10.0, 10.0}})); // 分离

    const AABB neg{{-10.0, -10.0, -10.0}, {-5.0, -5.0, -5.0}};
    assert(neg.contains({-7.5, -7.5, -7.5}));
    assert(!neg.contains({0.0, 0.0, 0.0}));
    assert(neg.intersects(AABB{{-6.0, -6.0, -6.0}, {0.0, 0.0, 0.0}}));
    assert(!neg.intersects(box));
    std::cout << "[PASS] core: AABB contains/intersects 边界与负坐标\n";
}

// 伪代码:
//   1. 以 cellSize=10、timeSlotMs=1000 计算正坐标区域的 BlockKey,
//      断言中心 (25,5,15) 落在格子 (2,0,1)、ts=1500 落在时间槽 1;
//   2. 用负坐标中心 (-5,...) 与负时间戳验证 floor 行为:
//      -5/10 = -0.5 向下取整应为 -1(而非向零取整的 0),
//      -500/1000 = -0.5 向下取整应为 -1;
//   3. 验证恰好落在格子边界上的点归入右侧格子(20/10=2)。
void testMakeBlockKey() {
    const AABB pos{{20.0, 0.0, 10.0}, {30.0, 10.0, 20.0}};  // 中心 (25,5,15)
    const BlockKey k1 = makeBlockKey(pos, 1500, 10.0, 1000);
    assert(k1.cellX == 2 && k1.cellY == 0 && k1.cellZ == 1 && k1.timeSlot == 1);

    const AABB neg{{-10.0, -10.0, -10.0}, {0.0, 0.0, 0.0}};  // 中心 (-5,-5,-5)
    const BlockKey k2 = makeBlockKey(neg, -500, 10.0, 1000);
    assert(k2.cellX == -1 && k2.cellY == -1 && k2.cellZ == -1);  // floor(-0.5) = -1
    assert(k2.timeSlot == -1);                                    // floor(-0.5) = -1

    const AABB edge{{10.0, 0.0, 0.0}, {30.0, 10.0, 10.0}};   // 中心 x=20 恰在格界
    const BlockKey k3 = makeBlockKey(edge, 2000, 10.0, 1000);
    assert(k3.cellX == 2 && k3.timeSlot == 2);
    std::cout << "[PASS] core: makeBlockKey 格子/时间槽划分与负坐标 floor\n";
}

// 伪代码:
//   1. 默认构造 MemoryBlock,断言 id/version/timestamp 为零值;
//   2. 逐字段赋值后逐一读回断言(字段往返);
//   3. 把 BlockKey 与 CellCoord 放入 unordered_map 各存取一次,
//      验证 == 与哈希函数对象可直接用于无序容器。
void testMemoryBlockFields() {
    const MemoryBlock def;
    assert(def.id == 0 && def.version == 0 && def.timestamp == 0);
    assert(def.payload.empty());

    MemoryBlock b;
    b.id = 42;
    b.key = BlockKey{1, 2, 3, 4};
    b.region = AABB{{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
    b.payload = "hello";
    b.timestamp = 1234;
    b.version = 7;
    b.lastAccess = 5678;
    assert(b.id == 42 && (b.key == BlockKey{1, 2, 3, 4}));
    assert(b.payload == "hello" && b.timestamp == 1234);
    assert(b.version == 7 && b.lastAccess == 5678);
    assert(b.region.contains({0.5, 0.5, 0.5}));

    std::unordered_map<BlockKey, int, BlockKeyHash> keyMap;
    keyMap[BlockKey{1, 2, 3, 4}] = 9;
    assert(keyMap.at(BlockKey{1, 2, 3, 4}) == 9);
    assert(keyMap.find(BlockKey{4, 3, 2, 1}) == keyMap.end());

    std::unordered_map<CellCoord, int, CellCoordHash> cellMap;
    cellMap[CellCoord{-1, 0, 1}] = 5;
    assert(cellMap.at(CellCoord{-1, 0, 1}) == 5);
    std::cout << "[PASS] core: MemoryBlock 字段与键的哈希容器适配\n";
}

// 伪代码:
//   1. 以 base=0.8、lastUpdate=0、halfLife=1000ms 验证整倍半衰期衰减:
//      now=1000 -> 0.4,now=2000 -> 0.2(用 1e-9 容差比较浮点);
//   2. elapsed<=0(now == lastUpdate、now < lastUpdate)断言原样返回 base;
//   3. halfLife=0 断言不衰减,任意 now 都返回 base;
//   4. 断言衰减不改写入参语义:多次调用结果一致(纯函数)。
void testDecayedConfidence() {
    const double base = 0.8;
    assert(std::abs(decayedConfidence(base, 0, 1000, 1000) - 0.4) < 1e-9);
    assert(std::abs(decayedConfidence(base, 0, 2000, 1000) - 0.2) < 1e-9);
    assert(std::abs(decayedConfidence(base, 0, 3000, 1000) - 0.1) < 1e-9);

    assert(decayedConfidence(base, 1000, 1000, 1000) == base);   // elapsed == 0
    assert(decayedConfidence(base, 2000, 1000, 1000) == base);   // elapsed < 0
    assert(decayedConfidence(base, 0, 5000, 0) == base);         // halfLife = 0
    assert(decayedConfidence(base, 0, 5000, -1) == base);        // halfLife < 0

    assert(decayedConfidence(base, 0, 1000, 1000) ==
           decayedConfidence(base, 0, 1000, 1000));              // 纯函数
    std::cout << "[PASS] core: decayedConfidence 半衰期衰减与边界\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用五个测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testTimeRange();
    testAabb();
    testMakeBlockKey();
    testMemoryBlockFields();
    testDecayedConfidence();
    std::cout << "ALL TESTS PASS (core)\n";
    return 0;
}
