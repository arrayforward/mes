// ============================================================================
// mse/knowledge_assist.cpp —— knowledge 协助:属性提取 + 规则协助推理
//
// 纪律:本类没有任何写路径,输出永远只是建议(见头文件注释)。
//   属性提取 = UTF-8 滑窗子串枚举 → 词形归一 → 命中字典键/值域;
//   协助推理 = knowledge::Engine::infer 前向链 → 映射回已注册事件类型名。
// ============================================================================

#include "mse/knowledge_assist.h"

#include <set>
#include <utility>

#include "knowledge/engine.h"
#include "knowledge/lexicon.h"
#include "knowledge/types.h"
#include "mse/dictionary.h"

namespace mse {

namespace {

// ---- UTF-8 字符边界切分:返回每个字符的字节区间 [offset, len) ----
std::vector<std::pair<size_t, size_t>> utf8_chars(const std::string& text) {
    std::vector<std::pair<size_t, size_t>> chars;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((b & 0x80) == 0) len = 1;             // ASCII
        else if ((b & 0xE0) == 0xC0) len = 2;     // 2 字节
        else if ((b & 0xF0) == 0xE0) len = 3;     // 3 字节(中文常见)
        else if ((b & 0xF8) == 0xF0) len = 4;     // 4 字节
        if (i + len > text.size()) len = 1;       // 截断的非法序列按单字节跳过
        chars.emplace_back(i, len);
        i += len;
    }
    return chars;
}

// json 值域成员 → 可匹配文本(字符串直取,其余规范序列化,如 2 → "2")
std::string member_text(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    return v.dump();
}

} // namespace

KnowledgeAssist::KnowledgeAssist() : eng_(std::make_unique<knowledge::Engine>()) {}
KnowledgeAssist::~KnowledgeAssist() = default;

bool KnowledgeAssist::load(const std::string& ontology_seed_path,
                           const std::string& lexicon_path) {
    // 任一资产加载成功即视为可用;单项失败只损失对应能力,不影响另一项。
    if (!ontology_seed_path.empty()) {
        try {
            eng_->define_ontology(ontology_seed_path);
            loaded_ = true;
        } catch (const std::exception&) {
            // 资产缺失/格式错误:本体推理退化,继续尝试词典
        }
    }
    if (!lexicon_path.empty()) {
        try {
            eng_->apply_lexicon(lexicon_path);
            loaded_ = true;
        } catch (const std::exception&) {
            // 同上:词形归一退化,纯子串匹配仍可用
        }
    }
    return loaded_;
}

bool KnowledgeAssist::loaded() const { return loaded_; }

std::map<std::string, json> KnowledgeAssist::extract_attributes(
    const std::string& text, const std::vector<AttributeEntry>& entries) const {
    std::map<std::string, json> suggestions;
    if (text.empty()) return suggestions;

    // 枚举长度 2..8 字符的全部子串(中文滑窗,按 UTF-8 字符边界切)
    const auto chars = utf8_chars(text);
    std::vector<std::pair<std::string, size_t>> windows;  // (子串, 字符数)
    for (size_t i = 0; i < chars.size(); ++i) {
        for (size_t len = 2; len <= 8 && i + len <= chars.size(); ++len) {
            const size_t begin = chars[i].first;
            const size_t end = chars[i + len - 1].first + chars[i + len - 1].second;
            windows.emplace_back(text.substr(begin, end - begin), len);
        }
    }

    // 同键多命中取最长文本命中:best[key] = (命中字符数, 值)
    std::map<std::string, std::pair<size_t, json>> best;
    auto consider = [&](const std::string& key, size_t hit_len, json value) {
        auto it = best.find(key);
        if (it == best.end() || hit_len > it->second.first)
            best[key] = {hit_len, std::move(value)};
    };

    for (const auto& [sub, sub_len] : windows) {
        // knowledge 未加载时退化为纯子串匹配(不做归一)
        const std::string norm =
            loaded_ ? eng_->lexicon().normalize(sub) : sub;

        for (const auto& e : entries) {
            if (e.status != "active") continue;

            // ① 键名命中:子串(或归一后)== 键名
            if (sub == e.key || norm == e.key) {
                if (e.range.empty()) {
                    // 值域空:只匹配键名本身,值取命中的文本片段
                    consider(e.key, sub_len, json(sub));
                } else {
                    // 值域非空:在全文中找值域成员(或其近义)命中,值取规范成员
                    for (const auto& m : e.range) {
                        const std::string mt = member_text(m);
                        if (mt.empty()) continue;
                        if (text.find(mt) != std::string::npos) {
                            consider(e.key, utf8_chars(mt).size(), m);
                            continue;
                        }
                        if (loaded_) {
                            for (const auto& [near_word, score] :
                                 eng_->lexicon().near_of(mt)) {
                                (void)score;
                                if (!near_word.empty() &&
                                    text.find(near_word) != std::string::npos)
                                    consider(e.key, utf8_chars(near_word).size(), m);
                            }
                        }
                    }
                }
                continue;
            }

            // ② 值域成员直接命中:子串(或归一后)== 值域成员
            for (const auto& m : e.range) {
                const std::string mt = member_text(m);
                if (!mt.empty() && (sub == mt || norm == mt))
                    consider(e.key, sub_len, m);
            }
        }
    }

    for (auto& [key, hit] : best) suggestions[key] = std::move(hit.second);
    return suggestions;
}

std::vector<std::string> KnowledgeAssist::suggest_followups(
    const std::string& concept_name, const std::vector<std::string>& candidates) const {
    if (!loaded_ || concept_name.empty()) return {};

    const std::set<std::string> cand_set(candidates.begin(), candidates.end());
    std::set<std::string> seen;  // 去重保序
    std::vector<std::string> out;

    knowledge::ReasoningChain chain = eng_->infer(concept_name);
    for (const auto& inf : chain.inferences) {
        if (inf.relation != knowledge::RelType::TRIGGERS &&
            inf.relation != knowledge::RelType::CAUSES &&
            inf.relation != knowledge::RelType::REQUIRES)
            continue;
        if (cand_set.count(inf.candidate) == 0) continue;
        if (seen.insert(inf.candidate).second) out.push_back(inf.candidate);
    }
    return out;
}

} // namespace mse
