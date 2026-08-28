#include "voxel_conformance.h"

#include "test_framework.h"
#include "voxelstore/voxel_store.h"

using namespace voxelstore;

namespace {

/// 造一个带全字段的块（观测管线 + 周期模式 + LOD）。
VoxelBlock make_block(uint64_t id, Aabb region, int64_t timestamp, int level,
                      std::string payload) {
    VoxelBlock b;
    b.id = id;
    b.cell_x = 1;
    b.cell_y = 2;
    b.cell_z = 3;
    b.time_slot = timestamp / 1000;
    b.region = region;
    b.payload = std::move(payload);
    b.timestamp = timestamp;
    b.version = 1;
    b.last_access = timestamp;
    b.state = "Stable";
    b.confidence = 0.9;
    b.confirmations = 3.5;
    b.last_update = timestamp;
    b.pending_payload = std::string("候选负载");
    b.pending_confidence = 0.4;
    b.window_start = timestamp - 500;
    b.window_weight = 2.0;
    b.window_sources = {7, 8, 9};
    PeriodicPattern pat;
    pat.period_ms = 86400000;
    pat.phases = {PatternPhase{0, 43200000, "白天"}, PatternPhase{43200000, 43200000, "夜晚"}};
    b.pattern = pat;
    b.suspect = true;
    b.level = level;
    b.is_summary = level > 0;
    b.source_level = level > 0 ? level - 1 : -1;
    return b;
}

} // namespace

void run_voxel_conformance(VoxelStore& store) {
    // ============ 1. 块：写入 / 读取全字段往返 ============
    auto b1 = make_block(1, Aabb{0, 0, 0, 10, 10, 10}, 1000, 0, "一座房子");
    store.put_block(b1);
    {
        auto got = store.get_block(1);
        CHECK(got.has_value());
        CHECK_EQ(got->cell_y, (int64_t)2);
        CHECK_EQ(got->region.max_x, 10.0);
        CHECK_EQ(got->payload, "一座房子");
        CHECK_EQ(got->timestamp, (int64_t)1000);
        CHECK_EQ(got->version, (int64_t)1);
        CHECK_EQ(got->state, "Stable");
        CHECK_EQ(got->confidence, 0.9);
        CHECK_EQ(got->confirmations, 3.5);
        CHECK(got->pending_payload && *got->pending_payload == "候选负载");
        CHECK(got->pending_confidence && *got->pending_confidence == 0.4);
        CHECK_EQ(got->window_start, (int64_t)500);
        CHECK_EQ(got->window_sources.size(), (size_t)3);
        CHECK_EQ(got->window_sources[1], (int64_t)8);
        CHECK(got->pattern.has_value());
        CHECK_EQ(got->pattern->period_ms, (int64_t)86400000);
        CHECK_EQ(got->pattern->phases.size(), (size_t)2);
        CHECK_EQ(got->pattern->phases[1].payload, "夜晚");
        CHECK(got->suspect);
        CHECK_EQ(got->level, 0);
        CHECK(!got->is_summary);
        CHECK_EQ(got->source_level, (int64_t)-1);
    }
    CHECK(!store.get_block(999).has_value());

    // ============ 2. upsert：同 id 再写覆盖当前态 ============
    {
        auto b1m = b1;
        b1m.payload = "一座两层房子";
        b1m.version = 2;
        b1m.pending_payload = std::nullopt;  // 可选字段清空也能往返
        b1m.pattern = std::nullopt;
        store.put_block(b1m);
        auto got = store.get_block(1);
        CHECK_EQ(got->payload, "一座两层房子");
        CHECK_EQ(got->version, (int64_t)2);
        CHECK(!got->pending_payload.has_value());
        CHECK(!got->pattern.has_value());
        CHECK_EQ(store.all_blocks().size(), (size_t)1);
    }

    // ============ 3. 时空联合查询 ============
    auto b2 = make_block(2, Aabb{20, 20, 20, 30, 30, 30}, 2000, 0, "一棵大树");
    b2.pattern = std::nullopt;
    b2.pending_payload = std::nullopt;
    auto b3 = make_block(3, Aabb{4, 4, 0, 6, 6, 3}, 1500, 1, "区块摘要");
    b3.pattern = std::nullopt;
    b3.pending_payload = std::nullopt;
    store.put_block(b2);
    store.put_block(b3);
    {
        // 空间：与 b1、b3 区域都相交
        auto r = store.query_blocks(Aabb{5, 5, 0, 15, 15, 15}, std::nullopt, std::nullopt,
                                    std::nullopt);
        CHECK_EQ(r.size(), (size_t)2);  // b1 + b3
        // 边界相接也算相交
        auto touch = store.query_blocks(Aabb{10, 10, 10, 12, 12, 12}, std::nullopt,
                                        std::nullopt, std::nullopt);
        CHECK_EQ(touch.size(), (size_t)1);
        CHECK_EQ(touch[0].id, (int64_t)1);
        // 不相交
        auto none = store.query_blocks(Aabb{11, 11, 11, 19, 19, 19}, std::nullopt,
                                       std::nullopt, std::nullopt);
        CHECK(none.empty());
        // 时间范围
        auto tr = store.query_blocks(std::nullopt, 1500, 2500, std::nullopt);
        CHECK_EQ(tr.size(), (size_t)2);  // b2 + b3
        // 层级
        auto lv = store.query_blocks(std::nullopt, std::nullopt, std::nullopt, 1);
        CHECK_EQ(lv.size(), (size_t)1);
        CHECK_EQ(lv[0].id, (int64_t)3);
        // 时空联合 + 层级
        auto combo = store.query_blocks(Aabb{0, 0, 0, 10, 10, 10}, 1200, 2500, 1);
        CHECK_EQ(combo.size(), (size_t)1);
        CHECK_EQ(combo[0].id, (int64_t)3);
    }

    // ============ 4. 删除：当前态可删，历史保留 ============
    {
        CHECK(store.remove_block(2));
        CHECK(!store.remove_block(2));  // 再删返回 false
        CHECK(!store.get_block(2).has_value());
    }

    // ============ 5. 版本链：append + seal + 时间回溯 ============
    {
        store.append_version(VoxelVersion{1, 1, "一座房子", 0.9, "Stable", 1000, std::nullopt});
        store.seal_version(1, 2000);
        store.append_version(VoxelVersion{1, 2, "一座两层房子", 0.95, "Stable", 2000,
                                          std::nullopt});
        auto history = store.versions_of(1);
        CHECK_EQ(history.size(), (size_t)2);
        CHECK_EQ(history[0].version, (int64_t)1);
        CHECK(history[0].valid_to && *history[0].valid_to == 2000);  // seal 生效
        CHECK(!history[1].valid_to.has_value());                     // 当前生效
        // 回溯：valid_from <= t < valid_to
        auto at1500 = store.version_at(1, 1500);
        CHECK(at1500 && at1500->payload == "一座房子");
        auto at2500 = store.version_at(1, 2500);
        CHECK(at2500 && at2500->payload == "一座两层房子");
        CHECK(!store.version_at(1, 500).has_value());  // 太早，无版本
        CHECK(!store.version_at(1, 2000 - 1)->payload.empty());
        CHECK(store.version_at(1, 1999)->version == 1);  // 边界：valid_to 不含
        CHECK(store.version_at(1, 2000)->version == 2);
    }

    // ============ 6. 动态实例：轨迹往返 ============
    {
        VoxelInstance car;
        car.id = 1;
        car.class_label = "car";
        car.bounds = Aabb{10, 20, 0, 12, 22, 2};
        car.latest = TrackPoint{3000, 11, 21, 1, 5.0, 0.0, 0.0};
        car.trajectory = {TrackPoint{1000, 1, 21, 1, 5.0, 0, 0},
                          TrackPoint{2000, 6, 21, 1, 5.0, 0, 0},
                          TrackPoint{3000, 11, 21, 1, 5.0, 0, 0}};
        car.sources = {42, 43};
        car.confidence = 0.8;
        car.state = "Active";
        car.last_seen = 3000;
        store.put_instance(car);

        auto got = store.get_instance(1);
        CHECK(got.has_value());
        CHECK_EQ(got->class_label, "car");
        CHECK_EQ(got->latest.px, 11.0);
        CHECK_EQ(got->trajectory.size(), (size_t)3);
        CHECK_EQ(got->trajectory[1].px, 6.0);
        CHECK_EQ(got->trajectory[2].t, (int64_t)3000);
        CHECK_EQ(got->sources.size(), (size_t)2);
        CHECK_EQ(got->state, "Active");
        CHECK_EQ(got->stationary_since, (int64_t)-1);
        CHECK_EQ(store.all_instances().size(), (size_t)1);
        // upsert：状态推进 Active -> Stationary
        car.state = "Stationary";
        car.stationary_since = 4000;
        store.put_instance(car);
        auto got2 = store.get_instance(1);
        CHECK_EQ(got2->state, "Stationary");
        CHECK_EQ(got2->stationary_since, (int64_t)4000);
    }

    // ============ 7. meta：id 发生器 / 全局参数 ============
    {
        CHECK_EQ(store.get_meta("next_id", 1), (int64_t)1);  // 默认值
        store.set_meta("next_id", 100);
        CHECK_EQ(store.get_meta("next_id", 1), (int64_t)100);
        store.set_meta("next_id", 101);  // 覆盖
        CHECK_EQ(store.get_meta("next_id", 1), (int64_t)101);
    }
}
