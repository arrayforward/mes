#include "conformance.h"

#include <unordered_map>

#include "eventstore/errors.h"
#include "eventstore/event_store.h"
#include "test_framework.h"

using namespace eventstore;

void run_conformance_suite(EventStore& store) {
    // ============ 1. 叙事：追加 / 读取往返 / 重复 / 缺失 ============
    auto na = Narrative::create("story", "勇者斗恶龙", {{"author", "test"}});
    store.append_narrative(na);
    {
        auto got = store.get_narrative(na.narrative_id);
        CHECK_EQ(got.kind, "story");
        CHECK_EQ(got.title, "勇者斗恶龙");
        CHECK_EQ(got.meta.at("author"), "test");
    }
    CHECK_THROWS_AS(store.append_narrative(na), DuplicateError);
    CHECK_THROWS_AS(store.get_narrative("na-none"), NotFoundError);

    // 外键：事件挂到不存在的叙事
    CHECK_THROWS_AS(store.append_event(Event::create("na-none", "x")), NotFoundError);

    // ============ 2. 一个故事两个事件，每个事件两个视角的侧写 ============
    auto ev1 = Event::create(na.narrative_id, "勇者拔剑");
    auto ev2 = Event::create(na.narrative_id, "恶龙袭击村庄");
    store.append_event(ev1);
    store.append_event(ev2);
    CHECK_EQ(store.events_of(na.narrative_id).size(), (size_t)2);

    auto p1a = Profile::create(ev1.event_id, "active", TimeRef::virtual_time("从前", 1),
                               "村口", "勇者", "拔出", "圣剑", {{"weapon", "圣剑"}});
    auto p1b = Profile::create(ev1.event_id, "observer", TimeRef::virtual_time("从前", 1),
                               "村口", "村民", "看到", "勇者拔剑");
    auto p2a = Profile::create(ev2.event_id, "active", TimeRef::virtual_time("第二天", 2),
                               "村庄上空", "恶龙", "袭击", "村庄");
    auto p2b = Profile::create(ev2.event_id, "passive", TimeRef::virtual_time("第二天", 2),
                               "村庄", "村民", "躲避", "恶龙");

    // 故意先追加"第二天"的侧写：验证 timeline 靠 Link 而非追加顺序排序
    int64_t s2a = store.append_profile(p2a);
    int64_t s2b = store.append_profile(p2b);
    int64_t s1a = store.append_profile(p1a);
    int64_t s1b = store.append_profile(p1b);
    CHECK(s2a < s2b && s2b < s1a && s1a < s1b);  // seq 全局单调递增

    // 读取往返：五要素 / 时间 / 图 / 载荷全部保真
    {
        auto got = store.get_profile(p1a.profile_id);
        CHECK_EQ(got.event_id, ev1.event_id);
        CHECK_EQ(got.perspective, "active");
        CHECK_EQ(got.subject, "勇者");
        CHECK_EQ(got.verb, "拔出");
        CHECK_EQ(got.object, "圣剑");
        CHECK_EQ(got.place, "村口");
        CHECK_EQ(got.time.kind, "virtual");
        CHECK_EQ(got.time.value, "从前");
        CHECK(got.time.ordinal && *got.time.ordinal == 1);
        CHECK_EQ(got.payload.at("weapon"), "圣剑");
        CHECK_EQ(got.graph.at("nodes").size(), (size_t)4);
        CHECK_EQ(got.graph.at("edges").size(), (size_t)3);
        CHECK_EQ(got.seq, s1a);
    }
    CHECK_THROWS_AS(store.get_profile("pf-none"), NotFoundError);

    // 同一事件的两个视角，按 seq 升序
    {
        auto ps = store.profiles_of(ev1.event_id);
        CHECK_EQ(ps.size(), (size_t)2);
        CHECK_EQ(ps[0].perspective, "active");
        CHECK_EQ(ps[1].perspective, "observer");
    }

    // ============ 3. Link：先后关系 + DAG 遍历 ============
    store.append_link(Link{p1a.profile_id, p2a.profile_id, "before"});
    store.append_link(Link{p1b.profile_id, p2b.profile_id, "before"});
    {
        auto out = store.links_from(p1a.profile_id);
        CHECK_EQ(out.size(), (size_t)1);
        CHECK_EQ(out[0].to_profile_id, p2a.profile_id);
        CHECK_EQ(out[0].relation, "before");
        auto in = store.links_to(p2a.profile_id);
        CHECK_EQ(in.size(), (size_t)1);
        CHECK_EQ(in[0].from_profile_id, p1a.profile_id);
    }
    CHECK_THROWS_AS(store.append_link(Link{p1a.profile_id, "pf-none", "before"}),
                    NotFoundError);

    // ============ 4. timeline：虚拟时间下按 Link 排序 ============
    {
        auto tl = store.timeline(na.narrative_id);
        CHECK_EQ(tl.size(), (size_t)4);
        std::unordered_map<std::string, int> pos;
        for (int i = 0; i < (int)tl.size(); ++i) pos[tl[(size_t)i].profile_id] = i;
        // p1* 虽后追加，但必须在 p2* 之前
        CHECK(pos[p1a.profile_id] < pos[p2a.profile_id]);
        CHECK(pos[p1b.profile_id] < pos[p2b.profile_id]);
    }

    // ============ 5. query_profiles 索引查询 ============
    {
        auto by_subject = store.query_profiles("恶龙", std::nullopt, std::nullopt);
        CHECK_EQ(by_subject.size(), (size_t)1);
        CHECK_EQ(by_subject[0].profile_id, p2a.profile_id);
        auto by_verb = store.query_profiles(std::nullopt, "拔出", std::nullopt);
        CHECK_EQ(by_verb.size(), (size_t)1);
        CHECK_EQ(by_verb[0].profile_id, p1a.profile_id);
        auto by_persp = store.query_profiles(std::nullopt, std::nullopt, "passive");
        CHECK_EQ(by_persp.size(), (size_t)1);
        auto combo = store.query_profiles("村民", "躲避", "passive");
        CHECK_EQ(combo.size(), (size_t)1);
        CHECK(combo[0].profile_id == p2b.profile_id);
        auto none = store.query_profiles("不存在的人", std::nullopt, std::nullopt);
        CHECK(none.empty());
    }

    // ============ 6. 实体与人名：重名 / 别名 / 绑定 / 重消歧 ============
    // 重名不同人：两个都叫 "张伟" 的实体
    auto zw_a = Entity::create("张伟", "person", json::array(), {{"village", "东村"}});
    auto zw_b = Entity::create("张伟", "person", json::array(), {{"village", "西村"}});
    // 同人异名：勇者，别名 "小勇"
    auto hero = Entity::create("勇者", "person", json::array({"小勇"}));
    store.append_entity(zw_a);
    store.append_entity(zw_b);
    store.append_entity(hero);
    {
        auto found = store.find_entities_by_name("张伟");
        CHECK_EQ(found.size(), (size_t)2);  // 重名全部返回
        CHECK(found[0].entity_id != found[1].entity_id);
        auto by_alias = store.find_entities_by_name("小勇");
        CHECK_EQ(by_alias.size(), (size_t)1);
        CHECK_EQ(by_alias[0].entity_id, hero.entity_id);
        auto got = store.get_entity(zw_a.entity_id);
        CHECK_EQ(got.attributes.at("village"), "东村");
    }
    CHECK_THROWS_AS(store.append_entity(zw_a), DuplicateError);
    CHECK_THROWS_AS(store.get_entity("en-none"), NotFoundError);

    // 两个侧写的 subject 都是 surface "张伟"，绑定到不同实体，互不染指
    auto pz1 = Profile::create(ev1.event_id, "observer", TimeRef::virtual_time("从前", 1),
                               "村口", "张伟", "目睹", "拔剑");
    auto pz2 = Profile::create(ev2.event_id, "passive", TimeRef::virtual_time("第二天", 2),
                               "村庄", "张伟", "逃离", "火海");
    store.append_profile(pz1);
    store.append_profile(pz2);
    store.append_binding(Binding::create(pz1.profile_id, "subject", zw_a.entity_id));
    store.append_binding(Binding::create(pz2.profile_id, "subject", zw_b.entity_id, 0.6,
                                         "感知层初步消歧"));
    {
        auto b1 = store.effective_binding(pz1.profile_id, "subject");
        auto b2 = store.effective_binding(pz2.profile_id, "subject");
        CHECK(b1 && b1->entity_id == zw_a.entity_id);
        CHECK(b2 && b2->entity_id == zw_b.entity_id);
        CHECK(b2->confidence < 1.0);
        // surface 查询仍按原文命中两条
        auto by_surface = store.query_profiles("张伟", std::nullopt, std::nullopt);
        CHECK_EQ(by_surface.size(), (size_t)2);
        // 未消歧的槽位
        auto unbound = store.effective_binding(pz1.profile_id, "object");
        CHECK(!unbound);
    }
    // 绑定记录的外键检查
    CHECK_THROWS_AS(store.append_binding(Binding::create("pf-none", "subject", zw_a.entity_id)),
                    NotFoundError);
    CHECK_THROWS_AS(store.append_binding(Binding::create(pz1.profile_id, "subject", "en-none")),
                    NotFoundError);

    // 重消歧：latest wins，全历史保留
    {
        store.append_binding(Binding::create(pz2.profile_id, "subject", zw_a.entity_id, 0.9,
                                             "人工复核纠正"));
        auto eff = store.effective_binding(pz2.profile_id, "subject");
        CHECK(eff && eff->entity_id == zw_a.entity_id);
        auto history = store.bindings_for(pz2.profile_id, "subject");
        CHECK_EQ(history.size(), (size_t)2);  // 历史全保留，可审计
        CHECK(history[0].entity_id == zw_b.entity_id);
        CHECK(history[1].entity_id == zw_a.entity_id);
        CHECK_EQ(store.bindings_of(pz2.profile_id).size(), (size_t)2);
    }

    // 实体→侧写反查（有效绑定语义）
    {
        auto ps_a = store.profiles_of_entity(zw_a.entity_id);
        CHECK_EQ(ps_a.size(), (size_t)2);  // pz1 + 重消歧后的 pz2
        auto ps_b = store.profiles_of_entity(zw_b.entity_id);
        CHECK(ps_b.empty());               // pz2 已纠正走，不再是有效绑定
        // 槽位过滤
        auto ps_a_subj = store.profiles_of_entity(zw_a.entity_id, "subject");
        CHECK_EQ(ps_a_subj.size(), (size_t)2);
        auto ps_a_obj = store.profiles_of_entity(zw_a.entity_id, "object");
        CHECK(ps_a_obj.empty());
        // 从未被绑定的实体
        CHECK(store.profiles_of_entity(hero.entity_id).empty());
    }

    // 扩展索引：place / object 查询
    {
        auto by_place = store.query_profiles(std::nullopt, std::nullopt, std::nullopt,
                                             "村庄", std::nullopt);
        CHECK_EQ(by_place.size(), (size_t)2);  // p2b + pz2
        auto by_object = store.query_profiles(std::nullopt, std::nullopt, std::nullopt,
                                              std::nullopt, "圣剑");
        CHECK_EQ(by_object.size(), (size_t)1);
        CHECK_EQ(by_object[0].profile_id, p1a.profile_id);
        // 五条件组合
        auto combo5 = store.query_profiles("张伟", "逃离", "passive", "村庄", "火海");
        CHECK_EQ(combo5.size(), (size_t)1);
        CHECK_EQ(combo5[0].profile_id, pz2.profile_id);
    }

    // ============ 7. 纠正模式：refines（append-only，原侧写不变） ============
    {
        auto p1a_fix = Profile::create(ev1.event_id, "active", TimeRef::virtual_time("从前", 1),
                                       "村口老树下", "勇者", "拔出", "圣剑");
        store.append_profile(p1a_fix);
        store.append_link(Link{p1a_fix.profile_id, p1a.profile_id, "refines"});
        auto orig = store.get_profile(p1a.profile_id);
        CHECK_EQ(orig.place, "村口");  // 原侧写不变
        auto refs = store.links_from(p1a_fix.profile_id);
        CHECK_EQ(refs.size(), (size_t)1);
        CHECK_EQ(refs[0].relation, "refines");
        // refines 边不影响 timeline 排序，只多一个节点
        auto tl = store.timeline(na.narrative_id);
        CHECK_EQ(tl.size(), (size_t)7);
    }

    // ============ 7b. 修饰语：形容词/量词/程度词的保留 ============
    {
        auto pm = Profile::create(ev1.event_id, "observer", TimeRef::virtual_time("从前", 1),
                                  "村口", "村民", "看到", "拔剑一幕", json::object(),
                                  {Modifier{"verb", "manner", "奋力"},
                                   Modifier{"object", "quality", "锋利无比"},
                                   Modifier{"object", "quantity", "一把"}});
        store.append_profile(pm);
        auto got = store.get_profile(pm.profile_id);
        CHECK_EQ(got.modifiers.size(), (size_t)3);
        CHECK_EQ(got.modifiers[0].text, "奋力");  // 顺序保留
        CHECK_EQ(got.modifiers[0].kind, "manner");
        CHECK_EQ(got.modifiers[0].target, "verb");
        CHECK_EQ(got.modifiers[1].text, "锋利无比");
        CHECK_EQ(got.modifiers[2].kind, "quantity");
        // 图里包含修饰语节点与修饰边
        CHECK_EQ(got.graph.at("nodes").size(), (size_t)7);  // 4 骨架 + 3 修饰
        CHECK_EQ(got.graph.at("edges").size(), (size_t)6);
        // 按修饰语查询：text / kind / target / 组合
        auto by_text = store.find_profiles_by_modifier("奋力", std::nullopt, std::nullopt);
        CHECK_EQ(by_text.size(), (size_t)1);
        CHECK_EQ(by_text[0].profile_id, pm.profile_id);
        auto by_kind = store.find_profiles_by_modifier(std::nullopt, "quantity", std::nullopt);
        CHECK_EQ(by_kind.size(), (size_t)1);
        auto by_target = store.find_profiles_by_modifier(std::nullopt, std::nullopt, "object");
        CHECK_EQ(by_target.size(), (size_t)1);  // object 上两个修饰语仍只命中一个侧写
        auto combo = store.find_profiles_by_modifier("锋利无比", "quality", "object");
        CHECK_EQ(combo.size(), (size_t)1);
        // 条件必须落在同一修饰语上："奋力"是 manner 不是 quantity
        auto cross = store.find_profiles_by_modifier("奋力", "quantity", std::nullopt);
        CHECK(cross.empty());
        auto none = store.find_profiles_by_modifier("不存在的词", std::nullopt, std::nullopt);
        CHECK(none.empty());
        // 无修饰语的侧写不被误命中
        auto any_mod = store.find_profiles_by_modifier(std::nullopt, "manner", std::nullopt);
        CHECK_EQ(any_mod.size(), (size_t)1);
    }

    // ============ 8. 环检测（放最后，会污染本 store 的叙事） ============
    {
        auto ev3 = Event::create(na.narrative_id, "环测试");
        store.append_event(ev3);
        auto c1 = Profile::create(ev3.event_id, "active", TimeRef::virtual_time("t1"),
                                  "x", "甲", "做", "事");
        auto c2 = Profile::create(ev3.event_id, "active", TimeRef::virtual_time("t2"),
                                  "x", "乙", "做", "事");
        store.append_profile(c1);
        store.append_profile(c2);
        store.append_link(Link{c1.profile_id, c2.profile_id, "before"});
        store.append_link(Link{c2.profile_id, c1.profile_id, "before"});
        CHECK_THROWS_AS(store.timeline(na.narrative_id), EventStoreError);
    }
}
