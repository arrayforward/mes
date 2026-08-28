// ============================================================================
// 文件: tests/service/test_stmb_service.cpp
// 模块: stmb_tests(StmbService 单元测试)
// 覆盖范围:
//   1. put 后 query 命中(时空联合查询的基本路径);
//   2. 空间粗筛 + 精过滤:与查询区域同格但 AABB 不相交的块不得被误命中;
//   3. 时间范围过滤:空间命中但时间戳超出范围的块不得命中;
//   4. remove 后两个索引同步清理,query 不再命中该块;
//   5. LRU 淘汰后索引同步清理,被淘汰块 query 不到;
//   6. expireBefore 同步清理 store 与索引,返回被清理的 id;
//   7. stats 字段(blockCount / capacity / evictCount)与实际状态一致;
//   8. 状态机:Pending 确认达阈值转 Stable;Stable 报告矛盾转 Changing 且
//      原数据仍生效;confirm 确认变更后新数据生效、version+1、历史两个版本;
//      非 Stable 块 reportChange / Stable 块 confirm 被拒绝;
//   9. 置信度衰减:带 now 的 query/get 返回有效置信度,存储基准值不改写;
//  10. 被淘汰块的版本历史仍可回溯,purgeHistory 后才消失。
// 测试思路: 用小块 AABB 与分明的格子(cellSize=10)构造用例,逐条 assert;
//           #undef NDEBUG 保证 Release 构建下断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "stmb_service.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

namespace {

using namespace stmb;

// 伪代码:
//   1. 以 (cx, cy, cz) 为中心、half 为半边长构造 AABB;
//   2. 填充 payload 与 timestamp(id/key 由服务层写入时生成);
//   3. 返回构造好的块。
MemoryBlock makeBlock(double cx, double cy, double cz, double half,
                      TimeStamp ts, const std::string& payload) {
    MemoryBlock b;
    b.region.min = {cx - half, cy - half, cz - half};
    b.region.max = {cx + half, cy + half, cz + half};
    b.payload = payload;
    b.timestamp = ts;
    return b;
}

// 伪代码:
//   1. 容量 3 的服务写入一个块,断言无淘汰;
//   2. 用覆盖该块的区域与时间范围查询:断言恰好命中一次且负载正确;
//   3. 用不相关区域查询:断言无命中。
void testPutThenQuery() {
    StmbService service(3, 10.0, 1000);
    assert(!service.put(makeBlock(5.0, 5.0, 5.0, 2.0, 100, "alpha")).has_value());

    const auto hits = service.query(AABB{{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}},
                                    TimeRange{0, 1000});
    assert(hits.size() == 1 && hits[0].payload == "alpha" && hits[0].id == 1);
    assert(service.query(AABB{{100.0, 100.0, 100.0}, {110.0, 110.0, 110.0}},
                         TimeRange{0, 1000}).empty());
    std::cout << "[PASS] service: put 后 query 命中\n";
}

// 伪代码:
//   1. 写入块 A([1,2]^3)与块 B([6,9]^3):两者同处格子 (0,0,0),但 B 与
//      查询区域 [0,5]^3 的 AABB 并不相交(粗筛会给候选,精过滤应剔除);
//   2. 查询 [0,5]^3 + 时间 [0,2000]:断言只命中 A,B 未被误命中;
//   3. 另写入同区域但 ts=5000 的块 C,用时间范围 [0,2000] 查询:
//      断言 C 因时间过滤不命中,验证时间维度过滤生效。
void testCoarseFilterAndPreciseFilter() {
    StmbService service(5, 10.0, 1000);
    service.put(makeBlock(1.5, 1.5, 1.5, 0.5, 1000, "hit"));    // id1,与查询区相交
    service.put(makeBlock(7.5, 7.5, 7.5, 1.5, 1000, "near"));   // id2,同格但不相交
    service.put(makeBlock(2.0, 2.0, 2.0, 0.5, 5000, "late"));   // id3,相交但时间超界

    const auto hits = service.query(AABB{{0.0, 0.0, 0.0}, {5.0, 5.0, 5.0}},
                                    TimeRange{0, 2000});
    assert(hits.size() == 1 && hits[0].payload == "hit");
    std::cout << "[PASS] service: 空间粗筛+精过滤与时间范围过滤\n";
}

// 伪代码:
//   1. 写入两个块(id1、id2);
//   2. remove(1) 返回 true;用覆盖 id1 时空范围的查询验证不再命中
//      (两个索引已同步清理),而 id2 仍可命中;
//   3. remove(999) 返回 false(不存在的 id 无副作用)。
void testRemoveSyncsIndexes() {
    StmbService service(3, 10.0, 1000);
    service.put(makeBlock(5.0, 5.0, 5.0, 2.0, 100, "alpha"));
    service.put(makeBlock(25.0, 5.0, 5.0, 2.0, 200, "bravo"));

    assert(service.remove(1));
    assert(service.query(AABB{{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}},
                         TimeRange{0, 1000}).empty());
    const auto rest = service.query(AABB{{20.0, 0.0, 0.0}, {30.0, 10.0, 10.0}},
                                    TimeRange{0, 1000});
    assert(rest.size() == 1 && rest[0].payload == "bravo");
    assert(!service.remove(999));
    std::cout << "[PASS] service: remove 后索引同步清理\n";
}

// 伪代码:
//   1. 容量 2 的服务写入块 1、块 2;
//   2. 写入块 3 触发淘汰:断言返回块 1 的完整数据;
//   3. 用覆盖块 1 时空范围的查询验证被淘汰块不再命中(索引已同步清理);
//   4. 断言 stats:块数 2、容量 2、淘汰计数 1。
void testEvictionSyncsIndexes() {
    StmbService service(2, 10.0, 1000);
    service.put(makeBlock(5.0, 5.0, 5.0, 2.0, 100, "alpha"));
    service.put(makeBlock(25.0, 5.0, 5.0, 2.0, 200, "bravo"));

    const auto evicted = service.put(makeBlock(5.0, 25.0, 5.0, 2.0, 300, "charlie"));
    assert(evicted.has_value() && evicted->id == 1 && evicted->payload == "alpha");
    assert(service.query(AABB{{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}},
                         TimeRange{0, 1000}).empty());

    const ServiceStats s = service.stats();
    assert(s.blockCount == 2 && s.capacity == 2 && s.evictCount == 1);
    std::cout << "[PASS] service: LRU 淘汰后索引同步清理且 stats 正确\n";
}

// 伪代码:
//   1. 写入 ts=100/2000/3000 三个块;
//   2. expireBefore(2500):断言返回 id1、id2 两个 id,等于阈值的语义为
//      「严格小于」故 ts=3000 的块保留;
//   3. 用覆盖被清理块时空范围的查询验证不再命中;
//   4. 断言 stats 块数回落为 1,剩余块仍可正常查询。
void testExpireSyncsIndexes() {
    StmbService service(5, 10.0, 1000);
    service.put(makeBlock(5.0, 5.0, 5.0, 2.0, 100, "old1"));
    service.put(makeBlock(25.0, 5.0, 5.0, 2.0, 2000, "old2"));
    service.put(makeBlock(5.0, 25.0, 5.0, 2.0, 3000, "keep"));

    const auto expired = service.expireBefore(2500);
    assert(expired.size() == 2);
    assert(std::find(expired.begin(), expired.end(), 1) != expired.end());
    assert(std::find(expired.begin(), expired.end(), 2) != expired.end());

    assert(service.query(AABB{{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}},
                         TimeRange{0, 500}).empty());
    assert(service.query(AABB{{20.0, 0.0, 0.0}, {30.0, 10.0, 10.0}},
                         TimeRange{1500, 2500}).empty());
    const auto kept = service.query(AABB{{0.0, 20.0, 0.0}, {10.0, 30.0, 10.0}},
                                    TimeRange{2500, 3500});
    assert(kept.size() == 1 && kept[0].payload == "keep");
    assert(service.stats().blockCount == 1);
    std::cout << "[PASS] service: expireBefore 同步清理 store 与索引\n";
}

// 伪代码(状态机全路径):
//   1. 配置确认阈值 2 的服务,写入块 A(id1,ts=100,置信度 0.8):
//      断言初始状态 Pending、确认计数 0;
//   2. confirm 一次:仍 Pending(1 < 2);confirm 第二次:转 Stable;
//   3. reportChange("A2", 0.9):返回 true,状态转 Changing,
//      但 query 命中的仍是旧负载 "A"(当前生效数据不变);
//   4. confirm 确认变更:query 命中新负载 "A2"、version=2、状态 Stable;
//   5. 断言 historyOf 有两个版本(v1 被封存 validTo=确认时刻),
//      getAt(旧时刻) 回溯到 "A",getAt(确认时刻) 命中 "A2";
//   6. 非法迁移:对 Pending 块 reportChange、对 Stable 块 confirm、
//      对不存在的 id confirm/reportChange,全部返回 false。
void testStateMachine() {
    StmbService service(ServiceConfig{4, 10.0, 1000, 2, 0, "", 0.0, 0, 8, {}, 0, 0, 0});
    MemoryBlock a = makeBlock(5.0, 5.0, 5.0, 2.0, 100, "A");
    a.confidence = 0.8;
    service.put(a);

    assert(service.get(1)->state == BlockState::Pending);
    assert(service.get(1)->confirmations == 0);
    assert(service.confirm(1, 200));
    assert(service.get(1)->state == BlockState::Pending);   // 未达阈值
    assert(service.get(1)->confirmations == 1);
    assert(service.confirm(1, 300));
    assert(service.get(1)->state == BlockState::Stable);    // 达阈值转稳定

    assert(service.reportChange(1, "A2", 0.9, 400));
    const auto during = service.query(AABB{{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}},
                                      TimeRange{0, 1000});
    assert(during.size() == 1 && during[0].state == BlockState::Changing);
    assert(during[0].payload == "A");                       // 旧数据仍生效

    assert(service.confirm(1, 500));                        // 确认变更
    const auto after = service.get(1);
    assert(after->state == BlockState::Stable);
    assert(after->payload == "A2" && after->version == 2);
    assert(std::abs(after->confidence - 0.9) < 1e-9);

    const auto history = service.historyOf(1);
    assert(history.size() == 2);
    assert(history[0].version == 1 && history[0].payload == "A");
    assert(history[0].validTo.has_value() && *history[0].validTo == 500);
    assert(history[1].version == 2 && history[1].payload == "A2");
    assert(!history[1].validTo.has_value());
    assert(service.getAt(1, 100)->payload == "A");          // 时间回溯
    assert(service.getAt(1, 499)->payload == "A");
    assert(service.getAt(1, 500)->payload == "A2");

    service.put(makeBlock(25.0, 5.0, 5.0, 2.0, 600, "B"));  // id2,Pending
    assert(!service.reportChange(2, "B2", 0.5, 700));       // Pending 不接受变更
    assert(!service.confirm(1, 800));                       // Stable 无事可确认
    assert(!service.confirm(999, 800));                     // 不存在的 id
    assert(!service.reportChange(999, "x", 0.5, 800));
    std::cout << "[PASS] service: 状态机 Pending->Stable->Changing->Stable\n";
}

// 伪代码(置信度衰减):
//   1. 配置半衰期 1000ms 的服务,写入 ts=0、基准置信度 0.8 的块;
//   2. get(id) / query(不带 now):断言返回基准值 0.8;
//   3. get(id, 1000) 断言约 0.4,get(id, 2000) 断言约 0.2;
//      query(带 now=1000) 命中块置信度约 0.4;
//   4. 再次 get(id):断言存储基准值未被衰减调用改写,仍为 0.8。
void testConfidenceDecay() {
    StmbService service(ServiceConfig{2, 10.0, 1000, 1, 1000, "", 0.0, 0, 8, {}, 0, 0, 0});
    MemoryBlock b = makeBlock(5.0, 5.0, 5.0, 2.0, 0, "decay");
    b.confidence = 0.8;
    service.put(b);

    assert(std::abs(service.get(1)->confidence - 0.8) < 1e-9);
    assert(std::abs(service.get(1, 1000)->confidence - 0.4) < 1e-9);
    assert(std::abs(service.get(1, 2000)->confidence - 0.2) < 1e-9);

    const AABB region{{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}};
    const auto baseHits = service.query(region, TimeRange{0, 1000});
    assert(baseHits.size() == 1 && std::abs(baseHits[0].confidence - 0.8) < 1e-9);
    const auto decayedHits = service.query(region, TimeRange{0, 1000}, 1000);
    assert(decayedHits.size() == 1 && std::abs(decayedHits[0].confidence - 0.4) < 1e-9);

    assert(std::abs(service.get(1)->confidence - 0.8) < 1e-9);  // 基准值不改写
    std::cout << "[PASS] service: query/get 返回衰减后的有效置信度\n";
}

// 伪代码(淘汰块历史回溯):
//   1. 容量 1、阈值 1 的服务写入块 A 并完成一次变更(两个版本);
//   2. 写入块 B 触发 LRU 淘汰 A:断言 A 查询不到(store 与索引已清理);
//   3. 断言 historyOf(A) 仍有两个版本、getAt 可回溯旧数据(历史默认保留);
//   4. purgeHistory(A) 后断言历史清空。
void testEvictedHistoryRetained() {
    StmbService service(ServiceConfig{1, 10.0, 1000, 1, 0, "", 0.0, 0, 8, {}, 0, 0, 0});
    service.put(makeBlock(5.0, 5.0, 5.0, 2.0, 100, "A"));
    assert(service.confirm(1, 200));                        // Pending -> Stable
    assert(service.reportChange(1, "A2", 0.9, 300));
    assert(service.confirm(1, 400));                        // Changing -> Stable(v2)

    const auto evicted = service.put(makeBlock(25.0, 5.0, 5.0, 2.0, 500, "B"));
    assert(evicted.has_value() && evicted->id == 1);
    assert(service.query(AABB{{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}},
                         TimeRange{0, 1000}).empty());

    const auto history = service.historyOf(1);              // 历史保留
    assert(history.size() == 2);
    assert(service.getAt(1, 100)->payload == "A");
    assert(service.getAt(1, 400)->payload == "A2");

    service.purgeHistory(1);
    assert(service.historyOf(1).empty());
    assert(!service.getAt(1, 100).has_value());
    std::cout << "[PASS] service: 淘汰块历史可回溯,purgeHistory 手动清理\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用八个测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testPutThenQuery();
    testCoarseFilterAndPreciseFilter();
    testRemoveSyncsIndexes();
    testEvictionSyncsIndexes();
    testExpireSyncsIndexes();
    testStateMachine();
    testConfidenceDecay();
    testEvictedHistoryRetained();
    std::cout << "ALL TESTS PASS (stmb_service)\n";
    return 0;
}
