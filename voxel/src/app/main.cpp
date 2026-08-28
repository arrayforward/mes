// ============================================================================
// 文件: main.cpp
// 模块: stmb_demo(演示程序,依赖 stmb_service)
// 用途: 端到端演示时空记忆块服务的完整工作流程:
//         1. 用小容量(4)构造服务;
//         2. 写入 5 个分布在不同空间位置与时间点的记忆块,触发 LRU 淘汰;
//         3. 演示「空间区域 + 时间范围」联合查询;
//         4. 演示访问刷新 LRU 后再次写入时淘汰顺序的变化;
//         5. 演示按时间戳的过期清理;
//         6. 打印最终统计信息;
//         7. 演示块状态机:确认转稳定 -> 报告矛盾观测 -> 确认变更生效;
//         8. 演示版本历史与时间回溯(getAt);
//         9. 演示置信度随时间按半衰期衰减(存储基准值不改写);
//        10. 演示本地持久化:写入 -> checkpoint -> 重建服务 -> 数据仍在;
//        11. 演示分片模式:跨分片写入、驻留上限换出、checkpoint 与延迟加载。
// 设计思路: 每一步都打印输入与结果,让人能直观对照服务行为;
//           demo 只用服务门面 API,不触碰内部部件,模拟真实使用方。
// 架构角色: 可执行入口,是展示 stmb_service 能力的最小示例。
// ============================================================================
#include "stmb_service.h"
#include "functions.h"
#include "json.h"
#include "vql.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

using stmb::AABB;
using stmb::BlockId;
using stmb::MemoryBlock;
using stmb::TimeRange;
using stmb::TimeStamp;

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
//   1. 以 (cx, cy, cz) 为中心构造半边长 3 的窄查询区域;
//   2. 返回构造好的区域(分片演示按单块精确查询用)。
AABB around2(double cx, double cy, double cz) {
    AABB box;
    box.min = {cx - 3.0, cy - 3.0, cz - 3.0};
    box.max = {cx + 3.0, cy + 3.0, cz + 3.0};
    return box;
}

// 伪代码:
//   1. 若有被淘汰块,打印其 id 与负载;否则打印"无淘汰"。
void printEvicted(const std::optional<MemoryBlock>& evicted) {
    if (evicted.has_value()) {
        std::cout << "    -> 触发 LRU 淘汰:块 id=" << evicted->id
                  << " payload=\"" << evicted->payload << "\"\n";
    } else {
        std::cout << "    -> 无淘汰\n";
    }
}

// 伪代码:
//   1. 打印查询条件;
//   2. 遍历命中结果,逐行打印块 id、负载与时间戳;
//   3. 若无命中,打印提示。
void printHits(const std::vector<MemoryBlock>& hits) {
    if (hits.empty()) {
        std::cout << "    -> 无命中\n";
        return;
    }
    for (const MemoryBlock& b : hits) {
        std::cout << "    -> 命中 块 id=" << b.id
                  << " payload=\"" << b.payload << "\""
                  << " timestamp=" << b.timestamp << "\n";
    }
}

}  // namespace

// 伪代码:
//   1. 构造服务:容量 4、格子边长 10、时间槽 1000 毫秒;
//   2. 依次写入 5 个块(空间上相距较远、时间戳递增 1000),
//      第 5 次写入会触发 LRU 淘汰,打印每一步结果;
//   3. 做一次「区域 + 时间范围」联合查询并打印命中;
//   4. 先查询命中某个块刷新其 LRU,再写入新块,展示被淘汰的是另一个更久未用的块;
//   5. 调用 expireBefore 清理老块,打印被清理的 id 列表;
//   6. 打印最终 stats;
//   7. 状态机演示:confirm 转 Stable -> reportChange 转 Changing(旧数据仍生效)
//      -> confirm 确认变更(新数据生效、版本 +1);
//   8. 打印 foxtrot 的版本历史,并用 getAt 演示时间回溯;
//   9. 用带半衰期配置的服务演示置信度衰减;
//  10. 持久化演示:开 dataDir 的服务写入并 checkpoint,重建后数据仍在;
//  11. 分片模式演示:3 分片写入(驻留上限 2)、跨片查询、checkpoint 后重建
//      延迟加载,返回 0。
int main() {
    std::cout << "==== stmb demo:时空记忆块服务 ====\n";

    stmb::StmbService service(4, 10.0, 1000);
    std::cout << "[1] 构造服务:容量=4, 格子边长=10, 时间槽=1000ms\n";

    std::cout << "[2] 写入 5 个块(容量 4,第 5 次写入将触发淘汰)\n";
    printEvicted(service.put(makeBlock( 5.0,  5.0,  5.0, 2.0,    0, "alpha")));
    printEvicted(service.put(makeBlock(25.0,  5.0,  5.0, 2.0, 1000, "bravo")));
    printEvicted(service.put(makeBlock( 5.0, 25.0,  5.0, 2.0, 2000, "charlie")));
    printEvicted(service.put(makeBlock(25.0, 25.0,  5.0, 2.0, 3000, "delta")));
    printEvicted(service.put(makeBlock( 5.0,  5.0, 25.0, 2.0, 4000, "echo")));

    std::cout << "[3] 联合查询:区域=[0,30]x[0,30]x[0,10], 时间范围=[1000,3000]\n";
    AABB queryRegion;
    queryRegion.min = {0.0, 0.0, 0.0};
    queryRegion.max = {30.0, 30.0, 10.0};
    printHits(service.query(queryRegion, TimeRange{1000, 3000}));

    std::cout << "[4] 先查询命中 bravo(刷新其 LRU),再写入新块 foxtrot\n";
    AABB bravoRegion;
    bravoRegion.min = {23.0, 3.0, 3.0};
    bravoRegion.max = {27.0, 7.0, 7.0};
    printHits(service.query(bravoRegion, TimeRange{0, 4000}));
    printEvicted(service.put(makeBlock(25.0, 25.0, 25.0, 2.0, 5000, "foxtrot")));

    std::cout << "[5] 过期清理:清理时间戳 < 4000 的全部块\n";
    const std::vector<BlockId> expired = service.expireBefore(4000);
    for (BlockId id : expired) {
        std::cout << "    -> 清理 块 id=" << id << "\n";
    }
    if (expired.empty()) {
        std::cout << "    -> 无块被清理\n";
    }

    const stmb::ServiceStats s = service.stats();
    std::cout << "[6] 最终统计:块数=" << s.blockCount
              << " 容量=" << s.capacity
              << " 累计淘汰=" << s.evictCount << "\n";

    std::cout << "[7] 状态机:确认 foxtrot 转稳定 -> 报告矛盾观测 -> 确认变更\n";
    service.confirm(6, 6000);  // Pending -> Stable(便捷构造阈值=1)
    std::cout << "    确认后状态:" << stmb::toString(service.get(6)->state) << "\n";
    service.reportChange(6, "foxtrot-v2", 0.95, 6500);  // Stable -> Changing
    const auto during = service.get(6);
    std::cout << "    报告变更后状态:" << stmb::toString(during->state)
              << " 生效数据仍=\"" << during->payload << "\"\n";
    service.confirm(6, 7000);  // Changing -> Stable(v2)
    const auto after = service.get(6);
    std::cout << "    确认变更后状态:" << stmb::toString(after->state)
              << " 版本=" << after->version
              << " 生效数据=\"" << after->payload << "\"\n";

    std::cout << "[8] 版本历史与时间回溯\n";
    for (const stmb::BlockVersion& v : service.historyOf(6)) {
        std::cout << "    版本 " << v.version << ": payload=\"" << v.payload
                  << "\" validFrom=" << v.validFrom << " validTo=";
        if (v.validTo.has_value()) {
            std::cout << *v.validTo;
        } else {
            std::cout << "至今";
        }
        std::cout << "\n";
    }
    std::cout << "    getAt(t=5500) -> \"" << service.getAt(6, 5500)->payload << "\"\n";
    std::cout << "    getAt(t=7000) -> \"" << service.getAt(6, 7000)->payload << "\"\n";

    std::cout << "[9] 置信度衰减(半衰期 1000ms,基准 0.8,lastUpdate=0)\n";
    stmb::StmbService decaySvc(stmb::ServiceConfig{4, 10.0, 1000, 1, 1000, "",
                                                   0.0, 0, 8, {}, 0, 0, 0});
    stmb::MemoryBlock decayBlock = makeBlock(5.0, 5.0, 5.0, 2.0, 0, "sensor");
    decayBlock.confidence = 0.8;
    decaySvc.put(decayBlock);
    for (const stmb::TimeStamp now : {0, 1000, 2000}) {
        std::cout << "    now=" << now << " 有效置信度="
                  << decaySvc.get(1, now)->confidence << "\n";
    }
    std::cout << "    存储基准值仍为 " << decaySvc.get(1)->confidence << "\n";

    std::cout << "[10] 持久化:写入 -> checkpoint -> 重建服务 -> 数据仍在\n";
    const std::string dataDir = "/tmp/stmb_demo_data";
    std::filesystem::remove_all(dataDir);  // 清掉历史演示数据,保证输出可重复
    {
        stmb::StmbService persistent(stmb::ServiceConfig{4, 10.0, 1000, 1, 0,
                                                         dataDir, 0.0, 0, 8, {}, 0, 0, 0});
        persistent.put(makeBlock(5.0, 5.0, 5.0, 2.0, 100, "persist-a"));
        persistent.put(makeBlock(25.0, 5.0, 5.0, 2.0, 200, "persist-b"));
        persistent.confirm(1, 300);  // id1 -> Stable
        persistent.checkpoint();
        std::cout << "    服务 A:写入 2 块(id1 已确认 Stable)并 checkpoint 到 "
                  << dataDir << "\n";
    }
    {
        stmb::StmbService restored(stmb::ServiceConfig{4, 10.0, 1000, 1, 0,
                                                       dataDir, 0.0, 0, 8, {}, 0, 0, 0});
        const stmb::ServiceStats rs = restored.stats();
        std::cout << "    服务 B(同目录重建):块数=" << rs.blockCount
                  << " id1 状态=" << stmb::toString(restored.get(1)->state)
                  << " payload=\"" << restored.get(1)->payload << "\"\n";
        AABB region;
        region.min = {0.0, 0.0, 0.0};
        region.max = {30.0, 10.0, 10.0};
        printHits(restored.query(region, TimeRange{0, 1000}));
    }
    std::filesystem::remove_all(dataDir);

    std::cout << "[11] 分片模式:跨分片写入 -> 驻留上限换出 -> checkpoint -> 重建\n";
    const std::string shardDir = "/tmp/stmb_demo_shards";
    std::filesystem::remove_all(shardDir);
    const stmb::ServiceConfig shardCfg{100, 10.0, 1000, 1, 0,
                                       shardDir, 100.0, 10000, 2, {}, 0, 0, 0};
    {
        stmb::StmbService sharded(shardCfg);
        sharded.put(makeBlock(5.0, 5.0, 5.0, 2.0, 100, "shard-a"));
        sharded.put(makeBlock(150.0, 5.0, 5.0, 2.0, 200, "shard-b"));
        sharded.put(makeBlock(5.0, 5.0, 5.0, 2.0, 15000, "shard-c"));
        std::cout << "    写入 3 块(3 个分片),驻留分片数="
                  << sharded.stats().loadedShards << "(上限 2)\n";
        AABB wide;
        wide.min = {0.0, 0.0, 0.0};
        wide.max = {200.0, 10.0, 10.0};
        printHits(sharded.query(wide, TimeRange{0, 20000}));
        std::cout << "    跨分片查询后驻留分片数=" << sharded.stats().loadedShards
                  << "\n";
        sharded.checkpoint();
        std::cout << "    已 checkpoint 到 " << shardDir << "(manifest + 分片文件)\n";
    }
    {
        stmb::StmbService restored(shardCfg);
        std::cout << "    重建后驻留分片数=" << restored.stats().loadedShards
                  << "(延迟加载)\n";
        printHits(restored.query(around2(5.0, 5.0, 5.0), TimeRange{0, 1000}));
        std::cout << "    查询触达后驻留分片数=" << restored.stats().loadedShards
                  << "\n";
    }
    std::filesystem::remove_all(shardDir);

    std::cout << "[12] LOD 金字塔:粗骨架 -> 局部细化 -> 聚合上卷 -> 先粗后细\n";
    stmb::StmbService lod(stmb::ServiceConfig{5000, 1.0, 1000, 1, 0, "",
                                              0.0, 0, 8, {100.0, 10.0, 1.0}, 0, 0, 0});
    lod.put(makeBlock(50.0, 50.0, 50.0, 50.0, 100, "skeleton"));  // extent 100 -> L0
    lod.put(makeBlock(10.0, 10.0, 10.0, 1.0, 200, "detail-a"));   // extent 2 -> L2
    lod.put(makeBlock(12.0, 10.0, 10.0, 1.0, 300, "detail-b"));   // -> L2
    std::cout << "    自动层级:skeleton->L" << lod.get(1)->level
              << " detail-a->L" << lod.get(2)->level << "\n";
    const AABB wide = {{0.0, 0.0, 0.0}, {100.0, 100.0, 100.0}};
    const stmb::TimeRange all{0, 10000};
    std::cout << "    分层查询:L0=" << lod.queryLevel(wide, all, 0).size()
              << " L1=" << lod.queryLevel(wide, all, 1).size()
              << " L2=" << lod.queryLevel(wide, all, 2).size() << "\n";
    std::cout << "    上卷前 L0 区域已细化? "
              << (lod.isRefined(wide, 0) ? "是" : "否") << "\n";
    lod.buildSummaries(1);
    lod.buildSummaries(0);
    for (const stmb::MemoryBlock& b : lod.queryAllLevels(wide, all)) {
        if (b.isSummary) {
            std::cout << "    摘要块 L" << b.level << ": \"" << b.payload
                      << "\" confidence=" << b.confidence << "\n";
        }
    }
    std::cout << "    先粗后细查询命中 " << lod.queryCoarseToFine(wide, all).size()
              << " 块(L0 骨架 + 下钻的 L1 摘要 + L2 细节)\n";

    std::cout << "[13] 动静分离:车辆轨迹 -> 静止沉淀 -> 临时占用\n";
    stmb::StmbService dyn(stmb::ServiceConfig{100, 10.0, 1000, 1, 0, "",
                                              0.0, 0, 8, {}, 1500, 60000, 0});
    stmb::MemoryBlock building;
    building.region = {{3.0, 3.0, 3.0}, {7.0, 7.0, 7.0}};
    building.payload = "building";
    building.timestamp = 0;
    dyn.put(building);
    const std::array<double, 3> zero{0.0, 0.0, 0.0};
    dyn.reportMoving(1, "car", AABB{{19.0, 4.0, 4.0}, {21.0, 6.0, 6.0}}, zero, 0, 1);
    dyn.reportMoving(1, "car", AABB{{14.0, 4.0, 4.0}, {16.0, 6.0, 6.0}}, zero, 1000, 1);
    dyn.reportMoving(1, "car", AABB{{6.0, 4.0, 4.0}, {10.0, 6.0, 6.0}}, zero, 2000, 1);
    dyn.reportMoving(1, "car", AABB{{6.0, 4.0, 4.0}, {10.0, 6.0, 6.0}}, zero, 3000, 1);
    const auto traj = dyn.trajectoryOf(1);
    std::cout << "    车辆轨迹 " << traj.size() << " 点,第三段差分速度=( "
              << traj[2].velocity[0] << ", " << traj[2].velocity[1] << ", "
              << traj[2].velocity[2] << " )\n";
    const auto hits = dyn.query(AABB{{3.0, 3.0, 3.0}, {7.0, 7.0, 7.0}},
                                stmb::TimeRange{0, 100});
    std::cout << "    建筑被实例临时占用:" << hits[0].temporarilyOccupiedBy.size()
              << " 个\n";
    dyn.updateDynamic(4500);
    std::cout << "    静止超时后状态:" << stmb::toString(dyn.queryDynamic(
        AABB{{0.0, 0.0, 0.0}, {30.0, 10.0, 10.0}}, stmb::TimeRange{0, 4000})[0].state)
              << "\n";
    const auto created = dyn.promoteStationary(5000);
    dyn.confirm(created[0], 6000);
    std::cout << "    沉淀为静态块 id=" << created[0] << " payload=\""
              << dyn.get(created[0])->payload << "\" 状态="
              << stmb::toString(dyn.get(created[0])->state) << "\n";

    std::cout << "[14] 观测管线:多源仲裁 -> 变化分类 -> 窗口 -> 周期模式\n";
    stmb::StmbService pipe(stmb::ServiceConfig{100, 10.0, 1000, 2.0, 0, "",
                                               0.0, 0, 8, {}, 0, 0, 5000});
    pipe.registerSource(1, 0.9);
    pipe.registerSource(2, 0.4);
    stmb::MemoryBlock meadow;
    meadow.region = {{3.0, 3.0, 3.0}, {7.0, 7.0, 7.0}};
    meadow.payload = "meadow";
    meadow.timestamp = 0;
    pipe.put(meadow);
    pipe.confirm(1, 100);
    pipe.confirm(1, 200);  // -> Stable
    auto printReport = [](const char* step, const stmb::ChangeReport& r) {
        std::cout << "    " << step << " -> " << stmb::toString(r.type)
                  << (r.accepted ? "(接受)" : "(挂起/忽略)") << ": " << r.message
                  << "\n";
    };
    printReport("低可靠观测 \"shop\"      ",
                pipe.submitObservation(1, stmb::Observation{"shop", 0.9, 2, 1000}));
    printReport("反向观测 \"meadow\"      ",
                pipe.submitObservation(1, stmb::Observation{"meadow", 0.9, 1, 2000}));
    pipe.submitObservation(1, stmb::Observation{"shop", 0.9, 1, 3000});
    pipe.submitObservation(1, stmb::Observation{"shop", 0.9, 2, 3500});
    printReport("多源累计 \"shop\" 达阈值",
                pipe.submitObservation(1, stmb::Observation{"shop", 0.9, 1, 4000}));
    stmb::PeriodicPattern pattern;
    pattern.periodMs = 10000;
    pattern.phases = {{0, 5000, "shop"}, {5000, 5000, "shop-night"}};
    pipe.registerPattern(1, pattern);
    printReport("相位内观测 \"shop-night\"",
                pipe.submitObservation(1, stmb::Observation{"shop-night", 0.9, 1, 6500}));
    pipe.submitObservation(1, stmb::Observation{"mall", 0.9, 2, 8000});
    std::cout << "    窗口超时扫描丢弃候选 " << pipe.sweepWindows(14000) << " 个,当前=\""
              << pipe.get(1)->payload << "\" 版本=" << pipe.get(1)->version << "\n";

    std::cout << "[15] VQL + Function Calling(大模型外部记忆接口)\n";
    stmb::StmbService brain(stmb::ServiceConfig{5000, 1.0, 1000, 2.0, 0, "",
                                                0.0, 0, 8, {100.0, 10.0, 1.0},
                                                1000, 60000, 5000});
    stmb::MemoryBlock skel;
    skel.region = {{0.0, 0.0, 0.0}, {100.0, 100.0, 100.0}};
    skel.payload = "skeleton";
    skel.timestamp = 100;
    skel.level = 0;
    brain.put(skel);                                    // id1
    stmb::MemoryBlock detail = makeBlock(10.0, 10.0, 10.0, 1.0, 200, "detail");
    detail.level = 2;
    brain.put(detail);                                  // id2
    brain.confirm(1, 300);
    brain.confirm(1, 350);                              // 加权达阈值 -> Stable
    brain.reportChange(1, "skeleton-v2", 0.9, 400);
    brain.confirm(1, 500);                              // v2
    brain.reportMoving(1, "car", AABB{{4.0, 4.0, 4.0}, {6.0, 6.0, 6.0}},
                       {1.0, 0.0, 0.0}, 600, 1);

    stmb::VqlEngine engine(brain);
    auto runVql = [&engine](const std::string& sql) {
        const stmb::VqlResult r = engine.execute(sql);
        if (r.ok) {
            std::cout << "    VQL> " << sql << "\n         => "
                      << r.data.dump() << "\n";
        } else {
            std::cout << "    VQL> " << sql << "\n         !! " << r.error << "\n";
        }
    };
    runVql("FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) SUMMARY");
    runVql("FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) LEVEL 0 AT 100");
    runVql("FIND HISTORY OF 1");
    runVql("FIND DYNAMIC IN REGION(0,0,0,50,50,50) DURING(0,1000)");
    runVql("FIND TRAJECTORY OF 1");
    runVql("STATS");
    runVql("find blocks in region(0,0,0,100,100) during(0,1000)");  // 故意写错

    stmb::FunctionRegistry registry(brain);
    std::cout << "    toolSchemas(缩进,截取前两个工具):\n";
    const std::string schemaText = registry.toolSchemas().dump(2);
    std::size_t cut = schemaText.find("stmb_query_at");
    std::cout << schemaText.substr(0, cut == std::string::npos ? 600 : cut)
              << "...(共 8 个工具)\n";
    auto showCall = [&registry](const std::string& name, const std::string& args) {
        const stmb::JsonValue parsed = stmb::JsonValue::parse(args, nullptr).value();
        std::cout << "    CALL " << name << " " << args << "\n         => "
                  << registry.dispatch(name, parsed).dump() << "\n";
    };
    showCall("stmb_put",
             R"({"region":[200,0,0,210,10,10],"payload":"outpost","timestamp":600,"level":2})");
    showCall("stmb_query", R"({"region":[190,0,0,220,20,20],"time_range":[500,700]})");
    showCall("stmb_observe",
             R"({"block_id":1,"payload":"skeleton-v3","confidence":0.9,"source_id":1,"timestamp":700})");
    showCall("stmb_stats", "{}");
    return 0;
}
