// 推理器单测：种子（别名归一 + 近义降级）、规则前向链（限深）、扩散融合。

#include <cmath>

#include "knowledge/reasoner.h"
#include "test_framework.h"

using namespace knowledge;

static const std::string kAssets = KNOWLEDGE_ASSETS_DIR;
static std::string cp(const std::string& n) { return make_concept_id(n); }

struct Ctx {
    Ontology ont;
    Lexicon lex;
    Ctx() {
        ont.load(kAssets + "/ontology_seed.json");
        lex.load(kAssets + "/lexicon.json");
        apply_lexicon(ont, lex);
    }
};

TEST(seed_alias_normalization) {
    Ctx c;
    Reasoner r(c.ont, c.lex);
    // "抽出" 别名归一到 "拔出" ⇒ 硬种子命中
    ReasoningChain chain = r.infer("抽出");
    CHECK(!chain.seeds.empty());
    CHECK_EQ(chain.seeds[0].first, cp("拔出"));
    CHECK_EQ(chain.seeds[0].second, 1.0);
    // 规则命中：战斗动作 TRIGGERS 警觉（直接种子置信度 0.8；近义种子另有低分命中）
    bool hit = false;
    for (const auto& i : chain.inferences)
        if (i.candidate == "警觉" && std::fabs(i.confidence - 0.8) < 1e-9) {
            hit = true;
            CHECK(!i.path.empty());
        }
    CHECK(hit);
    CHECK(chain.confidence > 0.0);
    // 扩散激活了图谱节点
    CHECK(!chain.activated.empty());
}

TEST(seed_near_synonym_degraded) {
    Ctx c;
    Reasoner r(c.ont, c.lex);
    // "袭击" 无概念节点，近义降级到 "攻击"（0.4 × 0.85 = 0.34）
    ReasoningChain chain = r.infer("袭击");
    bool seed_found = false;
    for (const auto& [id, w] : chain.seeds)
        if (id == cp("攻击")) {
            seed_found = true;
            CHECK(std::fabs(w - 0.34) < 1e-9);
        }
    CHECK(seed_found);
    bool hit = false;
    for (const auto& i : chain.inferences)
        if (i.candidate == "警觉") hit = true;
    CHECK(hit);
}

TEST(rule_forward_chain_multi_depth) {
    Ctx c;
    // 加一条二级规则：警觉 TRIGGERS 逃跑 ⇒ 前向链 depth=2 命中
    c.ont.add_concept("逃跑");
    c.ont.add_rule({"rl-alert-flee", cp("警觉"), RelType::TRIGGERS, cp("逃跑"), 0.5, ""});
    Reasoner r(c.ont, c.lex);
    ReasoningChain chain = r.infer("攻击");
    bool l1 = false, l2 = false;
    for (const auto& i : chain.inferences) {
        if (i.candidate == "警觉") {
            l1 = true;
            CHECK(std::fabs(i.confidence - 0.8) < 1e-9);
        }
        if (i.candidate == "逃跑") {
            l2 = true;
            CHECK(std::fabs(i.confidence - 0.4) < 1e-9);  // 0.8 × 0.5 沿链衰减
        }
    }
    CHECK(l1);
    CHECK(l2);
}

TEST(no_seed_returns_empty) {
    Ctx c;
    Reasoner r(c.ont, c.lex);
    ReasoningChain chain = r.infer("完全不存在的词");
    CHECK(chain.seeds.empty());
    CHECK(chain.inferences.empty());
    CHECK_EQ(chain.confidence, 0.0);
}

int main() { return tfw::run_all(); }
