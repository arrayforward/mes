#pragma once

// ============================================================================
// 模块：knowledge 公共类型层——语义图谱树全库统一的图数据模型。
// 本体层级（IS_A/PART_OF）是 ame 缺失、本库补齐的能力；语义关系镜像 ame
// RELATED 六类并扩展（TRIGGERS 为规则推理主通道）；桥接层把概念/实例锚回
// storage 的事件侧写（事实仍由 storage 唯一持有，此处只存可审计的回指）。
// ============================================================================

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace knowledge {

using Attrs = std::unordered_map<std::string, std::string>;

/// 库统一错误类型（与 storage 约定一致：派生自 std::runtime_error）。
class KnowledgeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// 确定性 id 派生（保留 storage 前缀约定：cp- 概念、in- 实例）。
/// 同一种子资产多次加载 id 稳定，运行时快照因此可跨进程移植恢复。
std::string make_concept_id(const std::string& name);
std::string make_instance_id(const std::string& name);

// 关系类型（详见 docs/design.md 关系分类法）
enum class RelType {
    // 本体层级（ame 缺失，新增；均传递）
    IS_A,          // 概念 -> 概念（子类）
    PART_OF,       // 概念 -> 概念（组成）
    // 语义关系（镜像 ame RELATED 并扩展）
    CAUSES,        // 因果 0.9
    TRIGGERS,      // 触发/使能 0.85（规则推理主通道）
    REQUIRES,      // 前置条件 0.8
    PREVENTS,      // 抑制 0.4
    SIMILAR,       // 相似 0.7（近义词保留双节点，只建弱边）
    OPPOSITE,      // 对立 0.5（最低档，防反义被当强联想）
    COOCCUR,       // 共现 0.6（写入即生长）
    BEFORE,        // 时序 0.7（同步 storage Link.before；镜像 ame NEXT）
    // 桥接层（概念/实例 -> 世界证据）
    INSTANCE_OF,   // 实例 -> 概念
    OBSERVED_IN,   // 概念/实例 -> 侧写锚点 {profile_id, event_id}
};

/// 本体概念（运行时只读，变更需版本化）。
struct ConceptNode {
    std::string concept_id;  // "cp-" 前缀（种子资产内由名字确定性派生，快照可移植）
    std::string name;        // 规范名（词典归一化后的词形）
    Attrs attrs;             // 属性定义：entity_type / 取值域 / 单位等
};

/// 世界实例（从侧写浮现，运行时生长）。
struct InstanceNode {
    std::string instance_id;  // "in-" 前缀
    std::string name;         // surface 字符串（归一化后）
    std::string entity_ref;   // storage 的 entity_id（可空）
    Attrs attrs;              // event 实例携带 event_id/profile_id/perspective/time 等
};

/// 图边：from/to 为节点 id；OBSERVED_IN 的 to 是 storage profile_id（非图节点）。
struct Edge {
    std::string from, to;
    RelType type = RelType::COOCCUR;
    double weight = 1.0;
    Attrs attrs;  // event_id / crystallized / note ...
};

/// 语义规则（v2 §4.1 "所有 CombatAction 可能触发 Aggro"）。
struct InferenceRule {
    std::string rule_id;      // "rl-" 前缀
    std::string antecedent;   // 前提概念 id
    RelType relation = RelType::TRIGGERS;  // TRIGGERS | CAUSES | REQUIRES | PREVENTS
    std::string consequent;   // 结论概念 id
    double confidence = 1.0;
    std::string note;
};

/// 一条推理候选：规则前向链的产出，带可解释路径。
struct Inference {
    std::string rule_id;
    std::string subject;               // 触发主体（事件/实例名）
    std::string candidate;             // 结论概念名
    RelType relation = RelType::TRIGGERS;
    double confidence = 0.0;           // 规则置信度 × 绑定/匹配置信度
    std::vector<std::string> path;     // 可解释路径（人读步骤序列）
    bool conflict = false;             // 与 PREVENTS 结论冲突标注（供审计）
};

} // namespace knowledge
