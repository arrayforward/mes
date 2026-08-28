// ============================================================================
// 模块：词典实现（镜像 ame bench/src/lexicon_apply.cpp 的三分流）。
// 本文件主体思路：
//   步骤1：解析 entries；syn_strong（兼容旧版 synonyms）→ 别名归一表
//         （主词别名需互为同义组成员，防多义词互指稀释，如 great↔big）；
//   步骤2：syn_near 登记近义词表（score>0.6、每词 ≤8 条）；
//   步骤3：apply_lexicon 把近义/反义落成 SIMILAR / OPPOSITE 边
//         （双方概念存在才建边，宽容跳过）。
// ============================================================================

#include "knowledge/lexicon.h"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "knowledge/ontology.h"

namespace knowledge {

static const std::vector<std::pair<std::string, double>> kEmptyNear;

void Lexicon::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw KnowledgeError("lexicon: cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) throw KnowledgeError("lexicon: parse error in " + path);
    load_json(j);
}

void Lexicon::load_json(const json& j) {
    version_ = j.value("version", "");
    if (!j.contains("entries") || !j["entries"].is_array())
        throw KnowledgeError("lexicon: missing entries array");

    entries_.clear();
    alias_.clear();
    for (const auto& e : j["entries"]) {
        Entry en;
        en.word = e.value("word", "");
        if (en.word.empty()) continue;
        // 兼容旧版 synonyms 字段
        const json* strong = e.contains("syn_strong") ? &e["syn_strong"]
                             : e.contains("synonyms") ? &e["synonyms"] : nullptr;
        if (strong)
            for (const auto& s : *strong)
                if (s.is_string() && !s.get<std::string>().empty())
                    en.syn_strong.push_back(s.get<std::string>());
        if (e.contains("syn_near")) {
            int cnt = 0;
            for (const auto& r : e["syn_near"]) {
                if (cnt >= 8) break;  // 每词至多 8 条近义
                double score = r.value("score", 0.7);
                std::string w = r.value("word", "");
                if (w.empty() || score <= 0.6) continue;  // 低分近义不入表
                en.syn_near.push_back({std::move(w), score});
                ++cnt;
            }
        }
        if (e.contains("antonyms"))
            for (const auto& s : e["antonyms"])
                if (s.is_string() && !s.get<std::string>().empty())
                    en.antonyms.push_back(s.get<std::string>());
        entries_.push_back(std::move(en));
    }

    // 主词 -> strong 集合：本身是主词的词，仅当与当前词互为同义组成员
    // （双向 strong 互指）时才允许归并；多义词单向互指跳过。
    std::unordered_map<std::string, std::unordered_set<std::string>> strong_of;
    for (const auto& en : entries_)
        strong_of[en.word] = {en.syn_strong.begin(), en.syn_strong.end()};
    auto alias_ok = [&](const std::string& word, const std::string& syn) {
        auto it = strong_of.find(syn);
        if (it == strong_of.end()) return true;         // 非主词：直接归并
        return it->second.count(word) > 0;              // 主词：仅互为同义组才归并
    };
    for (const auto& en : entries_)
        for (const auto& s : en.syn_strong)
            if (alias_ok(en.word, s)) alias_[s] = en.word;

    // 灌库时清空对称近义表重建
    near_.clear();
    for (const auto& en : entries_) {
        for (const auto& [w, score] : en.syn_near) {
            near_[en.word].push_back({w, score});
            auto& rev = near_[w];
            bool dup = false;
            for (const auto& [x, _s] : rev)
                if (x == en.word) {
                    dup = true;
                    break;
                }
            if (!dup) rev.push_back({en.word, score});
        }
    }
}

std::string Lexicon::normalize(const std::string& word) const {
    auto it = alias_.find(word);
    return it == alias_.end() ? word : it->second;
}

const std::vector<std::pair<std::string, double>>&
Lexicon::near_of(const std::string& word) const {
    auto it = near_.find(normalize(word));
    return it == near_.end() ? kEmptyNear : it->second;
}

int apply_lexicon(Ontology& ont, const Lexicon& lex) {
    int n = 0;
    for (const auto& en : lex.entries()) {
        const ConceptNode* wc = ont.find_concept(en.word);
        // 近义 -> SIMILAR 边（保留双节点，只经图扩散生效，不进别名表）
        for (const auto& [w, score] : en.syn_near) {
            const ConceptNode* nc = ont.find_concept(lex.normalize(w));
            if (wc && nc) {
                ont.add_edge({wc->concept_id, nc->concept_id, RelType::SIMILAR, score, {}});
                ++n;
            }
        }
        // 反义 -> OPPOSITE 边（最低档系数，扩散中不会被当强联想）
        for (const auto& a : en.antonyms) {
            const ConceptNode* ac = ont.find_concept(lex.normalize(a));
            if (wc && ac) {
                ont.add_edge({wc->concept_id, ac->concept_id, RelType::OPPOSITE, 1.0, {}});
                ++n;
            }
        }
    }
    return n;
}

} // namespace knowledge
