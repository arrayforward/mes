// tick_bench —— mechanical-heart 心跳 tick 基准测试
//
// 模拟制造业中心心跳的一拍（README §1 心跳模型的可执行版本）：
//   感知（合成观测，不计时）→
//   stage entity    : entity::Resolver.ingest       —— 观测归并 / 实体浮现
//   stage geocore   : Kernel.BeginFrame + Save      —— 空间锚定
//   stage eventstore: append_event + append_profile  —— 因果存证
//   stage voxelstore: put_block                      —— 时空记忆块更新
//   stage knowledge : Engine.understand(profile_id)  —— 语义理解（分类链 + 规则推理）
//
// knowledge 引擎在跑拍前一次性装配（define_ontology / apply_lexicon /
// 可选 load_ame_lexicon 真实大词典），装配耗时不计入 tick；
// 每拍只计时 understand() 的推理成本。
//
// 用法：
//   tick_bench [--ticks=N] [--warmup=N] [--stations=K]
//              [--backend=memory|sqlite] [--knowledge=on|off] [--full-lexicon]
// 输出：每阶段 min/avg/p50/p95/p99/max（微秒）、整拍统计、吞吐（ticks/s）。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "entity/resolver.h"
#include "entitytree/backends/memory_store.h"
#include "entitytree/backends/sql_store.h"
#include "eventstore/backends/memory_store.h"
#include "eventstore/backends/sql_store.h"
#include "geocore/kernel.hpp"
#include "knowledge/engine.h"
#include "voxelstore/backends/memory_voxel_store.h"
#include "voxelstore/backends/sql_voxel_store.h"

namespace {

using Clock = std::chrono::steady_clock;
using Us = std::chrono::duration<double, std::micro>;
using Ms = std::chrono::duration<double, std::milli>;

constexpr int kStages = 5;
const char* kStageNames[kStages] = {"entity", "geocore", "eventstore", "voxelstore",
                                    "knowledge"};

struct Options {
    int ticks = 1000;
    int warmup = 50;
    int stations = 8;
    std::string backend = "memory";
    bool knowledge_on = true;
    bool full_lexicon = false;  // 启动时灌入 7.7 万词真实词典（一次性成本）
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--full-lexicon") { o.full_lexicon = true; continue; }
        auto eat = [&](const char* key, std::string& out) -> bool {
            std::string prefix = std::string(key) + "=";
            if (a.rfind(prefix, 0) == 0) { out = a.substr(prefix.size()); return true; }
            return false;
        };
        std::string v;
        if (eat("--ticks", v)) o.ticks = std::stoi(v);
        else if (eat("--warmup", v)) o.warmup = std::stoi(v);
        else if (eat("--stations", v)) o.stations = std::stoi(v);
        else if (eat("--backend", v)) o.backend = v;
        else if (eat("--knowledge", v)) o.knowledge_on = (v == "on" || v == "1" || v == "true");
        else {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            std::fprintf(stderr,
                "用法: tick_bench [--ticks=N] [--warmup=N] [--stations=K]\n"
                "                 [--backend=memory|sqlite] [--knowledge=on|off] [--full-lexicon]\n");
            std::exit(2);
        }
    }
    if (o.backend != "memory" && o.backend != "sqlite") {
        std::fprintf(stderr, "backend 仅支持 memory|sqlite\n");
        std::exit(2);
    }
    return o;
}

struct Stats {
    std::vector<double> samples;  // 微秒
    void add(double us) { samples.push_back(us); }
    void report(const char* name) const {
        if (samples.empty()) return;
        std::vector<double> s = samples;
        std::sort(s.begin(), s.end());
        auto pct = [&](double p) { return s[static_cast<size_t>((s.size() - 1) * p)]; };
        double sum = 0;
        for (double v : s) sum += v;
        std::printf("  %-12s n=%-6zu avg=%8.2f  p50=%8.2f  p95=%8.2f  p99=%8.2f  max=%9.2f  min=%8.2f us\n",
                    name, s.size(), sum / s.size(), pct(0.50), pct(0.95), pct(0.99),
                    s.back(), s.front());
    }
    double avg() const {
        double sum = 0;
        for (double v : samples) sum += v;
        return samples.empty() ? 0.0 : sum / samples.size();
    }
};

// 三个领域存储的统一持有（memory / sqlite 两套后端）。
struct Stores {
    std::unique_ptr<entitytree::EntityStore> entity;
    std::unique_ptr<eventstore::EventStore>  events;
    std::unique_ptr<voxelstore::VoxelStore>  voxels;
};

Stores make_stores(const std::string& backend) {
    Stores s;
    if (backend == "sqlite") {
        // 分文件部署，避免多连接同库写锁干扰计时
        std::remove("bench_tick_entity.db");
        std::remove("bench_tick_events.db");
        std::remove("bench_tick_voxel.db");
        s.entity = std::make_unique<entitytree::SqlEntityStore>("bench_tick_entity.db");
        s.events = std::make_unique<eventstore::SqlStore>("bench_tick_events.db");
        s.voxels = std::make_unique<voxelstore::SqlVoxelStore>("bench_tick_voxel.db");
    } else {
        s.entity = std::make_unique<entitytree::MemoryEntityStore>();
        s.events = std::make_unique<eventstore::MemoryStore>();
        s.voxels = std::make_unique<voxelstore::MemoryVoxelStore>();
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    Options opt = parse_args(argc, argv);

    Stores stores = make_stores(opt.backend);

    // ---- 一次性装配（不计入 tick）----
    entity::ResolverConfig rcfg;
    rcfg.bucket_size = 3600;  // 1 小时一个时间桶：同工位观测聚合到同一侧写
    entity::Resolver resolver(*stores.entity, rcfg);

    geocore::Kernel geo;
    geocore::SceneId scene = geo.CreateScene(
        "plant", /*tags=*/0, geocore::AnchorPath{}.Append(1),
        geocore::Euclidean3D{1024.0});  // 1km 厂房，Euclidean3D
    if (scene == geocore::kInvalidScene) {
        std::fprintf(stderr, "CreateScene 失败\n");
        return 1;
    }

    auto narrative = eventstore::Narrative::create("fact", "产线心跳");
    stores.events->append_narrative(narrative);

    // knowledge 引擎预加载：本体 + 种子词典（+ 可选真实大词典）
    knowledge::Engine eng;
    if (opt.knowledge_on) {
        auto ks0 = Clock::now();
        eng.define_ontology(std::string(KNOWLEDGE_ASSETS_DIR) + "/ontology_seed.json");
        auto ks1 = Clock::now();
        int edges = eng.apply_lexicon(std::string(KNOWLEDGE_ASSETS_DIR) + "/lexicon.json");
        auto ks2 = Clock::now();
        int ame_entries = 0;
        if (opt.full_lexicon) {
            ame_entries = eng.load_ame_lexicon(
                std::string(KNOWLEDGE_AME_LEXICON_DIR) + "/lexicon_zh.json");
        }
        auto ks3 = Clock::now();
        std::printf("knowledge 装配（一次性，不计入 tick）：\n");
        std::printf("  define_ontology  %8.1f ms\n", Ms(ks1 - ks0).count());
        std::printf("  apply_lexicon    %8.1f ms  (本体建边 %d 条)\n", Ms(ks2 - ks1).count(), edges);
        if (opt.full_lexicon)
            std::printf("  load_ame_lexicon %8.1f ms  (真实词典灌入 %d 条)\n",
                        Ms(ks3 - ks2).count(), ame_entries);
        std::printf("\n");
    }

    const char* attr_keys[3] = {"station_state", "torque", "temperature"};
    const char* attr_vals[3] = {"running", "12.5", "341.2"};

    Stats stage[kStages];
    Stats tick_total;

    std::printf("mechanical-heart tick bench: backend=%s ticks=%d warmup=%d stations=%d knowledge=%s%s\n",
                opt.backend.c_str(), opt.ticks, opt.warmup, opt.stations,
                opt.knowledge_on ? "on" : "off",
                opt.full_lexicon ? " full-lexicon" : "");

    const int total_iters = opt.warmup + opt.ticks;
    for (int i = 0; i < total_iters; ++i) {
        const bool warm = i < opt.warmup;
        const int station = i % opt.stations;
        const int64_t t = 1000 + i;  // 每拍推进 1 秒

        // ---- 感知（合成观测，不计时）----
        entitytree::Observation ob;
        ob.attribute_key = attr_keys[i % 3];
        ob.value = attr_vals[i % 3];
        ob.confidence = 0.9;
        ob.source_id = "sensor-" + std::to_string(station);
        ob.anchor_ref = "station-" + std::to_string(station);
        ob.timestamp = t;

        const geocore::Vec3d pos{10.0 * station + 0.01 * (i % 7), 2.0, 5.0};
        const std::string payload = "station-" + std::to_string(station) + " ok";

        auto tick_begin = Clock::now();

        // ---- stage 1: entity 浮现 ----
        auto t0 = Clock::now();
        std::string profile_id = resolver.ingest(ob);
        auto t1 = Clock::now();

        // ---- stage 2: geocore 锚定（帧推进 + 工件落位）----
        geo.BeginFrame(static_cast<double>(t));
        geocore::ObjectId oid = geo.Save(scene, pos, {}, 0.5);
        auto t2 = Clock::now();

        // ---- stage 3: eventstore 因果存证 ----
        auto ev = eventstore::Event::create(narrative.narrative_id, "station tick");
        stores.events->append_event(ev);
        auto pf = eventstore::Profile::create(
            ev.event_id, "active", eventstore::TimeRef::virtual_time("tick", t),
            ob.anchor_ref, ob.source_id, "report", payload);
        stores.events->append_profile(pf);
        auto t3 = Clock::now();

        // ---- stage 4: voxelstore 时空记忆块 ----
        voxelstore::VoxelBlock blk;
        blk.id = static_cast<uint64_t>(station) + 1;
        blk.region = voxelstore::Aabb{pos.x - 1, 0, 0, pos.x + 1, 4, 10};
        blk.payload = payload;
        blk.timestamp = t;
        blk.version = static_cast<uint32_t>(i / opt.stations) + 1;
        blk.state = "Stable";
        blk.confidence = 0.95;
        stores.voxels->put_block(blk);
        auto t4 = Clock::now();

        // ---- stage 5: knowledge 语义理解（分类链 + 规则推理 + 锚点）----
        if (opt.knowledge_on) {
            auto ing = eng.understand(*stores.events, pf.profile_id);
            (void)ing;
        }
        auto t5 = Clock::now();

        (void)profile_id;
        (void)oid;

        if (!warm) {
            stage[0].add(Us(t1 - t0).count());
            stage[1].add(Us(t2 - t1).count());
            stage[2].add(Us(t3 - t2).count());
            stage[3].add(Us(t4 - t3).count());
            if (opt.knowledge_on) stage[4].add(Us(t5 - t4).count());
            tick_total.add(Us(t5 - tick_begin).count());
        }
    }

    std::printf("\n每阶段耗时（%d 拍有效样本）：\n", opt.ticks);
    for (int s = 0; s < kStages; ++s) stage[s].report(kStageNames[s]);

    std::printf("\n整拍（%s 阶段串行合计）：\n", opt.knowledge_on ? "5" : "4");
    tick_total.report("tick_total");

    std::printf("\n吞吐：%.0f ticks/s（串行单线程）\n",
                tick_total.avg() > 0 ? 1.0e6 / tick_total.avg() : 0.0);

    if (opt.backend == "sqlite") {
        std::remove("bench_tick_entity.db");
        std::remove("bench_tick_events.db");
        std::remove("bench_tick_voxel.db");
    }
    return 0;
}
