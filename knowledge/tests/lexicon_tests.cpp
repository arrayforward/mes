// 词典单测：别名归一、近义表过滤、互为同义组防互指、三分流灌库建边、ame 词汇层桥接。

#include <fstream>

#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"
#include "knowledge/ame_lexicon.h"
#include "knowledge/lexicon.h"
#include "knowledge/ontology.h"
#include "test_framework.h"

using namespace knowledge;

static const std::string kAssets = KNOWLEDGE_ASSETS_DIR;

TEST(load_seed_lexicon) {
    Lexicon lex;
    lex.load(kAssets + "/lexicon.json");
    CHECK_EQ(lex.version(), std::string("1.0.0"));
    // 强同义 → 别名归一
    CHECK_EQ(lex.normalize("抽出"), std::string("拔出"));
    CHECK_EQ(lex.normalize("警惕"), std::string("警觉"));
    CHECK_EQ(lex.normalize("目睹"), std::string("看到"));
    // 未命中返回原词
    CHECK_EQ(lex.normalize("勇者"), std::string("勇者"));
    // 近义词表（score>0.6）
    const auto& near = lex.near_of("攻击");
    CHECK_EQ(near.size(), (size_t)2);
    CHECK_EQ(near[0].first, std::string("袭击"));
    CHECK(near[0].second > 0.8);
    // 近义词表查询也走别名归一；非主词经反向映射降级扩展到主词
    CHECK_EQ(lex.near_of("袭击").size(), (size_t)1);
    CHECK_EQ(lex.near_of("袭击")[0].first, std::string("攻击"));
}

TEST(near_filter_and_cap) {
    // score<=0.6 过滤、每词 ≤8 条
    json j = {{"version", "t"}, {"entries", json::array()}};
    json e;
    e["word"] = "w";
    e["syn_near"] = json::array();
    for (int i = 0; i < 10; ++i)
        e["syn_near"].push_back({{"word", "n" + std::to_string(i)}, {"score", 0.9}});
    e["syn_near"].push_back({{"word", "low"}, {"score", 0.5}});
    j["entries"].push_back(e);
    Lexicon lex;
    lex.load_json(j);
    CHECK_EQ(lex.near_of("w").size(), (size_t)8);
}

TEST(alias_mutual_guard) {
    // 主词别名需互为同义组成员：乙是主词但不互指甲 ⇒ 乙不归并到甲；
    // 丙非主词 ⇒ 直接归并到乙。
    json j = {{"entries",
               {{{"word", "甲"}, {"syn_strong", {"乙"}}},
                {{"word", "乙"}, {"syn_strong", {"丙"}}}}}};
    Lexicon lex;
    lex.load_json(j);
    CHECK_EQ(lex.normalize("乙"), std::string("乙"));   // 单向互指跳过
    CHECK_EQ(lex.normalize("丙"), std::string("乙"));   // 非主词归并
}

TEST(apply_lexicon_three_way) {
    Ontology ont;
    ont.load(kAssets + "/ontology_seed.json");
    Lexicon lex;
    lex.load(kAssets + "/lexicon.json");
    int n = apply_lexicon(ont, lex);
    CHECK(n >= 2);
    // 近义：拔出↔持械 双节点都在 ⇒ SIMILAR 边（score 作权重）
    CHECK(ont.has_edge(make_concept_id("拔出"), make_concept_id("持械"), RelType::SIMILAR));
    // 反义：警觉↔放松 ⇒ OPPOSITE 边
    CHECK(ont.has_edge(make_concept_id("警觉"), make_concept_id("放松"), RelType::OPPOSITE));
    // 近义词"袭击"没有概念节点 ⇒ 宽容跳过，不建边
    CHECK(!ont.has_edge(make_concept_id("攻击"), make_concept_id("袭击"), RelType::SIMILAR));
    // 强同义不建边（别名表归并，单节点）
    CHECK(!ont.has_edge(make_concept_id("拔出"), make_concept_id("抽出"), RelType::SIMILAR));
}

TEST(lexicon_errors) {
    Lexicon lex;
    CHECK_THROWS_AS(lex.load(kAssets + "/no_such_file.json"), KnowledgeError);
    CHECK_THROWS_AS(lex.load_json(json::object()), KnowledgeError);
}

// ---- ame 词汇层桥接（复刻 lexicon_apply 三分流，驱动 ame::KeywordManager）----

TEST(ame_lexicon_three_way) {
    // 临时词典：强同义 / 近义（含低分过滤）/ 反义 / 旧版 synonyms+related 字段
    const std::string path = "test_ame_lexicon_fixture.json";
    {
        std::ofstream f(path);
        f << R"({"version":"t","entries":[
            {"word":"拔出","syn_strong":["拔","抽拔"]},
            {"word":"攻击","syn_near":[{"word":"袭击","score":0.85},{"word":"弱分","score":0.5}]},
            {"word":"警觉","antonyms":["放松"]},
            {"word":"great","synonyms":["excellent"],"related":[{"word":"good","score":0.9}]}
        ]})";
    }
    ame::Storage st;
    ame::KeywordManager kw(st);
    int n = load_ame_lexicon(kw, path);
    CHECK(n >= 2);
    // 强同义 → 别名归一（含旧版 synonyms）
    CHECK_EQ(kw.normalize("拔"), std::string("拔出"));
    CHECK_EQ(kw.normalize("excellent"), std::string("great"));
    // 近义词表：score>0.6 过滤 + 对称登记（非主词反向降级）
    CHECK_EQ(kw.near_words("攻击").size(), (size_t)1);
    CHECK_EQ(kw.near_words("袭击").size(), (size_t)1);
    CHECK_EQ(kw.near_words("袭击")[0].first, std::string("攻击"));
    CHECK_EQ(kw.near_words("弱分").size(), (size_t)0);
    // 旧版 related 视作近义
    CHECK_EQ(kw.near_words("great").size(), (size_t)1);

    // 节点不存在时不建边；双方 Keyword 节点存在后重灌 ⇒ RELATED 边落地
    CHECK(st.edge_count() == 0);
    kw.get_or_create("攻击", "t");
    kw.get_or_create("袭击", "t");
    kw.get_or_create("警觉", "t");
    kw.get_or_create("放松", "t");
    load_ame_lexicon(kw, path);
    const ame::Node* a = kw.find("攻击");
    const ame::Node* x = kw.find("袭击");
    const ame::Node* jg = kw.find("警觉");
    const ame::Node* fs = kw.find("放松");
    CHECK(a && x && jg && fs);
    CHECK(st.find_edge(a->uid, x->uid, ame::RelType::REL_SIMILAR) != nullptr);
    CHECK(st.find_edge(x->uid, a->uid, ame::RelType::REL_SIMILAR) != nullptr);  // 无向补反向
    CHECK(st.find_edge(jg->uid, fs->uid, ame::RelType::REL_OPPOSITE) != nullptr);

    CHECK_THROWS_AS(load_ame_lexicon(kw, "no_such_file.json"), KnowledgeError);
    std::remove(path.c_str());
}

TEST(surface_forms_and_near_candidates) {
    ame::Storage st;
    ame::KeywordManager kw(st);
    Lexicon lex;
    lex.load(kAssets + "/lexicon.json");
    // ame 层命中（"抽拔" 只在 ame 词典里）：词形集合含归一形，原词永远优先
    const std::string path = "test_ame_chain_fixture.json";
    {
        std::ofstream f(path);
        f << R"({"entries":[{"word":"拔出","syn_strong":["抽拔"]}]})";
    }
    load_ame_lexicon(kw, path);
    auto forms = surface_forms(&kw, lex, "抽拔");
    CHECK_EQ(forms[0], std::string("抽拔"));
    CHECK(std::find(forms.begin(), forms.end(), "拔出") != forms.end());
    // ame 未命中，种子词典归一形也在集合里（"抽出" 只在种子词典里）
    forms = surface_forms(&kw, lex, "抽出");
    CHECK_EQ(forms[0], std::string("抽出"));
    CHECK(std::find(forms.begin(), forms.end(), "拔出") != forms.end());
    // 两层都未命中只有原词；ame 层为空也可工作
    CHECK_EQ(surface_forms(&kw, lex, "勇者").size(), (size_t)1);
    CHECK(surface_forms(nullptr, lex, "抽出").size() >= 2);
    // 近义候选 = 种子词典 ∪ ame 层
    auto cands = near_candidates(&kw, lex, "攻击");
    CHECK(!cands.empty());
    std::remove(path.c_str());
}

int main() { return tfw::run_all(); }
