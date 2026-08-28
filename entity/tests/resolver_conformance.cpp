#include "resolver_conformance.h"

#include <cmath>
#include <optional>

#include "entity/geocore_distance.h"
#include "entity/resolver.h"
#include "entitytree/errors.h"
#include "test_framework.h"

using namespace entity;
using namespace entitytree;

namespace {

Observation make_obs(const std::string& key, const std::string& value, double conf,
                     const std::string& src, const std::string& anchor, int64_t ts) {
    Observation o;  // observation_id 留空，由 Resolver 生成
    o.attribute_key = key;
    o.value = value;
    o.confidence = conf;
    o.source_id = src;
    o.anchor_ref = anchor;
    o.timestamp = ts;
    return o;
}

bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

/// merged view 中某 (key,value) 的权重；不存在返回 -1。
double view_weight(const EntityNode& e, const std::string& key, const std::string& value) {
    if (!e.attributes.contains(key)) return -1.0;
    for (const auto& en : e.attributes[key])
        if (en.value("value", "") == value) return en.value("weight", 0.0);
    return -1.0;
}

bool explanation_has(const SearchHit& h, const std::string& key) {
    for (const auto& en : h.explanation)
        if (en.value("key", "") == key) return true;
    return false;
}

} // namespace

void run_resolver_conformance(EntityStore& store) {
    // 主解析器：plate_number 为强标识（权重 3），其余权重 1
    ResolverConfig cfg;
    cfg.bucket_size = 300;
    cfg.theta_form = 3.0;
    cfg.attribute_weight = {{"plate_number", 3.0}, {"color", 1.0},
                            {"brand", 1.0}, {"model", 1.0}};
    Resolver r1(store, cfg);

    // ============ 1. 单源连续观测 → info_score 累积 → θ_form 浮现 → 自动新建实体 ============
    std::string p0 = r1.ingest(make_obs("plate_number", "沪X000", 1.0, "cam0", "gate-0", 10));
    {
        auto p = store.get_profile(p0);
        CHECK_EQ(p.status, "pool");                       // 3*log1p(1)≈2.08 < 3.0
        CHECK(near(p.info_score, 3.0 * std::log1p(1.0)));
        CHECK_EQ(p.time_bucket, (int64_t)0);
    }
    std::string p0b = r1.ingest(make_obs("color", "black", 1.0, "cam0", "gate-0", 20));
    CHECK_EQ(p0b, p0);                                    // 同时空窗口 → 同一侧写
    CHECK_EQ(store.get_profile(p0).status, "pool");       // 4*log1p(1)≈2.77 < 3.0
    r1.ingest(make_obs("brand", "T", 1.0, "cam0", "gate-0", 30));
    // 5*log1p(1)≈3.47 ≥ 3.0 → 浮现并自动解析：无候选 → 新建实体
    CHECK_EQ(store.get_profile(p0).status, "linked");
    auto eff0 = store.effective_binding(p0);
    CHECK(eff0.has_value());
    CHECK_EQ(eff0->kind, "merge");
    std::string e0 = eff0->entity_id;
    {
        auto e = store.get_entity(e0);
        CHECK_EQ(e.status, "candidate");
        CHECK_EQ(e.view_version, (int64_t)1);
        CHECK_EQ(e.updated_at, (int64_t)30);              // 基准时间 = 最新观测时间
        CHECK(near(view_weight(e, "plate_number", "沪X000"), 1.0));
        // credibility = 绑定置信度均值(1.0) × 来源多样性(1/(1+1)) = 0.5
        CHECK(near(e.credibility, 0.5));
    }
    // 已 linked 的侧写继续累积观测（镜像继续更新，不再触发浮现）
    r1.ingest(make_obs("model", "M", 1.0, "cam0", "gate-0", 40));
    {
        auto p = store.get_profile(p0);
        CHECK_EQ(p.status, "linked");
        CHECK_EQ(p.attributes.size(), (size_t)4);
    }

    // ============ 2. 双源、同锚点相邻桶、共享强标识 → 合并为同一实体，侧写镜像都保留 ============
    std::string p1 = r1.ingest(make_obs("plate_number", "沪A12345", 1.0, "cam1", "gate-1", 100));
    r1.ingest(make_obs("color", "red", 1.0, "cam1", "gate-1", 110));
    r1.ingest(make_obs("brand", "VW", 1.0, "cam1", "gate-1", 120));
    CHECK_EQ(store.get_profile(p1).status, "linked");
    std::string e1 = store.effective_binding(p1)->entity_id;

    std::string p2 = r1.ingest(make_obs("plate_number", "沪A12345", 1.0, "cam2", "gate-1", 350));
    r1.ingest(make_obs("color", "red", 1.0, "cam2", "gate-1", 360));
    r1.ingest(make_obs("brand", "VW", 1.0, "cam2", "gate-1", 370));
    CHECK(p2 != p1);                                       // 不同时间桶 → 不同侧写
    CHECK_EQ(store.get_profile(p2).status, "linked");
    {
        auto b2 = store.bindings_of(p2);
        CHECK_EQ(b2.size(), (size_t)1);
        CHECK_EQ(b2[0].kind, "merge");
        CHECK(b2[0].confidence >= cfg.theta_merge);        // 完全一致 → score=1.0
        CHECK_EQ(b2[0].entity_id, e1);                     // 合并到 p1 的实体
        CHECK_EQ(store.effective_binding(p2)->entity_id, e1);
        // 两个侧写镜像都保留
        CHECK_EQ(store.profiles_of_entity(e1).size(), (size_t)2);
        CHECK_EQ(store.get_profile(p1).attributes.size(), (size_t)3);
        CHECK_EQ(store.get_profile(p2).attributes.size(), (size_t)3);
        auto e = store.get_entity(e1);
        CHECK_EQ(e.view_version, (int64_t)2);              // 合并后重建
        CHECK_EQ(e.updated_at, (int64_t)370);
        CHECK(near(view_weight(e, "plate_number", "沪A12345"), 2.0));  // 双源加权
        // credibility = 1.0 × 2/(1+2) ≈ 0.667
        CHECK(near(e.credibility, 2.0 / 3.0));
    }

    // ============ 3. 误合并后拆分：双实体各自重建，绑定历史完整 ============
    std::string e2 = r1.split(p2, "嫌疑车B");
    {
        CHECK(e2 != e1);
        CHECK_EQ(store.profiles_of_entity(e1).size(), (size_t)1);
        CHECK_EQ(store.profiles_of_entity(e2).size(), (size_t)1);
        CHECK_EQ(store.profiles_of_entity(e1)[0].profile_id, p1);
        CHECK_EQ(store.profiles_of_entity(e2)[0].profile_id, p2);
        // append-only：merge + split 两条历史全部保留，latest wins
        auto hist = store.bindings_of(p2);
        CHECK_EQ(hist.size(), (size_t)2);
        CHECK_EQ(hist[0].kind, "merge");
        CHECK_EQ(hist[1].kind, "split");
        CHECK(hist[0].seq < hist[1].seq);
        CHECK_EQ(store.effective_binding(p2)->entity_id, e2);
        // 双实体 merged view 各自重建
        auto ea = store.get_entity(e1);
        auto eb = store.get_entity(e2);
        CHECK(near(view_weight(ea, "plate_number", "沪A12345"), 1.0));
        CHECK(near(view_weight(eb, "plate_number", "沪A12345"), 1.0));
        CHECK_EQ(eb.name, "嫌疑车B");
        CHECK(near(ea.credibility, 0.5));                  // 各自回到单源单绑定
        CHECK(near(eb.credibility, 0.5));
    }

    // ============ 4. 低可信观测：可浮现，但实体 credibility 明显更低 ============
    std::string p3 = r1.ingest(make_obs("plate_number", "沪B222", 0.2, "cam3", "gate-2", 100));
    r1.ingest(make_obs("color", "blue", 0.2, "cam3", "gate-2", 110));
    r1.ingest(make_obs("brand", "Y", 0.2, "cam3", "gate-2", 120));
    CHECK_EQ(store.get_profile(p3).status, "linked");      // info_score 与置信度无关，照常浮现
    std::string e3 = store.effective_binding(p3)->entity_id;
    {
        auto e = store.get_entity(e3);
        CHECK(near(e.credibility, 0.1));                   // 0.2 × 1/2
        CHECK(e.credibility < store.get_entity(e1).credibility);
        CHECK(near(view_weight(e, "plate_number", "沪B222"), 0.2));  // 低置信度加权
    }

    // ============ 5. 存疑区：score 落在 θ_split 与 θ_merge 之间 → 不合并，标 disputed ============
    ResolverConfig cfg2;
    cfg2.bucket_size = 300;
    cfg2.theta_form = 3.0;
    cfg2.attribute_weight = {{"plate_number", 2.0}, {"color", 1.0},
                             {"brand", 1.0}, {"model", 1.0}};
    Resolver r2(store, cfg2);
    std::string pa = r2.ingest(make_obs("plate_number", "沪C111", 1.0, "s1", "lot-a", 0));
    r2.ingest(make_obs("color", "red", 1.0, "s1", "lot-a", 10));
    r2.ingest(make_obs("brand", "X", 1.0, "s1", "lot-a", 20));
    r2.ingest(make_obs("model", "Y", 1.0, "s1", "lot-a", 30));
    std::string ea_id = store.effective_binding(pa)->entity_id;
    CHECK_EQ(store.get_entity(ea_id).status, "candidate");
    // 与 ea 共享 color/model（弱匹配），plate/brand 冲突：score = ((2-3)/5+1)/2 + 0.05 = 0.45
    std::string pd = r2.ingest(make_obs("plate_number", "沪D999", 1.0, "s2", "lot-a", 400));
    r2.ingest(make_obs("color", "red", 1.0, "s2", "lot-a", 410));
    r2.ingest(make_obs("brand", "Z", 1.0, "s2", "lot-a", 420));
    r2.ingest(make_obs("model", "Y", 1.0, "s2", "lot-a", 430));
    {
        CHECK_EQ(store.get_profile(pd).status, "emerged"); // 存疑：保持 emerged 等更多证据
        CHECK(store.bindings_of(pd).empty());              // 未追加任何绑定
        CHECK_EQ(store.get_entity(ea_id).status, "disputed");
        CHECK_EQ(store.profiles_of_entity(ea_id).size(), (size_t)1);  // 原绑定不受影响
    }

    // ============ 6. 检索：排序 / topk / min_credibility / explanation ============
    {
        // 单属性：e1、e2 都含 plate_number=沪A12345（credibility 均为 0.5）
        auto hits = r1.search({{"plate_number", "沪A12345"}}, 10);
        CHECK_EQ(hits.size(), (size_t)2);
        CHECK(near(hits[0].match_score, 1.0));
        CHECK(near(hits[0].final_score, 0.7 * 1.0 + 0.3 * 0.5));
        CHECK(explanation_has(hits[0], "plate_number"));
        CHECK_EQ(r1.search({{"plate_number", "沪A12345"}}, 1).size(), (size_t)1);  // topk 截断
        CHECK(r1.search({{"plate_number", "沪A12345"}}, 10, 0.6).empty());         // 可信度过滤
        // 低可信实体：放宽过滤可召回，收紧则被滤掉
        CHECK_EQ(r1.search({{"plate_number", "沪B222"}}, 10, 0.05).size(), (size_t)1);
        CHECK(r1.search({{"plate_number", "沪B222"}}, 10, 0.5).empty());
        // 多属性：e1/e2 全中（match=1.0），ea 只中 color（match=1/4）
        auto multi = r1.search({{"plate_number", "沪A12345"}, {"color", "red"}}, 10);
        CHECK_EQ(multi.size(), (size_t)3);
        CHECK(near(multi[0].match_score, 1.0));
        CHECK(near(multi[2].match_score, 0.25));
        CHECK(multi[0].final_score > multi[2].final_score);
        CHECK_EQ(multi[2].entity_id, ea_id);
        auto top2 = r1.search({{"plate_number", "沪A12345"}, {"color", "red"}}, 2);
        CHECK_EQ(top2.size(), (size_t)2);
        CHECK(top2[0].entity_id != ea_id && top2[1].entity_id != ea_id);
    }

    // ============ 7. append-only 铁律：观测与绑定只有追加，历史全部可查 ============
    {
        CHECK_EQ(store.observations_of("gate-1").size(), (size_t)6);   // 双源各 3 条
        CHECK_EQ(store.query_observations({}, "cam1", {}).size(), (size_t)3);
        CHECK_EQ(store.query_observations({}, "cam2", {}).size(), (size_t)3);
        // 重消歧（split）之后：绑定历史仍是完整两条，且侧写镜像未丢任何属性
        CHECK_EQ(store.bindings_of(p2).size(), (size_t)2);
        CHECK_EQ(store.get_profile(p2).attributes.size(), (size_t)3);
        // 属性召回走倒排索引
        CHECK_EQ(store.find_profiles_by_attribute("plate_number", "沪A12345").size(),
                 (size_t)2);
        CHECK(store.find_profiles_by_attribute("plate_number", "沪Z000").empty());
    }

    // ============ 8. 层级邻近召回（geocore AnchorPath 路径码） ============
    // r3：低浮现/合并阈值隔离 blocking 行为（候选侧写与实体无公共属性 key，
    // 属性召回恒为空——能否召回只取决于锚点邻近性），min_lca_depth=1。
    ResolverConfig cfg3;
    cfg3.bucket_size = 300;
    cfg3.theta_form = 1.0;
    cfg3.theta_merge = 0.5;   // 无公共 key 时 score=0.5+来源加分(0.05) → 合并
    cfg3.theta_split = 0.3;
    cfg3.min_lca_depth = 1;
    cfg3.attribute_weight = {{"plate_number", 3.0}};
    Resolver r3(store, cfg3);

    // 8a. 侧写在后代锚点（"1/1/2/3"）、实体在祖先锚点（"1/1"）：
    //     IsPrefixOf 前缀邻近 → 召回并合并（"房间"→"楼"→"街区"语义）
    std::string a1 = r3.ingest(make_obs("plate_number", "沪P001", 1.0, "q1", "1/1", 6000));
    std::string e_a = store.effective_binding(a1)->entity_id;
    std::string b1 = r3.ingest(make_obs("face_desc", "fx1", 1.0, "q2", "1/1/2/3", 6300));
    r3.ingest(make_obs("jacket", "jy1", 1.0, "q2", "1/1/2/3", 6310));
    {
        auto eff = store.effective_binding(b1);
        CHECK(eff.has_value());
        CHECK_EQ(eff->entity_id, e_a);              // 祖先锚点实体被召回并合并
        CHECK_EQ(eff->kind, "merge");
        CHECK_EQ(store.profiles_of_entity(e_a).size(), (size_t)2);
        CHECK(store.get_entity(e_a).attributes.contains("face_desc"));
    }

    // 8b. LCA 邻近：兄弟锚点（LCA=1 ≥ 1）召回合并；远亲锚点（LCA=0 < 1）不召回
    std::string a2 = r3.ingest(make_obs("plate_number", "沪P003", 1.0, "q1", "1/2", 7000));
    std::string e_b = store.effective_binding(a2)->entity_id;
    std::string b2 = r3.ingest(make_obs("face_desc", "fx2", 1.0, "q2", "2/9/9", 7300));
    r3.ingest(make_obs("jacket", "jy2", 1.0, "q2", "2/9/9", 7310));
    {
        auto eff = store.effective_binding(b2);
        CHECK(eff.has_value());
        CHECK(eff->entity_id != e_b);               // LCA=0 < min_lca_depth → 新建实体
        CHECK_EQ(store.profiles_of_entity(e_b).size(), (size_t)1);
    }
    std::string b3 = r3.ingest(make_obs("face_desc", "fx3", 1.0, "q2", "1/3", 7350));
    r3.ingest(make_obs("jacket", "jy3", 1.0, "q2", "1/3", 7360));
    {
        CHECK_EQ(store.effective_binding(b3)->entity_id, e_b);  // 兄弟锚点 LCA=1 → 合并
        CHECK_EQ(store.profiles_of_entity(e_b).size(), (size_t)2);
    }

    // 8c. 非法 anchor（非路径码）降级为字符串等值，行为与升级前一致
    std::string c1 = r3.ingest(make_obs("plate_number", "沪P005", 1.0, "q1",
                                        "camera-north", 8000));
    std::string e_c = store.effective_binding(c1)->entity_id;
    std::string c2 = r3.ingest(make_obs("face_desc", "fx4", 1.0, "q2",
                                        "camera-north", 8300));
    r3.ingest(make_obs("jacket", "jy4", 1.0, "q2", "camera-north", 8310));
    {
        CHECK_EQ(store.effective_binding(c2)->entity_id, e_c);  // 字符串相等 → 合并
        CHECK_EQ(store.profiles_of_entity(e_c).size(), (size_t)2);
    }
    std::string c3 = r3.ingest(make_obs("face_desc", "fx5", 1.0, "q2",
                                        "camera-south", 8300));
    r3.ingest(make_obs("jacket", "jy5", 1.0, "q2", "camera-south", 8310));
    {
        CHECK(store.effective_binding(c3)->entity_id != e_c);   // 字符串不等 → 新建
        CHECK_EQ(store.profiles_of_entity(e_c).size(), (size_t)2);
    }

    // ============ 9. 可信度半衰期衰减（stmb 纪律：查询有效值，基准不改写） ============
    ResolverConfig cfg4;
    cfg4.bucket_size = 300;
    cfg4.theta_form = 1.0;
    cfg4.credibility_half_life = 100;   // 秒
    cfg4.attribute_weight = {{"plate_number", 3.0}};
    Resolver r4(store, cfg4);
    std::string d1 = r4.ingest(make_obs("plate_number", "沪D001", 1.0, "dcam",
                                        "dock-1", 1000));
    std::string e_d = store.effective_binding(d1)->entity_id;
    {
        double base = store.get_entity(e_d).credibility;   // 1.0 × 1/2 = 0.5
        CHECK(near(base, 0.5));
        CHECK_EQ(store.get_entity(e_d).updated_at, (int64_t)1000);
        CHECK(near(r4.effective_credibility(e_d, 1000), base));        // t0 = 基准值
        CHECK(near(r4.effective_credibility(e_d, 1100), base * 0.5));  // 一个半衰期折半
        CHECK(near(r4.effective_credibility(e_d, 1200), base * 0.25)); // 两个
        CHECK(near(r4.effective_credibility(e_d, 900), base));         // elapsed≤0 → 基准值
        // 存储基准值不因查询衰减被改写
        CHECK(near(store.get_entity(e_d).credibility, base));
        // 未配半衰期的解析器（r1）：任意时刻都返回基准值
        CHECK(near(r1.effective_credibility(e_d, 100000), base));
    }

    // 属性新鲜度：陈旧属性命中的 match 得分低于新鲜属性命中。
    // 直接落存储构造两个实体（credibility 相同，隔离 match 差异）：
    //   en-fresh 的 color 观测于 t=1000，en-stale 的 color 观测于 t=500。
    ResolverConfig cfg5;
    cfg5.attribute_half_life = {{"color", 100}};   // 只有 color 衰减
    Resolver r5(store, cfg5);
    auto plant = [&](const std::string& pid, const std::string& eid,
                     const std::string& anchor, int64_t ts) {
        AttrProfile p;
        p.profile_id = pid;
        p.anchor_ref = anchor;
        p.time_bucket = ts / 300;
        p.status = "linked";
        p.seq = ts;
        p.attributes["color"] = json::array(
            {json{{"value", "red"}, {"confidence", 1.0}, {"source_id", "sx"},
                  {"obs_seq", 1}, {"ts", ts}}});
        store.upsert_profile(p);
        EntityNode e;
        e.entity_id = eid;
        e.credibility = 0.5;
        e.updated_at = ts;
        e.attributes["color"] = json::array({json{{"value", "red"}, {"weight", 1.0}}});
        store.upsert_entity(e);
        store.append_binding(EntityBinding{"bd-" + pid, pid, eid, 1.0, "merge", "", 0});
    };
    plant("ap-fresh", "en-fresh", "yard-a", 1000);
    plant("ap-stale", "en-stale", "yard-b", 500);
    {
        // color=red 还命中早期场景的 e1/e2/ea（它们的 color 观测更陈旧），
        // 关键断言：新鲜实体排第一、陈旧实体排第二，且 match 按半衰期折算
        auto hits = r5.search({{"color", "red"}}, 10, 0.0, /*now=*/1100);
        CHECK(hits.size() >= (size_t)2);
        CHECK_EQ(hits[0].entity_id, "en-fresh");              // 新鲜属性命中排前
        CHECK(near(hits[0].match_score, 0.5));                // 0.5^(100/100)
        CHECK_EQ(hits[1].entity_id, "en-stale");
        CHECK(near(hits[1].match_score, 0.015625));           // 0.5^(600/100)
        CHECK(hits[0].final_score > hits[1].final_score);
        // now=0（旧签名）：不启用时效，前两名 match 同为 1.0
        auto hits0 = r5.search({{"color", "red"}}, 10);
        CHECK(hits0.size() >= (size_t)2);
        CHECK(near(hits0[0].match_score, 1.0));
        CHECK(near(hits0[1].match_score, 1.0));
        // 未配置半衰期的 key（plate_number）：任意 now 因子恒 1
        auto ph = r5.search({{"plate_number", "沪D001"}}, 10, 0.0, 100000);
        CHECK_EQ(ph.size(), (size_t)1);
        CHECK(near(ph[0].match_score, 1.0));
    }

    // ============ 10. 来源可靠性（entitytree ↔ stmb 共享 et_sources） ============
    ResolverConfig cfg6;
    cfg6.bucket_size = 300;
    cfg6.theta_form = 1.0;
    cfg6.attribute_weight = {{"plate_number", 3.0}};
    cfg6.source_reward = 0.02;
    cfg6.source_penalty = 0.05;
    Resolver r6(store, cfg6);
    // 10a. 低可靠来源浮现的实体 credibility 低于高可靠（未注册按 1.0）来源
    store.upsert_source(SourceRecord{"cam-low", 0.2, 0, json::object()});
    std::string sl = r6.ingest(make_obs("plate_number", "沪L001", 1.0, "cam-low",
                                        "src-a", 100));
    std::string e_low = store.effective_binding(sl)->entity_id;
    std::string sh = r6.ingest(make_obs("plate_number", "沪H001", 1.0, "cam-high",
                                        "src-b", 100));
    std::string e_high = store.effective_binding(sh)->entity_id;
    {
        CHECK(near(store.get_entity(e_low).credibility, 0.1));   // 0.2×0.5
        CHECK(near(store.get_entity(e_high).credibility, 0.5));  // 1.0×0.5
        CHECK(store.get_entity(e_low).credibility <
              store.get_entity(e_high).credibility);
    }
    // 10b. merge 成功 → 涉及来源 reliability +0.02（封项 1.0）
    store.upsert_source(SourceRecord{"cam-m1", 0.5, 0, json::object()});
    store.upsert_source(SourceRecord{"cam-m2", 0.5, 0, json::object()});
    std::string m1 = r6.ingest(make_obs("plate_number", "沪M001", 1.0, "cam-m1",
                                        "src-c", 1000));
    std::string e_m = store.effective_binding(m1)->entity_id;
    std::string m2 = r6.ingest(make_obs("plate_number", "沪M001", 1.0, "cam-m2",
                                        "src-c", 1350));
    {
        CHECK_EQ(store.effective_binding(m2)->entity_id, e_m);   // 合并成功
        CHECK(near(store.get_source("cam-m2")->reliability, 0.52));  // 0.5 + 0.02
        CHECK(near(store.get_source("cam-m1")->reliability, 0.5));   // 未被调的不动
        CHECK(store.get_source("cam-m2")->updated_at > 0);
    }
    // 10c. 先 merge（+0.02）后 split（−0.05）：两个方向都生效
    store.upsert_source(SourceRecord{"cam-sp", 0.5, 0, json::object()});
    std::string m3 = r6.ingest(make_obs("plate_number", "沪M001", 1.0, "cam-sp",
                                        "src-c", 1700));
    CHECK_EQ(store.effective_binding(m3)->entity_id, e_m);       // 再次合并
    CHECK(near(store.get_source("cam-sp")->reliability, 0.52));
    r6.split(m3, "拆分车");
    {
        CHECK(near(store.get_source("cam-sp")->reliability, 0.47));  // 0.52 − 0.05
        CHECK_EQ(store.profiles_of_entity(e_m).size(), (size_t)2);   // m3 被拆走
    }
    // 10d. 封底 0.05：0.06 − 0.05 → clamp 0.05
    store.upsert_source(SourceRecord{"cam-floor", 0.06, 0, json::object()});
    std::string pf = r6.ingest(make_obs("plate_number", "沪F001", 1.0, "cam-floor",
                                        "src-d", 100));
    r6.split(pf);
    CHECK(near(store.get_source("cam-floor")->reliability, 0.05));

    // ============ 11. 多分辨率时间桶（bucket_sizes 梯子 + rollup + blocking 回退） ============
    ResolverConfig cfg7;
    cfg7.bucket_sizes = {300, 3600};   // level 0 最细(5min)，level 1 粗桶(1h)
    cfg7.theta_form = 1.0;
    cfg7.theta_merge = 0.5;
    cfg7.theta_split = 0.3;
    cfg7.attribute_weight = {{"plate_number", 3.0}};
    Resolver r7(store, cfg7);

    // 11a. 细桶浮现合并照旧
    std::string ml1 = r7.ingest(make_obs("plate_number", "沪R001", 1.0, "s1", "ml-1", 100));
    std::string e_ml = store.effective_binding(ml1)->entity_id;
    std::string ml2 = r7.ingest(make_obs("plate_number", "沪R001", 1.0, "s2", "ml-1", 350));
    CHECK_EQ(store.effective_binding(ml2)->entity_id, e_ml);   // 强标识照旧合并

    // 11b. rollup：跨两个细桶的侧写聚合出 level 1 粗桶侧写，双级镜像都在
    {
        auto rolled = r7.rollup(e_ml, 1);
        CHECK_EQ(rolled.size(), (size_t)1);
        auto cp = store.get_profile(rolled[0]);
        CHECK_EQ(cp.level, 1);
        CHECK_EQ(cp.time_bucket, (int64_t)0);            // 两个细桶都落在粗桶 0
        CHECK_EQ(cp.status, "linked");
        CHECK_EQ(cp.attributes["plate_number"].size(), (size_t)2);  // 属性并集
        CHECK_EQ(store.profiles_of_entity(e_ml).size(), (size_t)3);  // 两级都返回
        // 细桶镜像原样保留；merged view 只按 level 0 计权（不重复计分）
        CHECK_EQ(store.get_profile(ml1).attributes.size(), (size_t)1);
        CHECK(near(view_weight(store.get_entity(e_ml), "plate_number", "沪R001"), 2.0));
        // 幂等：重复 rollup 不再生成
        CHECK(r7.rollup(e_ml, 1).empty());
        // 单级配置（r1）不可 rollup
        CHECK(r1.rollup(e_ml, 1).empty());
        CHECK(r1.rollup(e1, 1).empty());
    }

    // 11c. blocking 回退：细桶 ±1 无候选时经粗桶（覆盖 1h）召回常驻实体并合并
    std::string n1 = r7.ingest(make_obs("face_desc", "fm1", 1.0, "s3", "ml-1", 4000));
    r7.ingest(make_obs("jacket", "jm1", 1.0, "s3", "ml-1", 4010));
    {
        auto eff = store.effective_binding(n1);
        CHECK(eff.has_value());
        CHECK_EQ(eff->entity_id, e_ml);   // 细桶无候选 → level 1 粗桶召回 → 合并
        CHECK_EQ(store.profiles_of_entity(e_ml).size(), (size_t)4);
    }

    // ============ 12. 轨迹连续性校验（vmax 物理可达） ============
    // 合成距离提供者：site-a↔site-b 10km；site-a↔site-e 600m；其余无数据
    ResolverConfig cfg8;
    cfg8.theta_form = 1.0;
    cfg8.theta_merge = 0.75;
    cfg8.theta_split = 0.3;
    cfg8.attribute_weight = {{"plate_number", 3.0}};
    cfg8.max_speed = 20.0;                  // m/s
    cfg8.trajectory_check = "veto";
    Resolver r8v(store, cfg8);
    r8v.set_distance_provider(
        [](const std::string& a, const std::string& b) -> std::optional<double> {
            if ((a == "site-a" && b == "site-b") || (a == "site-b" && b == "site-a"))
                return 10000.0;
            if ((a == "site-a" && b == "site-e") || (a == "site-e" && b == "site-a"))
                return 600.0;
            return std::nullopt;
        });

    // 12a. veto：10km / 60s（vmax 20m/s 只能走 1200m）→ 物理不可能 → 不合并
    std::string t1 = r8v.ingest(make_obs("plate_number", "沪T001", 1.0, "v1", "site-a", 100));
    std::string e_t = store.effective_binding(t1)->entity_id;
    std::string t2 = r8v.ingest(make_obs("plate_number", "沪T001", 1.0, "v2", "site-b", 160));
    {
        CHECK(store.effective_binding(t2)->entity_id != e_t);   // 全部候选被否决 → 新建
        CHECK_EQ(store.profiles_of_entity(e_t).size(), (size_t)1);
    }

    // 12b. penalty：同一不可能轨迹压分进存疑区（1.0 × 0.5 = 0.5 < θ_merge）
    ResolverConfig cfg8p = cfg8;
    cfg8p.trajectory_check = "penalty";
    cfg8p.trajectory_penalty = 0.5;
    Resolver r8p(store, cfg8p);
    r8p.set_distance_provider(
        [](const std::string& a, const std::string& b) -> std::optional<double> {
            if ((a == "site-c" && b == "site-d") || (a == "site-d" && b == "site-c"))
                return 10000.0;
            return std::nullopt;
        });
    std::string u1 = r8p.ingest(make_obs("plate_number", "沪T002", 1.0, "v1", "site-c", 100));
    std::string e_u = store.effective_binding(u1)->entity_id;
    std::string u2 = r8p.ingest(make_obs("plate_number", "沪T002", 1.0, "v2", "site-d", 160));
    {
        CHECK_EQ(store.get_profile(u2).status, "emerged");   // 存疑：不合并
        CHECK(store.bindings_of(u2).empty());
        CHECK_EQ(store.get_entity(e_u).status, "disputed");
    }

    // 12c. 近距离（600m ≤ 1200m）→ 正常合并
    std::string w1 = r8v.ingest(make_obs("plate_number", "沪T003", 1.0, "v1", "site-a", 400));
    std::string e_w = store.effective_binding(w1)->entity_id;
    std::string w2 = r8v.ingest(make_obs("plate_number", "沪T003", 1.0, "v2", "site-e", 460));
    CHECK_EQ(store.effective_binding(w2)->entity_id, e_w);

    // 12d. provider 无数据（nullopt）→ 不约束 → 正常合并
    std::string x1 = r8v.ingest(make_obs("plate_number", "沪T004", 1.0, "v1", "site-x", 100));
    std::string e_x = store.effective_binding(x1)->entity_id;
    std::string x2 = r8v.ingest(make_obs("plate_number", "沪T004", 1.0, "v2", "site-y", 160));
    CHECK_EQ(store.effective_binding(x2)->entity_id, e_x);

    // 12e. max_speed=0 完全关闭：同样的不可能轨迹照常合并
    ResolverConfig cfg8z = cfg8;
    cfg8z.max_speed = 0.0;
    Resolver r8z(store, cfg8z);
    r8z.set_distance_provider(
        [](const std::string& a, const std::string& b) -> std::optional<double> {
            if ((a == "site-f" && b == "site-g") || (a == "site-g" && b == "site-f"))
                return 10000.0;
            return std::nullopt;
        });
    std::string z1 = r8z.ingest(make_obs("plate_number", "沪T005", 1.0, "v1", "site-f", 700));
    std::string e_z = store.effective_binding(z1)->entity_id;
    std::string z2 = r8z.ingest(make_obs("plate_number", "沪T005", 1.0, "v2", "site-g", 760));
    CHECK_EQ(store.effective_binding(z2)->entity_id, e_z);

    // 12f. geocore 适配器 smoke：Euclidean3D 场景两个已知间距锚点 → 距离 5000m
    {
        geocore::Kernel k;
        auto scene = k.CreateScene("factory", 0, geocore::AnchorPath{}.Append(1),
                                   geocore::Euclidean3D{549755813888.0});
        CHECK(scene != geocore::kInvalidScene);
        auto ga = k.CreateAnchor(geocore::AnchorPath{}.Append(1),
                                 geocore::Kinematic::Static(geocore::Vec3d{0.0, 0.0, 0.0}));
        auto gb = k.CreateAnchor(geocore::AnchorPath{}.Append(1),
                                 geocore::Kinematic::Static(
                                     geocore::Vec3d{3000.0, 4000.0, 0.0}));
        k.BeginFrame(0.0);
        auto provider = make_geocore_distance(k, 0.0);
        auto d = provider(geocore::ToString(ga), geocore::ToString(gb));
        CHECK(d.has_value());
        CHECK(near(*d, 5000.0, 0.01));
        CHECK(near(*provider(geocore::ToString(ga), geocore::ToString(ga)), 0.0));
        CHECK(!provider("site-a", "site-b").has_value());   // 非路径码 → nullopt
    }
}
