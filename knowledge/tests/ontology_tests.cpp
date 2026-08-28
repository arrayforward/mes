// 本体单测：is-a/part-of 传递闭包、规则沿 is-a 继承、版本化资产加载、快照往返。

#include "knowledge/ontology.h"
#include "test_framework.h"

using namespace knowledge;

static const std::string kAssets = KNOWLEDGE_ASSETS_DIR;
static std::string cp(const std::string& n) { return make_concept_id(n); }

TEST(load_seed_ontology) {
    Ontology ont;
    ont.load(kAssets + "/ontology_seed.json");
    CHECK_EQ(ont.version(), std::string("1.0.0"));
    CHECK(ont.find_concept("战斗动作") != nullptr);
    CHECK_EQ(ont.find_concept("战斗动作")->concept_id, cp("战斗动作"));
    CHECK(ont.find_concept("不存在") == nullptr);
    // 幂等：同名重复添加复用同一节点
    ConceptNode& again = ont.add_concept("战斗动作");
    CHECK_EQ(again.concept_id, cp("战斗动作"));
}

TEST(is_a_closure) {
    Ontology ont;
    ont.load(kAssets + "/ontology_seed.json");
    CHECK(ont.is_a(cp("拔出"), cp("持械")));
    CHECK(ont.is_a(cp("拔出"), cp("战斗动作")));  // 传递
    CHECK(ont.is_a(cp("拔出"), cp("动作")));      // 传递到根
    CHECK(ont.is_a(cp("圣剑"), cp("物品")));
    CHECK(!ont.is_a(cp("拔出"), cp("移动动作")));
    CHECK(!ont.is_a(cp("动作"), cp("拔出")));     // 不可逆
    CHECK(ont.is_a(cp("拔出"), cp("拔出")));      // 自反

    auto anc = ont.ancestors(cp("拔出"));
    CHECK_EQ(anc.size(), (size_t)3);
    CHECK_EQ(anc[0], cp("持械"));  // 近祖先在前
    CHECK_EQ(anc[2], cp("动作"));

    auto desc = ont.descendants(cp("战斗动作"));
    CHECK(desc.size() == 3);  // 持械、拔出、攻击
    auto path = ont.is_a_path(cp("拔出"), cp("战斗动作"));
    CHECK_EQ(path.size(), (size_t)3);
    CHECK_EQ(path[0], cp("拔出"));
    CHECK_EQ(path[2], cp("战斗动作"));
    CHECK(ont.is_a_path(cp("拔出"), cp("状态")).empty());
}

TEST(part_of_closure) {
    Ontology ont;
    ont.load(kAssets + "/ontology_seed.json");
    auto wholes = ont.part_of_ancestors(cp("剑刃"));
    CHECK_EQ(wholes.size(), (size_t)1);
    CHECK_EQ(wholes[0], cp("圣剑"));
    // part-of 与 is-a 是两条独立通道
    CHECK(!ont.is_a(cp("剑刃"), cp("圣剑")));
}

TEST(rule_inheritance) {
    Ontology ont;
    ont.load(kAssets + "/ontology_seed.json");
    // 拔出 沿 is-a 继承 战斗动作 的规则
    auto rules = ont.rules_for(cp("拔出"));
    CHECK_EQ(rules.size(), (size_t)1);
    CHECK_EQ(rules[0].rule_id, std::string("rl-combat-aggro"));
    CHECK_EQ(rules[0].consequent, cp("警觉"));
    CHECK_EQ(rules[0].confidence, 0.8);
    // 攻击 继承 战斗动作 规则 + 自身规则
    CHECK_EQ(ont.rules_for(cp("攻击")).size(), (size_t)2);
    // 移动动作 不继承战斗规则
    CHECK(ont.rules_for(cp("躲避")).empty());
    // 同 id 规则去重
    ont.add_rule(ont.rules()[0]);
    CHECK_EQ(ont.rules().size(), (size_t)2);
}

TEST(seed_relations) {
    Ontology ont;
    ont.load(kAssets + "/ontology_seed.json");
    CHECK(ont.has_edge(cp("攻击"), cp("武器"), RelType::REQUIRES));
}

TEST(upsert_and_dedup_edges) {
    Ontology ont;
    ont.add_concept("甲");
    ont.add_concept("乙");
    ont.upsert_edge(cp("甲"), cp("乙"), RelType::COOCCUR, 1.0);
    ont.upsert_edge(cp("甲"), cp("乙"), RelType::COOCCUR, 1.0);
    CHECK_EQ(ont.edge_count(), (size_t)1);
    auto edges = ont.edges_from(cp("甲"));
    CHECK_EQ(edges.size(), (size_t)1);
    CHECK_EQ(edges[0].weight, 2.0);
    // add_edge 去重合并：weight 取大
    ont.add_edge({cp("甲"), cp("乙"), RelType::COOCCUR, 5.0, {}});
    CHECK_EQ(ont.edge_count(), (size_t)1);
    CHECK_EQ(ont.edges_from(cp("甲"))[0].weight, 5.0);
}

TEST(snapshot_roundtrip) {
    Ontology ont;
    ont.load(kAssets + "/ontology_seed.json");
    ont.add_instance("勇者");
    ont.upsert_edge(make_instance_id("勇者"), cp("人物"), RelType::COOCCUR, 2.0);
    json snap = ont.snapshot();

    Ontology restored;
    restored.load_snapshot(snap);
    CHECK_EQ(restored.version(), ont.version());
    CHECK(restored.is_a(cp("拔出"), cp("战斗动作")));
    CHECK(restored.find_instance("勇者") != nullptr);
    CHECK_EQ(restored.edge_count(), ont.edge_count());
    CHECK_EQ(restored.rules().size(), ont.rules().size());
    CHECK_EQ(restored.edges_from(make_instance_id("勇者"))[0].weight, 2.0);
}

TEST(ontology_errors) {
    Ontology ont;
    CHECK_THROWS_AS(ont.load(kAssets + "/no_such_file.json"), KnowledgeError);
    CHECK_THROWS_AS(
        ont.load_json(json::parse(R"({"concepts":[{"name":"甲","is_a":"不存在"}]})")),
        KnowledgeError);
}

int main() { return tfw::run_all(); }
