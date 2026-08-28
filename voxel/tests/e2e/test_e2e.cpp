// ============================================================================
// 文件: tests/e2e/test_e2e.cpp
// 模块: stmb_tests(端到端测试)
// 覆盖范围: 模拟体素世界智能体的一段完整时空记忆生命周期,按顺序执行并
//           逐步断言:
//   1. 创建小容量服务(容量 4);
//   2. 写入多个不同空间位置、不同时间戳的记忆块;
//   3. 「空间区域 + 时间范围」联合查询,验证只命中应命中的块;
//   4. 持续写入触发 LRU 淘汰,验证被淘汰的是最久未访问的块,
//      且被淘汰块随后查询不到(索引已同步清理);
//   5. 通过 query 访问某块刷新其 LRU 位置,再写入新块,
//      验证淘汰顺序随之改变(被刷新的块免于淘汰);
//   6. expireBefore 清理旧时间块,验证 store 与两个索引全部一致;
//   7. 校验 stats 与实际状态吻合;
//   8. 对块确认到 Stable 后 reportChange,验证 Changing 期间旧数据仍生效;
//   9. confirm 确认变更,验证 query 命中新数据、version+1、historyOf 两个版本;
//  10. getAt(旧时间) 返回旧版本(时间回溯),stats 不受状态机操作影响;
//  11. 持久化:开启 dataDir 的服务写入 + 状态机操作 + checkpoint + 再写入;
//  12. 同目录重建服务,验证块/状态/版本链/回溯全部延续,清理临时目录。
// 测试思路:
//   - 候选集合来自 unordered_set,迭代顺序不确定,因此凡是需要依赖
//     LRU 顺序的步骤,都先用「只命中单块的窄查询」把访问顺序刷成确定状态;
//   - 每个块的中心落在不同格子(cellSize=10),时间戳各占一个时间槽
//     (timeSlotMs=1000),保证窄查询只命中目标块;
//   - #undef NDEBUG 保证 Release 构建下断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "stmb_service.h"
#include "functions.h"
#include "json.h"
#include "vql.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace stmb;

// 伪代码:
//   1. 以 (cx, cy, cz) 为中心、半边长 2 构造 AABB;
//   2. 填充 payload 与 timestamp(id/key 由服务层写入时生成);
//   3. 返回构造好的块。
MemoryBlock makeBlock(double cx, double cy, double cz,
                      TimeStamp ts, const std::string& payload) {
    MemoryBlock b;
    b.region.min = {cx - 2.0, cy - 2.0, cz - 2.0};
    b.region.max = {cx + 2.0, cy + 2.0, cz + 2.0};
    b.payload = payload;
    b.timestamp = ts;
    return b;
}

// 伪代码:
//   1. 以 (cx, cy, cz) 为中心构造半边长 3 的窄查询区域(严格大于块半边长,
//      保证包住目标块,又因块间中心相距 20 而不会碰到其他块);
//   2. 返回构造好的区域。
AABB around(double cx, double cy, double cz) {
    return AABB{{cx - 3.0, cy - 3.0, cz - 3.0}, {cx + 3.0, cy + 3.0, cz + 3.0}};
}

// 伪代码:
//   1. 收集 hits 中所有块的 id 并排序;
//   2. 与期望 id 列表(排序后)逐一比较,完全相等才通过。
void assertHitIds(const std::vector<MemoryBlock>& hits, std::vector<BlockId> expected) {
    std::vector<BlockId> actual;
    for (const MemoryBlock& b : hits) {
        actual.push_back(b.id);
    }
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    if (actual != expected) {
        std::cerr << "assertHitIds 失配: actual=[";
        for (BlockId id : actual) std::cerr << id << " ";
        std::cerr << "] expected=[";
        for (BlockId id : expected) std::cerr << id << " ";
        std::cerr << "]\n";
    }
    assert(actual == expected);
}

// 伪代码(场景主流程):
//   1. 创建容量 4、格子 10、时间槽 1000ms 的服务;
//   2. 写入 A/B/C/D 四个块:中心分别位于 (5,5,5)/(25,5,5)/(5,25,5)/(25,25,5),
//      时间戳 0/1000/2000/3000,断言均无淘汰且 id 自增为 1..4;
//   3. 联合查询:区域 [0,30]x[0,30]x[0,10] + 时间 [1000,2000],
//      断言恰好命中 B、C(A 时间太早,D 时间太晚);
//   4. 先用窄查询依次单独访问 B、C,把 LRU 顺序刷为确定状态 C>B>D>A;
//      写入 E(中心 (5,5,25),ts=4000):断言淘汰最久未访问的 A(id1),
//      且用 A 的时空范围查询不再命中;
//   5. 窄查询访问 D(当前最久未使用),再写入 F(中心 (25,25,25),ts=5000):
//      断言被淘汰的是 B(id2)而非刚刷新的 D,验证淘汰顺序随访问改变;
//      并断言 D 仍可命中、B 查询不到;
//   6. expireBefore(2500):断言只清理 C(id3,ts=2000),
//      剩余块 D/E/F 通过宽查询全部可命中(store 与索引一致);
//   7. 校验 stats:块数 3、容量 4、累计淘汰 2。
void runLifecycle() {
    // 步骤 1:创建小容量服务
    StmbService service(4, 10.0, 1000);
    assert(service.stats().blockCount == 0 && service.stats().capacity == 4);

    // 步骤 2:写入多个不同时空坐标的记忆块
    assert(!service.put(makeBlock( 5.0,  5.0, 5.0,    0, "A")).has_value());  // id1
    assert(!service.put(makeBlock(25.0,  5.0, 5.0, 1000, "B")).has_value());  // id2
    assert(!service.put(makeBlock( 5.0, 25.0, 5.0, 2000, "C")).has_value());  // id3
    assert(!service.put(makeBlock(25.0, 25.0, 5.0, 3000, "D")).has_value());  // id4
    assert(service.stats().blockCount == 4);

    // 步骤 3:空间区域 + 时间范围联合查询
    assertHitIds(service.query(AABB{{0.0, 0.0, 0.0}, {30.0, 30.0, 10.0}},
                               TimeRange{1000, 2000}),
                 {2, 3});  // 只命中 B、C

    // 步骤 4 前置:用窄查询单独访问 B、C,把 LRU 顺序刷成确定的 C>B>D>A
    assertHitIds(service.query(around(25.0, 5.0, 5.0), TimeRange{900, 1100}), {2});
    assertHitIds(service.query(around(5.0, 25.0, 5.0), TimeRange{1900, 2100}), {3});

    // 步骤 4:写入 E 触发淘汰,最久未访问的 A 被淘汰且随后查询不到
    const auto evicted1 = service.put(makeBlock(5.0, 5.0, 25.0, 4000, "E"));  // id5
    assert(evicted1.has_value() && evicted1->id == 1 && evicted1->payload == "A");
    assert(service.query(around(5.0, 5.0, 5.0), TimeRange{0, 500}).empty());

    // 步骤 5:访问 D 刷新其 LRU 位置,再写入 F,淘汰对象从 D 变为 B
    assertHitIds(service.query(around(25.0, 25.0, 5.0), TimeRange{2900, 3100}), {4});
    const auto evicted2 = service.put(makeBlock(25.0, 25.0, 25.0, 5000, "F"));  // id6
    assert(evicted2.has_value() && evicted2->id == 2 && evicted2->payload == "B");
    assert(service.query(around(25.0, 5.0, 5.0), TimeRange{900, 1100}).empty());   // B 已清理
    assertHitIds(service.query(around(25.0, 25.0, 5.0), TimeRange{2900, 3100}),
                 {4});  // D 因刷新而保留

    // 步骤 6:过期清理 ts<2500 的块,只有 C(ts=2000) 被清理,store 与索引一致
    const auto expired = service.expireBefore(2500);
    assert(expired.size() == 1 && expired[0] == 3);
    assert(service.query(around(5.0, 25.0, 5.0), TimeRange{1900, 2100}).empty());
    assertHitIds(service.query(AABB{{0.0, 0.0, 0.0}, {30.0, 30.0, 30.0}},
                               TimeRange{0, 10000}),
                 {4, 5, 6});  // 只剩 D、E、F

    // 步骤 7:校验 stats 与实际状态吻合
    const ServiceStats s = service.stats();
    assert(s.blockCount == 3 && s.capacity == 4 && s.evictCount == 2);

    // 步骤 8:对存活块 D(id4,当前 Pending)确认到 Stable,再报告矛盾观测;
    //         Changing 期间 query 命中的仍是旧数据(候选变更未生效)
    assert(service.confirm(4, 6000));                        // Pending -> Stable
    assert(service.reportChange(4, "D2", 0.9, 6500));        // Stable -> Changing
    const auto during = service.query(around(25.0, 25.0, 5.0), TimeRange{2900, 3100});
    assert(during.size() == 1 && during[0].payload == "D");
    assert(during[0].state == BlockState::Changing);

    // 步骤 9:confirm 确认变更,query 命中新数据、version+1、historyOf 两个版本
    assert(service.confirm(4, 7000));                        // Changing -> Stable(v2)
    const auto after = service.query(around(25.0, 25.0, 5.0), TimeRange{2900, 3100});
    assert(after.size() == 1 && after[0].payload == "D2");
    assert(after[0].version == 2 && after[0].state == BlockState::Stable);
    const auto history = service.historyOf(4);
    assert(history.size() == 2);
    assert(history[0].payload == "D" && history[1].payload == "D2");

    // 步骤 10:时间回溯:旧时刻返回旧版本,确认时刻起返回新版本;
    //         状态机操作不影响 stats(无新增写入与淘汰)
    assert(service.getAt(4, 3500)->payload == "D");
    assert(service.getAt(4, 7000)->payload == "D2");
    const ServiceStats s2 = service.stats();
    assert(s2.blockCount == 3 && s2.capacity == 4 && s2.evictCount == 2);
    std::cout << "[PASS] e2e: 写入->联合查询->LRU淘汰->刷新改序->过期清理->stats"
                 "->状态机变更->版本历史->时间回溯\n";
}

// 伪代码(持久化生命周期):
//   1. 在系统临时目录下准备干净的 dataDir;
//   2. 服务 A(开启 dataDir):写入 2 块,confirm 转 Stable,checkpoint,
//      再写入第 3 块(仅存于 WAL),随后析构;
//   3. 同目录新建服务 B:断言 3 块全部可查、块 1 状态为 Stable、
//      版本链与 getAt 回溯可用、新写入 id 延续为 4;
//   4. B 再做一次变更并 checkpoint,重建服务 C 验证二次恢复也一致;
//   5. 清理临时目录。
void testPersistenceLifecycle() {
    const std::string dir =
        (std::filesystem::temp_directory_path() / "stmb_tests" / "e2e_persist").string();
    std::filesystem::remove_all(dir);
    const ServiceConfig config{4, 10.0, 1000, 1, 0, dir, 0.0, 0, 8, {}, 0, 0, 0};

    {
        StmbService serviceA(config);
        serviceA.put(makeBlock(5.0, 5.0, 5.0, 100, "P1"));    // id1
        serviceA.put(makeBlock(25.0, 5.0, 5.0, 200, "P2"));   // id2
        assert(serviceA.confirm(1, 300));                     // id1 -> Stable
        assert(serviceA.checkpoint());
        serviceA.put(makeBlock(5.0, 25.0, 5.0, 400, "P3"));   // id3,仅 WAL
    }

    {
        StmbService serviceB(config);
        assert(serviceB.stats().blockCount == 3);
        assertHitIds(serviceB.query(AABB{{0.0, 0.0, 0.0}, {30.0, 30.0, 10.0}},
                                    TimeRange{0, 1000}),
                     {1, 2, 3});
        assert(serviceB.get(1)->state == BlockState::Stable);
        assert(serviceB.historyOf(1).size() == 1);
        assert(serviceB.getAt(1, 100)->payload == "P1");

        // 二次变更 + checkpoint,验证多轮恢复
        assert(serviceB.reportChange(1, "P1-v2", 0.9, 500));
        assert(serviceB.confirm(1, 600));
        assert(serviceB.checkpoint());
    }

    {
        StmbService serviceC(config);
        const auto b1 = serviceC.get(1);
        assert(b1.has_value() && b1->payload == "P1-v2" && b1->version == 2);
        assert(serviceC.historyOf(1).size() == 2);
        assert(serviceC.getAt(1, 100)->payload == "P1");
        assert(serviceC.getAt(1, 600)->payload == "P1-v2");
        serviceC.put(makeBlock(25.0, 25.0, 5.0, 700, "P4"));
        assert(serviceC.get(4).has_value());                  // nextId 延续
    }

    std::filesystem::remove_all(dir);
    std::cout << "[PASS] e2e: 持久化 checkpoint->重建->状态延续->二次恢复\n";
}

// 伪代码(分片模式完整生命周期):
//   1. 准备干净目录,配置分片服务(容量 100、驻留上限 2、分片边长 100、
//      时间桶 10000ms);
//   2. 写入落在 3 个分片的 3 块,对块 1 完成一次状态机变更;
//   3. 跨分片查询:宽区域全覆盖,断言 3 块都命中,且 loadedShards <= 2
//      (查询过程中发生换出);
//   4. checkpoint 后析构;同目录重建服务:逐块验证数据、状态、版本链与
//      getAt 回溯延续,loadedShards 从 0 随查询增长;
//   5. 清理临时目录。
void testShardLifecycle() {
    const std::string dir =
        (std::filesystem::temp_directory_path() / "stmb_tests" / "e2e_shard").string();
    std::filesystem::remove_all(dir);
    const ServiceConfig config{100, 10.0, 1000, 1, 0, dir, 100.0, 10000, 2, {}, 0, 0, 0};

    {
        StmbService service(config);
        service.put(makeBlock(5.0, 5.0, 5.0, 100, "X1"));      // 分片 (0,0,0,0)
        service.put(makeBlock(150.0, 5.0, 5.0, 200, "X2"));    // 分片 (1,0,0,0)
        service.put(makeBlock(5.0, 5.0, 5.0, 15000, "X3"));    // 分片 (0,0,0,1)
        assert(service.confirm(1, 300));
        assert(service.reportChange(1, "X1-v2", 0.9, 400));
        assert(service.confirm(1, 500));

        const auto all = service.query(AABB{{0.0, 0.0, 0.0}, {200.0, 10.0, 10.0}},
                                       TimeRange{0, 20000});
        assertHitIds(all, {1, 2, 3});                          // 跨分片联合查询
        assert(service.stats().loadedShards <= 2);             // 驻留不超上限
        assert(service.checkpoint());
    }

    {
        StmbService service(config);
        assert(service.stats().loadedShards == 0);
        assertHitIds(service.query(around(5.0, 5.0, 5.0), TimeRange{0, 1000}), {1});
        assert(service.get(1)->payload == "X1-v2");
        assert(service.get(1)->version == 2);
        assert(service.historyOf(1).size() == 2);
        assert(service.getAt(1, 100)->payload == "X1");
        assert(service.getAt(1, 500)->payload == "X1-v2");
        assertHitIds(service.query(around(150.0, 5.0, 5.0), TimeRange{0, 1000}), {2});
        assertHitIds(service.query(around(5.0, 5.0, 5.0), TimeRange{14000, 16000}),
                     {3});
        assert(service.stats().loadedShards <= 2);
    }

    std::filesystem::remove_all(dir);
    std::cout << "[PASS] e2e: 分片模式 写入->跨片查询->换出->重启恢复\n";
}

// 伪代码(三级金字塔:粗骨架 + 局部细化):
//   1. LOD 服务({100,10,1}):写入 2 个粗层骨架块(北/南两个大区域),
//      在北区内写 3 个细层细节块和 1 个中层块;
//   2. 未细化检测:北区 isRefined 为 true,南区为 false;
//   3. 先粗后细查询:广域 queryCoarseToFine 断言命中 2 骨架 + 北区下钻的
//      4 块(1 中层 + 3 细层)共 6 块;南区骨架不下钻出细数据;
//   4. 聚合上卷:buildSummaries(1) 由 3 个细层块生成 1 个摘要;
//      buildSummaries(0) 由 1 个中层块生成 1 个摘要;断言摘要字段;
//   5. 上卷后 queryAllLevels 命中 6 + 2 摘要 = 8 块。
void testLodLifecycle() {
    StmbService service(ServiceConfig{5000, 1.0, 1000, 1, 0, "", 0.0, 0, 8,
                                      {100.0, 10.0, 1.0}, 0, 0, 0});
    MemoryBlock skelNorth;
    skelNorth.region = AABB{{0.0, 0.0, 0.0}, {100.0, 100.0, 100.0}};
    skelNorth.payload = "skeleton-north";
    skelNorth.timestamp = 100;
    skelNorth.level = 0;
    MemoryBlock skelSouth;
    skelSouth.region = AABB{{0.0, 100.0, 0.0}, {100.0, 200.0, 100.0}};
    skelSouth.payload = "skeleton-south";
    skelSouth.timestamp = 100;
    skelSouth.level = 0;
    MemoryBlock midNorth = makeBlock(25.0, 50.0, 50.0, 200, "mid-north");
    midNorth.level = 1;
    MemoryBlock detail1 = makeBlock(10.0, 10.0, 10.0, 300, "detail-1");
    detail1.level = 2;
    MemoryBlock detail2 = makeBlock(12.0, 10.0, 10.0, 300, "detail-2");
    detail2.level = 2;
    MemoryBlock detail3 = makeBlock(14.0, 10.0, 10.0, 300, "detail-3");
    detail3.level = 2;
    service.put(skelNorth);
    service.put(skelSouth);
    service.put(midNorth);
    service.put(detail1);
    service.put(detail2);
    service.put(detail3);

    const AABB north{{0.0, 0.0, 0.0}, {100.0, 100.0, 100.0}};
    const AABB south{{0.0, 100.0, 0.0}, {100.0, 200.0, 100.0}};
    const AABB wide{{0.0, 0.0, 0.0}, {100.0, 200.0, 100.0}};
    const TimeRange all{0, 10000};
    assert(service.isRefined(north, 0));
    assert(!service.isRefined(south, 0));

    const auto c2f = service.queryCoarseToFine(wide, all);
    assert(c2f.size() == 6);  // 2 骨架 + 北区 1 中层 + 3 细层
    assert(service.queryAllLevels(wide, all).size() == 6);

    assert(service.buildSummaries(1) == 1);  // 3 细层块同处一个 L1 格子
    assert(service.buildSummaries(0) == 1);  // 1 个中层块上卷到 L0
    std::size_t summaries = 0;
    for (const MemoryBlock& b : service.queryAllLevels(wide, all)) {
        if (b.isSummary) {
            ++summaries;
        }
    }
    assert(summaries == 2);
    assert(service.queryAllLevels(wide, all).size() == 8);
    std::cout << "[PASS] e2e: LOD 三级金字塔(粗骨架+局部细化+上卷)\n";
}

// 伪代码(动静分离完整场景):
//   1. 启用持久化 + 动态层的服务:写入静态建筑块并 confirm 转 Stable;
//   2. 车辆实例 3 次上报形成轨迹(逐步接近建筑),末次零速度;
//   3. 验证轨迹累积、建筑被车辆临时占用;
//   4. updateDynamic 推进静止超时 -> Stationary,promoteStationary 沉淀为
//      静态层 Pending 块,confirm 转 Stable,车辆实例归档;
//   5. checkpoint 后重建服务:静态两块、车辆归档、轨迹可回溯,全部一致;
//   6. 清理临时目录。
void testDynamicLifecycle() {
    const std::string dir =
        (std::filesystem::temp_directory_path() / "stmb_tests" / "e2e_dynamic").string();
    std::filesystem::remove_all(dir);
    const ServiceConfig config{100, 10.0, 1000, 1, 0, dir, 0.0, 0, 8, {},
                               1500, 60000, 0};
    const std::array<double, 3> zero{0.0, 0.0, 0.0};

    {
        StmbService service(config);
        service.put(makeBlock(5.0, 5.0, 5.0, 0, "building"));   // id1 静态建筑
        assert(service.confirm(1, 100));                        // -> Stable

        service.reportMoving(1, "car", AABB{{19.0, 4.0, 4.0}, {21.0, 6.0, 6.0}},
                             zero, 0, 1);
        service.reportMoving(1, "car", AABB{{14.0, 4.0, 4.0}, {16.0, 6.0, 6.0}},
                             zero, 1000, 1);
        service.reportMoving(1, "car", AABB{{6.0, 4.0, 4.0}, {10.0, 6.0, 6.0}},
                             zero, 2000, 1);                    // 停在建筑旁
        service.reportMoving(1, "car", AABB{{6.0, 4.0, 4.0}, {10.0, 6.0, 6.0}},
                             zero, 3000, 1);                    // 原地复报=静止
        assert(service.trajectoryOf(1).size() == 4);

        const auto occupied = service.query(AABB{{3.0, 3.0, 3.0}, {7.0, 7.0, 7.0}},
                                            TimeRange{0, 100});
        assert(occupied.size() == 1 &&
               occupied[0].temporarilyOccupiedBy.size() == 1);  // 临时占用

        service.updateDynamic(4500);                            // 静止超时
        assert(service.dynamicStats().stationaryCount == 1);
        const auto created = service.promoteStationary(5000);   // 沉淀
        assert(created.size() == 1 && created[0] == 2);
        assert(service.get(2)->state == BlockState::Pending);
        assert(service.confirm(2, 6000));                       // -> Stable
        assert(service.dynamicStats().archivedCount == 1);
        assert(service.checkpoint());
    }

    {
        StmbService service(config);
        assert(service.stats().blockCount == 2);
        assert(service.get(1)->state == BlockState::Stable);
        const auto settled = service.get(2);
        assert(settled.has_value() && settled->payload == "car" &&
               settled->state == BlockState::Stable);
        const DynamicStats ds = service.dynamicStats();
        assert(ds.archivedCount == 1 && ds.activeCount == 0);
        assert(service.trajectoryOf(1).size() == 4);            // 轨迹回溯
    }

    std::filesystem::remove_all(dir);
    std::cout << "[PASS] e2e: 动静分离(车辆轨迹->静止->沉淀->重启验证)\n";
}

// 伪代码(观测处理管线完整场景):
//   1. 多来源登记(高 0.9 / 低 0.4),写入 "meadow" 块并自确认转 Stable;
//   2. 低可靠观测 "shop" -> Mutation 挂起(窗口);反向观测 "meadow" ->
//      Transient 关闭,无版本产生;
//   3. 高+低多源累计 "shop" 达阈值 -> Mutation 生效,version+1;
//   4. 注册周期模式,相位内观测 -> Seasonal,版本数不变;
//   5. 低可靠候选挂起 -> sweepWindows 超时丢弃,恢复窗口前状态;
//   6. checkpoint 重建:payload/版本/模式全部延续;清理临时目录。
void testPipelineLifecycle() {
    const std::string dir =
        (std::filesystem::temp_directory_path() / "stmb_tests" / "e2e_pipeline").string();
    std::filesystem::remove_all(dir);
    const ServiceConfig config{100, 10.0, 1000, 2.0, 0, dir, 0.0, 0, 8, {},
                               0, 0, 5000};

    {
        StmbService service(config);
        service.registerSource(1, 0.9);   // 高可靠
        service.registerSource(2, 0.4);   // 低可靠
        service.put(makeBlock(5.0, 5.0, 5.0, 0, "meadow"));   // id1
        assert(service.confirm(1, 100));                      // 自确认 1.0
        assert(service.confirm(1, 200));                      // -> Stable

        // Mutation 挂起 -> Transient 关闭
        ChangeReport r = service.submitObservation(
            1, Observation{"shop", 0.9, 2, 1000});            // 0.4 < 2 挂起
        assert(r.type == ChangeType::Mutation && !r.accepted);
        r = service.submitObservation(1, Observation{"meadow", 0.9, 1, 2000});
        assert(r.type == ChangeType::Transient && r.accepted);  // 反向推翻
        assert(service.get(1)->version == 1);

        // 多源累计达阈值 -> Mutation 生效
        r = service.submitObservation(1, Observation{"shop", 0.9, 1, 3000});   // 0.9
        assert(!r.accepted);
        r = service.submitObservation(1, Observation{"shop", 0.9, 2, 3500});   // +0.4
        assert(!r.accepted);
        r = service.submitObservation(1, Observation{"shop", 0.9, 1, 4000});   // +0.9=2.2
        assert(r.type == ChangeType::Mutation && r.accepted);
        assert(service.get(1)->payload == "shop" && service.get(1)->version == 2);

        // 周期模式:相位内观测 -> Seasonal
        PeriodicPattern pattern;
        pattern.periodMs = 10000;
        pattern.phases = {{0, 5000, "shop"}, {5000, 5000, "shop-night"}};
        assert(service.registerPattern(1, pattern));
        r = service.submitObservation(1, Observation{"shop-night", 0.9, 1, 6500});
        assert(r.type == ChangeType::Seasonal && r.accepted);
        assert(service.historyOf(1).size() == 2);             // 不刷版本

        // 观察窗口:低可靠候选挂起 -> 超时丢弃
        r = service.submitObservation(1, Observation{"mall", 0.9, 2, 8000});
        assert(!r.accepted && service.get(1)->state == BlockState::Changing);
        assert(service.sweepWindows(8000 + 6000) == 1);
        assert(service.get(1)->state == BlockState::Stable);
        assert(service.get(1)->payload == "shop" && service.get(1)->version == 2);
        assert(service.checkpoint());
    }

    {
        StmbService service(config);
        const auto b = service.get(1);
        assert(b.has_value() && b->payload == "shop" && b->version == 2);
        assert(b->pattern.has_value() && b->pattern->phases.size() == 2);
        assert(service.historyOf(1).size() == 2);
    }

    std::filesystem::remove_all(dir);
    std::cout << "[PASS] e2e: 观测管线(多源->分类->窗口->仲裁->周期模式)\n";
}

// 伪代码(VQL + Function Calling 端到端):
//   1. LOD 服务写入骨架/细节块并做版本变更,构造 VqlEngine 与
//      FunctionRegistry;
//   2. VQL:FIND BLOCKS 命中、AT 回溯旧版本、SUMMARY 先粗后细、
//      FIND HISTORY、STATS,逐步断言结果数据;
//   3. dispatch:stmb_put 写新块 -> stmb_query 命中 -> stmb_observe 走管线
//      -> stmb_stats,模拟大模型一轮工具调用;
//   4. 未知工具与非法参数返回结构化错误。
void testVqlLifecycle() {
    StmbService service(ServiceConfig{5000, 1.0, 1000, 2.0, 0, "", 0.0, 0, 8,
                                      {100.0, 10.0, 1.0}, 0, 0, 5000});
    VqlEngine engine(service);
    FunctionRegistry registry(service);

    MemoryBlock skel;
    skel.region = AABB{{0.0, 0.0, 0.0}, {100.0, 100.0, 100.0}};
    skel.payload = "skeleton";
    skel.timestamp = 100;
    skel.level = 0;
    service.put(skel);                                    // id1
    MemoryBlock detail = makeBlock(10.0, 10.0, 10.0, 200, "detail");
    detail.level = 2;
    service.put(detail);                                  // id2
    assert(service.confirm(1, 300));                      // 自确认 1.0
    assert(service.confirm(1, 350));                      // 1.0+1.0 >= 2 -> Stable
    assert(service.reportChange(1, "skeleton-v2", 0.9, 400));
    assert(service.confirm(1, 500));                      // v2

    VqlResult r = engine.execute(
        "FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) SUMMARY");
    assert(r.ok && r.data.asArray().size() == 2);         // 骨架 + 细节
    r = engine.execute(
        "FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) LEVEL 0 AT 100");
    assert(r.data.asArray()[0].at("payload").asString() == "skeleton");
    r = engine.execute("FIND HISTORY OF 1");
    assert(r.data.asArray().size() == 2);
    r = engine.execute("STATS");
    assert(r.data.at("blocks").asNumber() == 2.0);

    // dispatch 驱动的"大模型工具调用"流程
    const auto argsOf = [](const std::string& text) {
        return JsonValue::parse(text, nullptr).value();
    };
    JsonValue resp = registry.dispatch(
        "stmb_put", argsOf(
            R"({"region":[200,0,0,210,10,10],"payload":"outpost","timestamp":600,"level":2})"));
    assert(resp.at("ok").asBool());
    resp = registry.dispatch(
        "stmb_query", argsOf(R"({"region":[190,0,0,220,20,20],"time_range":[500,700]})"));
    assert(resp.at("data").asArray().size() == 1);
    assert(resp.at("data").asArray()[0].at("payload").asString() == "outpost");
    resp = registry.dispatch(
        "stmb_observe", argsOf(
            R"({"block_id":1,"payload":"skeleton-v3","confidence":0.9,"source_id":1,"timestamp":700})"));
    assert(resp.at("ok").asBool() && resp.at("data").has("type"));
    resp = registry.dispatch("stmb_stats", argsOf("{}"));
    assert(resp.at("data").at("blocks").asNumber() == 3.0);
    resp = registry.dispatch("stmb_nope", argsOf("{}"));
    assert(!resp.at("ok").asBool());
    assert(resp.at("error").at("code").asString() == "unknown_tool");
    std::cout << "[PASS] e2e: VQL 查询 + dispatch 工具调用端到端\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用七个场景函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    runLifecycle();
    testPersistenceLifecycle();
    testShardLifecycle();
    testLodLifecycle();
    testDynamicLifecycle();
    testPipelineLifecycle();
    testVqlLifecycle();
    std::cout << "ALL TESTS PASS (e2e)\n";
    return 0;
}
