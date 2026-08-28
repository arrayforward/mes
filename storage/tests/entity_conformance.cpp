#include "entity_conformance.h"

#include "entitytree/entity_store.h"
#include "entitytree/errors.h"
#include "test_framework.h"

using namespace entitytree;

namespace {

Observation make_obs(const std::string& id, const std::string& key,
                     const std::string& value, double conf,
                     const std::string& src, const std::string& anchor, int64_t ts) {
    Observation o;
    o.observation_id = id;
    o.attribute_key = key;
    o.value = value;
    o.confidence = conf;
    o.source_id = src;
    o.anchor_ref = anchor;
    o.timestamp = ts;
    return o;
}

AttrProfile make_profile(const std::string& id, const std::string& anchor,
                         int64_t bucket, const std::string& status, json attrs,
                         int64_t seq) {
    AttrProfile p;
    p.profile_id = id;
    p.anchor_ref = anchor;
    p.time_bucket = bucket;
    p.attributes = std::move(attrs);
    p.info_score = 1.5;
    p.status = status;
    p.seq = seq;
    return p;
}

json attr_entry(const std::string& value, double conf, const std::string& src,
                int64_t obs_seq, int64_t ts) {
    return json{{"value", value}, {"confidence", conf}, {"source_id", src},
                {"obs_seq", obs_seq}, {"ts", ts}};
}

} // namespace

// 纯存储一致性套件：只测 EntityStore 的 CRUD 与查询原语，
// 浮现/合并/拆分/检索排序等算法场景在 entity 组件的 resolver 套件里。
void run_entity_conformance(EntityStore& store) {
    // ============ 1. 观测：append / get / 索引查询 / append-only ============
    int64_t s1 = store.append_observation(make_obs("ob-1", "plate_number", "沪A12345",
                                                   0.95, "cam1", "gate-1", 100));
    int64_t s2 = store.append_observation(make_obs("ob-2", "color", "red",
                                                   0.9, "cam1", "gate-1", 110));
    int64_t s3 = store.append_observation(make_obs("ob-3", "plate_number", "沪A12345",
                                                   0.92, "cam2", "gate-1", 350));
    {
        CHECK(s1 > 0 && s2 > s1 && s3 > s2);            // 自增 seq 单调
        auto o = store.get_observation("ob-1");
        CHECK_EQ(o.seq, s1);
        CHECK_EQ(o.attribute_key, "plate_number");
        CHECK_EQ(o.value, "沪A12345");
        CHECK(o.confidence > 0.94 && o.confidence < 0.96);
        CHECK_EQ(o.source_id, "cam1");
        CHECK_EQ(o.anchor_ref, "gate-1");
        CHECK_EQ(o.timestamp, (int64_t)100);
        CHECK_EQ(o.event_ref, "");
        CHECK_THROWS_AS(store.get_observation("ob-x"), NotFoundError);
        CHECK_THROWS_AS(store.append_observation(make_obs("ob-1", "k", "v", 1.0,
                                                          "s", "a", 0)),
                        DuplicateError);                // 同 id 重复追加拒绝
        // 索引查询
        CHECK_EQ(store.observations_of("gate-1").size(), (size_t)3);
        CHECK(store.observations_of("gate-1")[0].seq <
              store.observations_of("gate-1")[1].seq);  // seq 升序
        CHECK_EQ(store.query_observations("plate_number", {}, {}).size(), (size_t)2);
        CHECK_EQ(store.query_observations({}, "cam1", {}).size(), (size_t)2);
        CHECK_EQ(store.query_observations("plate_number", "cam2", {}).size(), (size_t)1);
        CHECK(store.query_observations("plate_number", {}, "gate-2").empty());
    }

    // ============ 2. 侧写：upsert 当前态 + 倒排索引整组替换 ============
    json attrs1;
    attrs1["plate_number"] = json::array({attr_entry("沪A12345", 0.95, "cam1", s1, 100)});
    attrs1["color"] = json::array({attr_entry("red", 0.9, "cam1", s2, 110)});
    store.upsert_profile(make_profile("ap-1", "gate-1", 0, "linked", attrs1, s1));
    store.upsert_profile(make_profile("ap-2", "gate-1", 1, "linked", attrs1, s3));
    store.upsert_profile(make_profile("ap-3", "gate-2", 0, "pool", json::object(), 0));
    {
        auto p = store.get_profile("ap-1");
        CHECK_EQ(p.anchor_ref, "gate-1");
        CHECK_EQ(p.time_bucket, (int64_t)0);
        CHECK_EQ(p.status, "linked");
        CHECK_EQ(p.attributes.size(), (size_t)2);
        CHECK_EQ(p.attributes["plate_number"][0].value("value", ""), "沪A12345");
        CHECK_THROWS_AS(store.get_profile("ap-x"), NotFoundError);
        // 时空窗口 × 状态组合查询
        CHECK_EQ(store.query_profiles("gate-1", {}, {}, {}).size(), (size_t)2);
        CHECK_EQ(store.query_profiles({}, 0, 0, {}).size(), (size_t)2);   // ap-1 + ap-3
        CHECK_EQ(store.query_profiles({}, {}, {}, "pool").size(), (size_t)1);
        CHECK_EQ(store.query_profiles({}, {}, {}, "linked").size(), (size_t)2);
        // 属性召回（倒排索引等值查询）
        CHECK_EQ(store.find_profiles_by_attribute("plate_number", "沪A12345").size(),
                 (size_t)2);
        CHECK_EQ(store.find_profiles_by_attribute("color", "red").size(), (size_t)2);
        CHECK(store.find_profiles_by_attribute("color", "blue").empty());
        // upsert 覆盖：改状态 + 减属性，倒排索引整组替换
        store.upsert_profile(make_profile("ap-3", "gate-2", 0, "emerged", json::object(), 0));
        CHECK_EQ(store.get_profile("ap-3").status, "emerged");
        json attrs2;
        attrs2["plate_number"] = json::array({attr_entry("沪B666", 0.8, "cam3", 0, 50)});
        store.upsert_profile(make_profile("ap-1", "gate-1", 0, "linked", attrs2, s1));
        CHECK_EQ(store.find_profiles_by_attribute("plate_number", "沪A12345").size(),
                 (size_t)1);   // ap-1 的旧值不再命中
        CHECK(store.find_profiles_by_attribute("color", "red").size() == 1);
        CHECK_EQ(store.find_profiles_by_attribute("plate_number", "沪B666").size(),
                 (size_t)1);
        // level：默认 0，粗桶镜像 upsert 后按 level 过滤
        CHECK_EQ(store.get_profile("ap-1").level, 0);
        AttrProfile coarse = make_profile("ap-1L1", "gate-1", 0, "linked", attrs2, s1 + 100);
        coarse.level = 1;
        store.upsert_profile(coarse);
        CHECK_EQ(store.get_profile("ap-1L1").level, 1);
        CHECK_EQ(store.query_profiles("gate-1", {}, {}, {}, 0).size(), (size_t)2);
        CHECK_EQ(store.query_profiles("gate-1", {}, {}, {}, 1).size(), (size_t)1);
        CHECK_EQ(store.query_profiles("gate-1", {}, {}, {}, {}).size(), (size_t)3);
        CHECK_EQ(store.query_profiles({}, {}, {}, "linked", 1).size(), (size_t)1);
    }

    // ============ 3. 实体：upsert 当前态 + merged view 缓存 + updated_at ============
    EntityNode e1;
    e1.entity_id = "en-1";
    e1.name = "嫌疑车A";
    e1.type = "vehicle";
    e1.status = "candidate";
    e1.updated_at = 100;
    store.upsert_entity(e1);   // 空 view 建行
    {
        auto e = store.get_entity("en-1");
        CHECK_EQ(e.name, "嫌疑车A");
        CHECK_EQ(e.type, "vehicle");
        CHECK_EQ(e.view_version, (int64_t)0);
        CHECK_EQ(e.updated_at, (int64_t)100);
        CHECK_THROWS_AS(store.get_entity("en-x"), NotFoundError);
    }
    EntityNode e1v = e1;
    e1v.attributes["plate_number"] = json::array({{{"value", "沪A12345"}, {"weight", 2.0}}});
    e1v.attributes["color"] = json::array({{{"value", "red"}, {"weight", 1.0}}});
    e1v.view_version = 1;
    e1v.credibility = 2.0 / 3.0;
    e1v.updated_at = 350;
    store.upsert_entity(e1v);
    EntityNode e2;
    e2.entity_id = "en-2";
    e2.attributes["plate_number"] = json::array({{{"value", "沪B666"}, {"weight", 1.0}}});
    e2.credibility = 0.5;
    e2.updated_at = 50;
    store.upsert_entity(e2);
    {
        auto e = store.get_entity("en-1");
        CHECK_EQ(e.view_version, (int64_t)1);
        CHECK(e.credibility > 0.66 && e.credibility < 0.67);
        CHECK_EQ(e.updated_at, (int64_t)350);
        CHECK_EQ(e.attributes["plate_number"][0].value("weight", 0.0), 2.0);
        // 实体属性召回（倒排索引）
        CHECK_EQ(store.find_entities_by_attribute("plate_number", "沪A12345").size(),
                 (size_t)1);
        CHECK_EQ(store.find_entities_by_attribute("color", "red").size(), (size_t)1);
        // view 重建后旧值不再命中
        EntityNode e1w = e1v;
        e1w.attributes = json::object();
        e1w.attributes["color"] = json::array({{{"value", "blue"}, {"weight", 1.0}}});
        e1w.view_version = 2;
        store.upsert_entity(e1w);
        CHECK(store.find_entities_by_attribute("plate_number", "沪A12345").empty());
        CHECK_EQ(store.find_entities_by_attribute("color", "blue").size(), (size_t)1);
    }

    // ============ 4. 绑定：append-only / latest wins / 有效绑定语义 ============
    {
        EntityBinding bad;
        bad.binding_id = "bd-x";
        bad.profile_id = "ap-x";   // 侧写不存在
        bad.entity_id = "en-1";
        CHECK_THROWS_AS(store.append_binding(bad), NotFoundError);
        bad.profile_id = "ap-1";
        bad.entity_id = "en-x";    // 实体不存在
        CHECK_THROWS_AS(store.append_binding(bad), NotFoundError);
    }
    EntityBinding b1{"bd-1", "ap-1", "en-1", 1.0, "merge", "initial", 0};
    EntityBinding b2{"bd-2", "ap-2", "en-1", 0.9, "merge", "auto", 0};
    EntityBinding b3{"bd-3", "ap-2", "en-2", 0.85, "split", "corrected", 0};
    int64_t bs1 = store.append_binding(b1);
    int64_t bs2 = store.append_binding(b2);
    int64_t bs3 = store.append_binding(b3);
    {
        CHECK(bs1 > 0 && bs2 > bs1 && bs3 > bs2);
        // 全历史保留，按 seq 升序
        auto hist = store.bindings_of("ap-2");
        CHECK_EQ(hist.size(), (size_t)2);
        CHECK_EQ(hist[0].kind, "merge");
        CHECK_EQ(hist[1].kind, "split");
        // latest wins：重消歧后 ap-2 有效指向 en-2
        auto eff = store.effective_binding("ap-2");
        CHECK(eff.has_value());
        CHECK_EQ(eff->entity_id, "en-2");
        CHECK_EQ(store.effective_binding("ap-1")->entity_id, "en-1");
        CHECK(!store.effective_binding("ap-3").has_value());
        // 实体→侧写反查：有效绑定语义，被纠正走的 ap-2 不再属于 en-1
        auto ps1 = store.profiles_of_entity("en-1");
        CHECK_EQ(ps1.size(), (size_t)1);
        CHECK_EQ(ps1[0].profile_id, "ap-1");
        auto ps2 = store.profiles_of_entity("en-2");
        CHECK_EQ(ps2.size(), (size_t)1);
        CHECK_EQ(ps2[0].profile_id, "ap-2");
        // 历史不因此消失
        CHECK_EQ(store.bindings_of("ap-2").size(), (size_t)2);
        CHECK_EQ(store.observations_of("gate-1").size(), (size_t)3);
    }

    // ============ 5. 来源可靠性：et_sources（entitytree ↔ stmb 共享表） ============
    SourceRecord src1;
    src1.source_id = "cam1";
    // 默认值：reliability=1.0 / updated_at=0 / meta={}
    store.upsert_source(src1);
    SourceRecord src2;
    src2.source_id = "cam2";
    src2.reliability = 0.6;
    src2.updated_at = 1000;
    src2.meta = json{{"vendor", "stmb"}, {"kind", "camera"}};
    store.upsert_source(src2);
    {
        auto g1 = store.get_source("cam1");
        CHECK(g1.has_value());
        CHECK(g1->reliability > 0.99);
        CHECK_EQ(g1->updated_at, (int64_t)0);
        auto g2 = store.get_source("cam2");
        CHECK(g2.has_value());
        CHECK(g2->reliability > 0.59 && g2->reliability < 0.61);
        CHECK_EQ(g2->updated_at, (int64_t)1000);
        CHECK_EQ(g2->meta.value("vendor", ""), "stmb");
        CHECK(!store.get_source("cam-x").has_value());   // 未注册 → nullopt
        // upsert 覆盖：可靠性随运行调整
        src2.reliability = 0.62;
        src2.updated_at = 2000;
        store.upsert_source(src2);
        CHECK(store.get_source("cam2")->reliability > 0.61);
        CHECK_EQ(store.get_source("cam2")->updated_at, (int64_t)2000);
        auto all = store.list_sources();
        CHECK_EQ(all.size(), (size_t)2);
        CHECK_EQ(all[0].source_id, "cam1");   // 按 source_id 升序
        CHECK_EQ(all[1].source_id, "cam2");
    }
}
