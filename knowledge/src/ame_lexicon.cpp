// ============================================================================
// 模块：ame 词汇层桥接实现——复刻 ame/bench/src/lexicon_apply.cpp。
// 与 ame 原版的唯一差异：near 表对称登记（add_near 双向各一条），使"袭击"
//   这类非主词也能反向降级扩展到主词概念（knowledge 摄入/推理管道需要）；
//   其余分流、互为同义组防互指、score>0.6 过滤、每词 ≤8 条、节点存在才建边
//   均与原版一致。
// ============================================================================

#include "knowledge/ame_lexicon.h"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "ame/keyword/keyword_manager.h"
#include "knowledge/lexicon.h"
#include "knowledge/types.h"

namespace knowledge {

int load_ame_lexicon(ame::KeywordManager& kw, const std::string& path) {
    std::ifstream f(path);
    if (!f) throw KnowledgeError("ame_lexicon: cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) throw KnowledgeError("ame_lexicon: parse error in " + path);
    if (!j.contains("entries") || !j["entries"].is_array())
        throw KnowledgeError("ame_lexicon: missing entries array in " + path);

    // 主词 -> strong 集合：本身是主词的词，仅当与当前词互为同义组成员
    // （双向 strong 互指）时才允许归并；多义词单向互指跳过（同 ame 原版）。
    std::unordered_map<std::string, std::unordered_set<std::string>> strong_of;
    for (const auto& e : j["entries"]) {
        std::string w = e.value("word", "");
        if (w.empty()) continue;
        std::unordered_set<std::string> ss;
        const nlohmann::json* strong = e.contains("syn_strong") ? &e["syn_strong"]
                                       : e.contains("synonyms") ? &e["synonyms"] : nullptr;
        if (strong)
            for (const auto& s : *strong)
                if (s.is_string()) ss.insert(s.get<std::string>());
        strong_of[w] = std::move(ss);
    }
    auto alias_ok = [&](const std::string& word, const std::string& syn) {
        auto it = strong_of.find(syn);
        if (it == strong_of.end()) return true;   // 非主词：直接归并
        return it->second.count(word) > 0;        // 主词：仅互为同义组才归并
    };

    int n = 0;
    for (const auto& e : j["entries"]) {
        std::string word = e.value("word", "");
        if (word.empty()) continue;
        // 同义 → 别名归并（syn_strong，兼容旧版 synonyms）
        const nlohmann::json* strong = e.contains("syn_strong") ? &e["syn_strong"]
                                       : e.contains("synonyms") ? &e["synonyms"] : nullptr;
        if (strong)
            for (const auto& s : *strong) {
                if (!s.is_string()) continue;
                std::string syn = s.get<std::string>();
                if (!syn.empty() && alias_ok(word, syn)) {
                    kw.add_alias(syn, word);
                    ++n;
                }
            }
        // 近义 → 近义词表（对称登记）+ REL_SIMILAR 边（score>0.6、≤8 条、节点存在）
        const nlohmann::json* near = e.contains("syn_near") ? &e["syn_near"] : nullptr;
        if (near) {
            int cnt = 0;
            for (const auto& r : *near) {
                if (cnt >= 8) break;
                std::string rw = r.value("word", "");
                double score = r.value("score", 0.7);
                if (rw.empty() || score <= 0.6) continue;
                kw.add_near(word, rw, score);
                kw.add_near(rw, word, score);  // 对称：反向降级扩展（与 ame 原版之差）
                ++cnt;
                if (kw.link_related(word, rw, ame::RelType::REL_SIMILAR, score) == ame::Err::Ok)
                    ++n;
            }
        }
        // 反义 → REL_OPPOSITE 边（最低档系数）
        if (e.contains("antonyms"))
            for (const auto& s : e["antonyms"]) {
                if (!s.is_string()) continue;
                if (kw.link_related(word, s.get<std::string>(), ame::RelType::REL_OPPOSITE,
                                    1.0) == ame::Err::Ok)
                    ++n;
            }
        // 旧版 related 字段兼容（视作近义，同 ame 原版）
        const nlohmann::json* rel = e.contains("related") ? &e["related"] : nullptr;
        if (rel && !near)
            for (const auto& r : *rel) {
                std::string rw = r.value("word", "");
                double score = r.value("score", 0.0);
                if (rw.empty() || score <= 0.6) continue;
                kw.add_near(word, rw, score);
                kw.add_near(rw, word, score);
                if (kw.link_related(word, rw, ame::RelType::REL_SIMILAR, score) == ame::Err::Ok)
                    ++n;
            }
    }
    return n;
}

std::vector<std::string> surface_forms(const ame::KeywordManager* ame, const Lexicon& lex,
                                       const std::string& word) {
    std::vector<std::string> forms;
    auto push = [&](const std::string& w) {
        if (w.empty()) return;
        for (const auto& x : forms)
            if (x == w) return;
        forms.push_back(w);
    };
    push(word);  // 原词永远优先（命名与精确匹配的基准）
    if (ame) {
        const std::string a = ame->normalize(word);
        push(a);             // ame 层归一（真实词典）
        push(lex.normalize(a));  // ame → 种子 链式
    }
    push(lex.normalize(word));  // 种子词典归一
    return forms;
}

std::vector<std::pair<std::string, double>>
near_candidates(const ame::KeywordManager* ame, const Lexicon& lex, const std::string& word) {
    std::vector<std::pair<std::string, double>> out;
    for (const auto& form : surface_forms(ame, lex, word)) {
        for (const auto& p : lex.near_of(form)) out.push_back(p);
        if (ame)
            for (const auto& [w, s] : ame->near_words(form, 8)) out.push_back({w, s});
    }
    return out;
}

} // namespace knowledge
