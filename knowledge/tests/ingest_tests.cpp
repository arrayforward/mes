// 摄入管道单测：用 SqlStore(":memory:") 构造侧写，验证八步管道（含 ame 词汇层归一）。

#include <cmath>
#include <fstream>

#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"
#include "eventstore/backends/sql_store.h"
#include "knowledge/ame_lexicon.h"
#include "knowledge/ingest.h"
#include "test_framework.h"

using namespace knowledge;
using namespace eventstore;

static const std::string kAssets = KNOWLEDGE_ASSETS_DIR;
static std::string cp(const std::string& n) { return make_concept_id(n); }
static std::string in(const std::string& n) { return make_instance_id(n); }

// 构造与 demo_story 同构的内存库 + 一条守卫通信事件（测绑定置信度）
struct Fixture {
    SqlStore store{":memory:"};
    Narrative story = Narrative::create("story", "勇者斗恶龙");
    Event ev1 = Event::create(story.narrative_id, "勇者拔剑");
    Event ev2 = Event::create(story.narrative_id, "恶龙袭击村庄");
    Event ev3 = Event::create(story.narrative_id, "守卫呼喊");
    Profile p1 = Profile::create(ev1.event_id, "active", TimeRef::virtual_time("从前"),
                                 "村口老树下", "勇者", "拔出", "圣剑");
    Profile p2 = Profile::create(ev2.event_id, "active", TimeRef::virtual_time("第二天"),
                                 "村庄上空", "恶龙", "袭击", "村庄");
    Profile p3 = Profile::create(ev3.event_id, "active", TimeRef::virtual_time("第二天"),
                                 "城门", "守卫", "呼喊", "");
    Entity guard = Entity::create("守卫", "person");

    Fixture() {
        store.append_narrative(story);
        store.append_event(ev1);
        store.append_event(ev2);
        store.append_event(ev3);
        store.append_profile(p1);
        store.append_profile(p2);
        store.append_profile(p3);
        store.append_link(Link{p1.profile_id, p2.profile_id, "before"});
        store.append_link(Link{p1.profile_id, p2.profile_id, "causes"});
        store.append_entity(guard);
        // 概率性消解：守卫 → 实体，置信度 0.5
        store.append_binding(Binding::create(p3.profile_id, "subject", guard.entity_id, 0.5));
    }
};

static Ontology make_ontology() {
    Ontology ont;
    ont.load(kAssets + "/ontology_seed.json");
    return ont;
}

TEST(classify_along_is_a_chain) {
    Fixture f;
    Ontology ont = make_ontology();
    Lexicon lex;
    lex.load(kAssets + "/lexicon.json");
    Ingester ing(ont, lex);

    IngestResult r = ing.ingest_profile(f.store, f.p1.profile_id);
    // 步骤4：勇者拔剑 被分类为 持械/战斗动作
    CHECK_EQ(r.chain.size(), (size_t)4);
    CHECK_EQ(r.chain[0], std::string("拔出"));
    CHECK_EQ(r.chain[1], std::string("持械"));
    CHECK_EQ(r.chain[2], std::string("战斗动作"));
    CHECK_EQ(r.event_class, std::string("战斗动作"));
    CHECK_EQ(r.verb_match, 1.0);
}

TEST(near_synonym_verb_degraded) {
    Fixture f;
    Ontology ont = make_ontology();
    Lexicon lex;
    lex.load(kAssets + "/lexicon.json");
    apply_lexicon(ont, lex);
    Ingester ing(ont, lex);

    IngestResult r = ing.ingest_profile(f.store, f.p2.profile_id);
    // "袭击"非概念，近义降级到"攻击"（score 0.85）
    CHECK_EQ(r.verb_concept_id, cp("攻击"));
    CHECK(std::fabs(r.verb_match - 0.85) < 1e-9);
    CHECK_EQ(r.event_class, std::string("战斗动作"));
}

TEST(rule_forward_chaining_with_path) {
    Fixture f;
    Ontology ont = make_ontology();
    Lexicon lex;
    lex.load(kAssets + "/lexicon.json");
    Ingester ing(ont, lex);

    IngestResult r = ing.ingest_profile(f.store, f.p1.profile_id);
    // 步骤8：规则命中——"所有战斗动作可能触发警觉"
    CHECK(!r.inferences.empty());
    const Inference* hit = nullptr;
    for (const auto& i : r.inferences)
        if (i.candidate == "警觉") hit = &i;
    CHECK(hit != nullptr);
    if (hit) {
        CHECK_EQ(hit->rule_id, std::string("rl-combat-aggro"));
        CHECK(std::fabs(hit->confidence - 0.8) < 1e-9);  // 0.8 × 1.0
        CHECK(!hit->conflict);
        // 可解释路径：事件 → 拔出 IS_A 持械 IS_A 战斗动作 TRIGGERS 警觉
        CHECK_EQ(hit->path.front(), std::string("勇者拔出圣剑"));
        CHECK_EQ(hit->path.back(), std::string("警觉"));
        CHECK(std::find(hit->path.begin(), hit->path.end(), "IS_A") != hit->path.end());
        CHECK(std::find(hit->path.begin(), hit->path.end(), "TRIGGERS") != hit->path.end());
    }
    // 袭击（近义 0.85）：规则置信度 × 匹配权重
    IngestResult r2 = ing.ingest_profile(f.store, f.p2.profile_id);
    bool found = false;
    for (const auto& i : r2.inferences)
        if (i.candidate == "警觉") {
            found = true;
            CHECK(std::fabs(i.confidence - 0.68) < 1e-9);  // 0.8 × 0.85
        }
    CHECK(found);
}

TEST(observed_in_anchors_back_reference) {
    Fixture f;
    Ontology ont = make_ontology();
    Lexicon lex;
    lex.load(kAssets + "/lexicon.json");
    Ingester ing(ont, lex);
    ing.ingest_profile(f.store, f.p1.profile_id);

    // 步骤5：动词概念/事件实例/主客体实例都有 OBSERVED_IN 锚点回指原 profile_id
    bool verb_anchor = false, ev_anchor = false, subj_anchor = false;
    for (const auto& e : ont.edges_to(f.p1.profile_id)) {
        CHECK(e.type == RelType::OBSERVED_IN);
        CHECK(e.attrs.at("event_id") == f.ev1.event_id);
        if (e.from == cp("拔出")) verb_anchor = true;
        if (e.from == in("勇者拔出圣剑")) ev_anchor = true;
        if (e.from == in("勇者")) subj_anchor = true;
    }
    CHECK(verb_anchor);
    CHECK(ev_anchor);
    CHECK(subj_anchor);
    // 回指可解析：锚点 id 就是 storage 的 profile_id
    const Profile& back = f.store.get_profile(f.p1.profile_id);
    CHECK_EQ(back.verb, std::string("拔出"));
}

TEST(instance_of_and_cooccur_growth) {
    Fixture f;
    Ontology ont = make_ontology();
    Lexicon lex;
    lex.load(kAssets + "/lexicon.json");
    Ingester ing(ont, lex);
    ing.ingest_profile(f.store, f.p1.profile_id);

    // 步骤3：圣剑 INSTANCE_OF 圣剑概念（按名命中）；事件 INSTANCE_OF 动词概念
    CHECK(ont.has_edge(in("圣剑"), cp("圣剑"), RelType::INSTANCE_OF));
    CHECK(ont.has_edge(in("勇者拔出圣剑"), cp("拔出"), RelType::INSTANCE_OF));
    // 步骤6：五要素节点两两共现（无向，规范方向存储，任一端可查）
    auto has_cooccur = [&](const std::string& a, const std::string& b) {
        return ont.has_edge(a, b, RelType::COOCCUR) || ont.has_edge(b, a, RelType::COOCCUR);
    };
    CHECK(has_cooccur(in("勇者"), cp("拔出")));
    CHECK(has_cooccur(cp("拔出"), in("圣剑")));
    // 幂等：重复摄入不重复生长
    IngestResult again = ing.ingest_profile(f.store, f.p1.profile_id);
    for (const auto& e : ont.edges_from(cp("拔出")))
        if (e.type == RelType::COOCCUR && e.to == in("勇者")) CHECK_EQ(e.weight, 1.0);
    CHECK_EQ(again.event_class, std::string("战斗动作"));
}

TEST(link_sync_causes_and_before) {
    Fixture f;
    Ontology ont = make_ontology();
    Lexicon lex;
    lex.load(kAssets + "/lexicon.json");
    Ingester ing(ont, lex);
    ing.ingest_all(f.store);

    // 步骤7：Link.before/causes 同步为事件实例级 BEFORE/CAUSES 边
    CHECK(ont.has_edge(in("勇者拔出圣剑"), in("恶龙袭击村庄"), RelType::BEFORE));
    CHECK(ont.has_edge(in("勇者拔出圣剑"), in("恶龙袭击村庄"), RelType::CAUSES));
}

TEST(instance_level_rule_with_binding_confidence) {
    Fixture f;
    Ontology ont = make_ontology();
    Lexicon lex;
    lex.load(kAssets + "/lexicon.json");
    // 测试专用规则：人物 TRIGGERS 警觉（实例级继承 + 绑定置信度）
    ont.add_rule({"rl-person-alert", cp("人物"), RelType::TRIGGERS, cp("警觉"), 0.6, ""});
    Ingester ing(ont, lex);
    IngestResult r = ing.ingest_profile(f.store, f.p3.profile_id);

    // 守卫 按名命中概念 守卫 IS_A 人物（绑定 conf 0.5 作 INSTANCE_OF 边权）
    CHECK(ont.has_edge(in("守卫"), cp("守卫"), RelType::INSTANCE_OF));
    // 规则沿 守卫 → 人物 继承：0.6 × 0.5（绑定置信度）= 0.3
    bool found = false;
    for (const auto& i : r.inferences)
        if (i.rule_id == "rl-person-alert") {
            found = true;
            CHECK_EQ(i.subject, std::string("守卫"));
            CHECK(std::fabs(i.confidence - 0.3) < 1e-9);
        }
    CHECK(found);
    // 呼喊 IS_A 通信动作：事件分类到通信域
    CHECK_EQ(r.event_class, std::string("通信动作"));
}

TEST(conflict_marking) {
    std::vector<Inference> infs;
    infs.push_back({"rl-a", "x", "警觉", RelType::TRIGGERS, 0.8, {}, false});
    infs.push_back({"rl-b", "x", "警觉", RelType::PREVENTS, 0.5, {}, false});
    infs.push_back({"rl-c", "y", "警觉", RelType::TRIGGERS, 0.9, {}, false});
    mark_conflicts(infs);
    CHECK(infs[0].conflict);
    CHECK(infs[1].conflict);
    CHECK(!infs[2].conflict);  // 不同主体不标
}

TEST(ame_layer_drives_normalization) {
    Fixture f;
    Ontology ont = make_ontology();
    Lexicon lex;
    lex.load(kAssets + "/lexicon.json");
    // ame 词汇层词典："抽拔" 只在 ame 词典里（种子词典没有）
    const std::string path = "test_ame_ingest_fixture.json";
    {
        std::ofstream f(path);
        f << R"({"entries":[{"word":"拔出","syn_strong":["抽拔"]}]})";
    }
    ame::Storage st;
    ame::KeywordManager kw(st);
    load_ame_lexicon(kw, path);

    Ingester ing(ont, lex);
    ing.set_ame_layer(&kw);
    // 动词"抽拔"经 ame 别名归一为"拔出"⇒ 命中本体概念，分类到战斗动作
    Profile p4 = Profile::create(f.ev1.event_id, "active", TimeRef::virtual_time("从前"),
                                 "村口老树下", "勇者", "抽拔", "圣剑");
    f.store.append_profile(p4);
    IngestResult r = ing.ingest_profile(f.store, p4.profile_id);
    CHECK_EQ(r.verb_concept_id, cp("拔出"));
    CHECK_EQ(r.verb_match, 1.0);
    CHECK_EQ(r.event_class, std::string("战斗动作"));
    // ame 侧生长：五要素落 Keyword 节点，近义词补 RELATED 边并唤醒
    CHECK(kw.find("拔出") != nullptr);
    std::remove(path.c_str());
}

int main() { return tfw::run_all(); }
