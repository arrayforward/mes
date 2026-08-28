#pragma once

// ============================================================================
// 模块：ame 词汇层桥接（ame_lexicon）——复刻 ame/bench/src/lexicon_apply.cpp
//   的三分流灌库逻辑，驱动 knowledge::Engine 持有的 ame::KeywordManager +
//   ame::Storage；并提供 ingest/reasoner 共用的归一/近义助手（ame 层优先，
//   本体种子词典兜底）。
// ============================================================================

#include <string>
#include <utility>
#include <vector>

namespace ame {
class KeywordManager;
}

namespace knowledge {

class Lexicon;

/// 词典灌库（ame schema：{version, entries:[{word, syn_strong, syn_near, antonyms}]}，
/// 兼容旧版 synonyms/related 字段）：
///   syn_strong → 别名归一表（主词别名需互为同义组成员，防多义词互指稀释）；
///   syn_near  → 近义词表（score>0.6、每词 ≤8 条；对称登记，非主词也能反向降级）
///               + 双方 Keyword 节点存在时建 REL_SIMILAR 边；
///   antonyms  → 双方节点存在时建 REL_OPPOSITE 边（最低档系数）。
/// 幂等、可多文件并集加载。返回灌入条目数。
int load_ame_lexicon(ame::KeywordManager& kw, const std::string& path);

/// 词形集合（匹配扩展用）：原词 + ame 层归一 + 种子词典归一（含链式组合），有序去重。
/// 真实大词典（中文词林）的同义群较松，争议词的别名归一噪声大（如 勇者→硬汉、
/// 进攻→进击），故归一结果只用于"匹配扩展"，实例命名一律保留原始 surface。
std::vector<std::string> surface_forms(const ame::KeywordManager* ame, const Lexicon& lex,
                                       const std::string& word);

/// 近义降级候选：各词形的 ame 层 near_words ∪ 种子词典 near_of（未去重，取最优分）。
std::vector<std::pair<std::string, double>>
near_candidates(const ame::KeywordManager* ame, const Lexicon& lex, const std::string& word);

} // namespace knowledge
