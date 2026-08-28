#pragma once

// ============================================================================
// 模块：词典（lexicon）——别名归一表 + 近义/反义分流（镜像 ame M3 与
//   bench/lexicon_apply 的三分流设计）。
//   syn_strong 归并为单节点（别名表，不建边）；syn_near 保留双节点建
//   SIMILAR 弱边；antonyms 建 OPPOSITE 最低档边。
// ============================================================================

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "knowledge/types.h"

namespace knowledge {

using nlohmann::json;

class Ontology;  // 前向声明：apply_lexicon 需要往图谱建边

/// 词典：写入侧与查询侧词形对齐的唯一入口。
class Lexicon {
public:
    struct Entry {
        std::string word;
        std::vector<std::string> syn_strong;                  // 强同义 -> 别名归并
        std::vector<std::pair<std::string, double>> syn_near; // 近义（带分数）-> SIMILAR 边
        std::vector<std::string> antonyms;                    // 反义 -> OPPOSITE 边
    };

    /// 从 JSON 文件 / JSON 对象加载（ame schema：{version, entries:[...]}）。
    void load(const std::string& path);
    void load_json(const json& j);

    const std::string& version() const { return version_; }

    /// 别名归一：命中别名表返回规范词，未命中返回原词（O(1)）。
    std::string normalize(const std::string& word) const;

    /// 近义词表（score>0.6、每词 ≤8 条，灌库时已过滤）；无记录返回空表。
    const std::vector<std::pair<std::string, double>>& near_of(const std::string& word) const;

    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::string version_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::string> alias_;  // synonym -> canonical
    // 对称近义表（含反向映射：非主词也能降级扩展到主词概念）
    std::unordered_map<std::string, std::vector<std::pair<std::string, double>>> near_;
};

/// 词典三分流灌库：强同义已在 load 时进别名表；此处把近义/反义落成
/// 概念间 SIMILAR / OPPOSITE 边（双方概念都存在才建，宽容跳过）。
/// 返回建边数。
int apply_lexicon(Ontology& ont, const Lexicon& lex);

} // namespace knowledge
