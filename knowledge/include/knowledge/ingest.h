#pragma once

// ============================================================================
// 模块：侧写摄入（ingest）——storage 事件侧写 → 语义图的只读桥接，
//   镜像 ame "写入即生长"。八步管道见 ingest.cpp 与 docs/design.md。
// ============================================================================

#include <string>
#include <unordered_map>
#include <vector>

#include "knowledge/lexicon.h"
#include "knowledge/ontology.h"
#include "knowledge/types.h"

namespace eventstore {
class EventStore;
}

namespace ame {
class KeywordManager;
}

namespace knowledge {

/// 单条侧写的理解结果。
struct IngestResult {
    std::string profile_id;
    std::string event_instance_id;    // 事件实例节点（五要素合成）
    std::string verb_concept_id;      // 动词概念（未命中为空）
    double verb_match = 0.0;          // 动词概念匹配权重：精确 1.0 / 近义降级 score
    std::vector<std::string> chain;   // 事件分类链（动词概念 → … → 根，概念名）
    std::string event_class;          // 顶类（根的直接子类，如"战斗动作"）
    std::vector<Inference> inferences;      // 规则前向链命中
    std::vector<std::string> instance_ids;  // 涉及的全部节点 id
};

class Ingester {
public:
    Ingester(Ontology& ont, const Lexicon& lex) : ont_(ont), lex_(lex) {}

    /// 接入 ame 词汇层（可空）：归一/近义降级优先走真实词典，并在摄入时生长
    /// ame 侧 Keyword 节点与 RELATED 边（软通道素材）。
    void set_ame_layer(ame::KeywordManager* kw) { ame_ = kw; }

    /// 摄入单条侧写（幂等：重复摄入返回缓存结果，不重复生长共现边）。
    IngestResult ingest_profile(eventstore::EventStore& store, const std::string& profile_id);
    /// 摄入 store 内全部侧写（query_profiles() 无过滤枚举），随后统一同步 Link 边。
    std::vector<IngestResult> ingest_all(eventstore::EventStore& store);
    /// 摄入某叙事的全部侧写（timeline 拓扑序），随后统一同步 Link 边。
    std::vector<IngestResult> ingest_narrative(eventstore::EventStore& store,
                                               const std::string& narrative_id);
    /// 第 7 步：把 storage Link.causes/before 同步为事件实例间的 CAUSES/BEFORE 边
    /// （目标侧写已摄入才建边）。
    void sync_links(eventstore::EventStore& store, const std::string& profile_id);

    bool ingested(const std::string& profile_id) const { return results_.count(profile_id) > 0; }

private:
    Ontology& ont_;
    const Lexicon& lex_;
    ame::KeywordManager* ame_ = nullptr;                    // ame 词汇层（可空）
    std::unordered_map<std::string, IngestResult> results_;        // profile_id -> 结果（幂等缓存）
    std::unordered_map<std::string, std::string> event_node_of_;   // profile_id -> 事件实例 id
};

/// 矛盾标注：同一 (subject, candidate) 同时被 TRIGGERS/CAUSES 与 PREVENTS
/// 推出时，两侧都标 conflict（供审计，不删除）。
void mark_conflicts(std::vector<Inference>& inferences);

} // namespace knowledge
