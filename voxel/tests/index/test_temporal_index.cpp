// ============================================================================
// 文件: tests/index/test_temporal_index.cpp
// 模块: stmb_tests(TemporalIndex 单元测试)
// 覆盖范围:
//   1. insert / queryRange 基本查询;
//   2. queryRange 的闭区间边界:恰好等于 start / end 的时间戳必须命中,
//      紧邻区间外的时间戳必须排除;
//   3. 重复时间戳:同一时间戳登记多个块,全部可查出;
//      remove 其中一个后桶保留,删空后桶销毁;
//   4. expireBefore:返回并删除「严格小于」阈值的全部 BlockId,
//      等于阈值的块保留,删除后索引状态一致。
// 测试思路: 每个主题一个测试函数,逐条 assert;#undef NDEBUG 保证
//           Release 构建下断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "temporal_index.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

namespace {

using namespace stmb;

// 伪代码:
//   1. 对 id 列表排序,便于与期望值做集合比较;
//   2. 断言排序后与期望列表完全相等。
void assertSameIds(std::vector<BlockId> actual, std::vector<BlockId> expected) {
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    assert(actual == expected);
}

// 伪代码:
//   1. 插入 ts=100/200/300 三个块;
//   2. 查询闭区间 [100,300]:断言三个全部命中;
//   3. 查询 [150,250]:断言只命中 ts=200 的块;
//   4. 查询 [0,99] 与 [301,400]:断言均为空。
void testInsertAndQueryRange() {
    TemporalIndex index;
    index.insert(100, 1);
    index.insert(200, 2);
    index.insert(300, 3);

    assertSameIds(index.queryRange(TimeRange{100, 300}), {1, 2, 3});
    assertSameIds(index.queryRange(TimeRange{150, 250}), {2});
    assert(index.queryRange(TimeRange{0, 99}).empty());
    assert(index.queryRange(TimeRange{301, 400}).empty());
    std::cout << "[PASS] temporal_index: insert / queryRange 基本查询\n";
}

// 伪代码:
//   1. 插入 ts=100 与 ts=200 两个块;
//   2. 查询 [100,200]:断言端点值双双命中(闭区间);
//   3. 查询 [101,199]:断言两个块都被排除;
//   4. 查询单点区间 [100,100] / [200,200]:断言各自只命中端点块。
void testRangeBoundaries() {
    TemporalIndex index;
    index.insert(100, 1);
    index.insert(200, 2);

    assertSameIds(index.queryRange(TimeRange{100, 200}), {1, 2});
    assert(index.queryRange(TimeRange{101, 199}).empty());
    assertSameIds(index.queryRange(TimeRange{100, 100}), {1});
    assertSameIds(index.queryRange(TimeRange{200, 200}), {2});
    std::cout << "[PASS] temporal_index: queryRange 闭区间边界\n";
}

// 伪代码:
//   1. 在同一时间戳 200 登记 id2、id3 两个块,另登记 ts=100 的 id1;
//   2. 查询 [100,200]:断言三个块全部命中,bucketCount 为 2;
//   3. remove(200, id2):断言查询只剩 id1、id3,且 ts=200 的桶仍在;
//   4. remove(200, id3):断言 ts=200 桶销毁,bucketCount 回落为 1;
//   5. 删除不存在的时间戳/块:断言无副作用。
void testDuplicateTimestamps() {
    TemporalIndex index;
    index.insert(100, 1);
    index.insert(200, 2);
    index.insert(200, 3);
    assertSameIds(index.queryRange(TimeRange{100, 200}), {1, 2, 3});
    assert(index.bucketCount() == 2);

    index.remove(200, 2);
    assertSameIds(index.queryRange(TimeRange{100, 200}), {1, 3});
    assert(index.bucketCount() == 2);

    index.remove(200, 3);
    assert(index.bucketCount() == 1);

    index.remove(999, 42);
    assert(index.bucketCount() == 1);
    std::cout << "[PASS] temporal_index: 重复时间戳多块与 remove\n";
}

// 伪代码:
//   1. 插入 ts=100/200/300 三个块;
//   2. expireBefore(200):断言只返回 id1(严格小于),ts=200 的块保留;
//   3. 全范围查询确认剩余 id2、id3,bucketCount 为 2;
//   4. expireBefore(1000):断言返回 id2、id3,索引清空(bucketCount 为 0);
//   5. 对空索引再次 expireBefore:断言返回空列表。
void testExpireBefore() {
    TemporalIndex index;
    index.insert(100, 1);
    index.insert(200, 2);
    index.insert(300, 3);

    assertSameIds(index.expireBefore(200), {1});
    assertSameIds(index.queryRange(TimeRange{0, 1000}), {2, 3});
    assert(index.bucketCount() == 2);

    assertSameIds(index.expireBefore(1000), {2, 3});
    assert(index.bucketCount() == 0);

    assert(index.expireBefore(5000).empty());
    std::cout << "[PASS] temporal_index: expireBefore 返回被清理 id\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用四个测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testInsertAndQueryRange();
    testRangeBoundaries();
    testDuplicateTimestamps();
    testExpireBefore();
    std::cout << "ALL TESTS PASS (temporal_index)\n";
    return 0;
}
