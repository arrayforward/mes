#pragma once

// ============================================================================
// mse/knowledge_assist.h —— knowledge 协助:属性提取 + 规则协助推理
//
// 用户约定:"knowledge 可以协助进行属性的提取和一部分的规则协助推理"。
//
// 纪律(API 文档 §七):knowledge 的输出永远只是**建议**——提取的属性进入
// 候选的 evidence 或作为候选草稿,协助推理的产物只是新候选;它们都必须经
// 写侧管线四层校验才可能成为事实。"LLM/AI 不碰状态"铁律在此同样成立:
// 本类没有任何写路径,只返回建议。
//
// 机制:
//   属性提取——对自由文本(备注/模糊时空原文)做滑窗子串枚举,经 knowledge
//   词形归一(surface_forms/lexicon normalize)后,与属性字典的已登记键及
//   其值域做命中匹配,产出建议的 {键: 值}。
//   规则协助推理——对概念(事件类型/属性键)调 knowledge::Engine::infer,
//   取规则前向链(TRIGGERS/CAUSES 等关系)命中的后继概念,映射为建议的
//   后续事件类型(仅建议,回到管线由规则过滤兜底)。
// ============================================================================

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mse/model.h"

namespace knowledge { class Engine; }

namespace mse {

struct AttributeEntry;

class KnowledgeAssist {
public:
    KnowledgeAssist();
    ~KnowledgeAssist();

    /// 加载制造域本体与词典(knowledge 资产 schema,见 mse/assets/)。
    /// 路径为空则跳过对应加载。返回是否至少加载成功一项。
    bool load(const std::string& ontology_seed_path, const std::string& lexicon_path);
    bool loaded() const;

    /// 属性提取:自由文本 → 建议的 {属性键: 值}。
    /// 只产出 entries 中已登记且 active 的键;值须落在该键值域内(值域空则
    /// 只匹配键名本身,值取命中的文本片段)。
    std::map<std::string, json> extract_attributes(
        const std::string& text, const std::vector<AttributeEntry>& entries) const;

    /// 规则协助推理:概念 → 建议的后续事件类型名(经知识图谱 TRIGGERS/CAUSES
    /// 前向链)。candidates 为可映射的事件类型全集(已注册类型名)。
    std::vector<std::string> suggest_followups(
        const std::string& concept_name, const std::vector<std::string>& candidates) const;

private:
    std::unique_ptr<knowledge::Engine> eng_;
    bool loaded_ = false;
};

} // namespace mse
