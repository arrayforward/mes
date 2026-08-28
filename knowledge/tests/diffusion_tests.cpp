// 扩散引擎单测：加权 BFS、能量衰减、出度归一、双向边、top3 路径、budget。

#include "knowledge/diffusion.h"
#include "test_framework.h"

using namespace knowledge;

static std::string cp(const std::string& n) { return make_concept_id(n); }

// 图：甲 IS_A 乙，乙 TRIGGERS 丙，甲 COOCCUR 丁（weight 2）
static Ontology make_graph() {
    Ontology ont;
    for (const char* n : {"甲", "乙", "丙", "丁"}) ont.add_concept(n);
    ont.add_edge({cp("甲"), cp("乙"), RelType::IS_A, 1.0, {}});
    ont.add_edge({cp("乙"), cp("丙"), RelType::TRIGGERS, 1.0, {}});
    ont.add_edge({cp("甲"), cp("丁"), RelType::COOCCUR, 2.0, {}});
    return ont;
}

TEST(diffuse_basic_energy_and_decay) {
    Ontology ont = make_graph();
    auto res = diffuse(ont, {{cp("甲"), 1.0}}, 3);
    CHECK(res.count(cp("甲")) > 0);
    CHECK(res.count(cp("乙")) > 0);
    CHECK(res.count(cp("丙")) > 0);
    CHECK(res.count(cp("丁")) > 0);
    CHECK(res[cp("甲")].energy >= 1.0);  // 种子能量（回流能量可叠加）
    // 能量随 hop 衰减：丙（2 跳） < 乙（1 跳）
    CHECK(res[cp("丙")].energy < res[cp("乙")].energy);
    CHECK(res[cp("丙")].energy > 0.0);
}

TEST(diffuse_undirected_traversal) {
    Ontology ont = make_graph();
    // 从丁出发：COOCCUR 无向 ⇒ 逆向到达甲，再到乙
    auto res = diffuse(ont, {{cp("丁"), 1.0}}, 3);
    CHECK(res.count(cp("甲")) > 0);
    CHECK(res.count(cp("乙")) > 0);
    // 有向边同样允许逆向联想（图按无向加权处理）：从丙逆向到乙
    auto res2 = diffuse(ont, {{cp("丙"), 1.0}}, 2);
    CHECK(res2.count(cp("乙")) > 0);
}

TEST(diffuse_top3_paths_explainable) {
    Ontology ont = make_graph();
    auto res = diffuse(ont, {{cp("甲"), 1.0}}, 3);
    const auto& ne = res[cp("丙")];
    CHECK(ne.paths.size() <= 3);  // top3 截断
    CHECK(!ne.paths.empty());
    const Path& p = ne.paths[0];
    CHECK_EQ(p.steps.size(), (size_t)2);  // 甲→乙→丙
    CHECK_EQ(p.steps[0].from, cp("甲"));
    CHECK_EQ(p.steps[0].to, cp("乙"));
    CHECK(p.steps[0].type == RelType::IS_A);
    CHECK(p.steps[1].type == RelType::TRIGGERS);
}

TEST(diffuse_budget_and_seed_filter) {
    Ontology ont = make_graph();
    // budget=1：只扩展甲本身 ⇒ 只有直接邻居被激活
    auto res = diffuse(ont, {{cp("甲"), 1.0}}, 3, /*budget=*/1);
    CHECK(res.count(cp("乙")) > 0);
    CHECK(res.count(cp("丙")) == 0);
    // 不存在的种子被跳过
    auto res2 = diffuse(ont, {{"cp-不存在", 1.0}}, 3);
    CHECK(res2.empty());
}

int main() { return tfw::run_all(); }
