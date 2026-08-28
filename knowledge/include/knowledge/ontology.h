#pragma once

// ============================================================================
// 模块：图谱本体（ontology）——概念/实例/边的内存图 + is-a/part-of 传递
//   闭包 + 规则库（沿 is-a 继承）+ 版本化资产加载 + JSON 快照。
// 这是 ame 缺失、本库补齐的核心：真正的本体层级与符号规则。
// ============================================================================

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "knowledge/conduction.h"
#include "knowledge/types.h"

namespace knowledge {

using nlohmann::json;

class Ontology {
public:
    // ---- 概念与实例（按 name 幂等复用） ----

    ConceptNode& add_concept(const std::string& name, Attrs attrs = {});
    InstanceNode& add_instance(const std::string& name, const std::string& entity_ref = "",
                               Attrs attrs = {});

    const ConceptNode* find_concept(const std::string& name) const;    // 按规范名
    const ConceptNode* concept_of(const std::string& concept_id) const;   // 按 id
    const InstanceNode* find_instance(const std::string& name) const;
    const InstanceNode* instance_of(const std::string& instance_id) const;
    /// 按 storage Entity.type 找初始类型概念（概念 attrs["entity_type"] 匹配）。
    const ConceptNode* concept_by_entity_type(const std::string& type) const;
    /// id 是否为图内节点（概念或实例）。OBSERVED_IN 的 to 不是图节点。
    bool has_node(const std::string& id) const;

    // ---- 边 ----

    /// 加边：同 (from,to,type) 去重合并（weight 取大、attrs 并集）。
    void add_edge(Edge e);
    /// 共现生长：同 (from,to,type) weight += delta，不存在则以 delta 建边（镜像 ame auto_grow）。
    void upsert_edge(const std::string& from, const std::string& to, RelType type,
                     double delta = 1.0);
    bool has_edge(const std::string& from, const std::string& to, RelType type) const;
    std::vector<Edge> edges_from(const std::string& node_id) const;
    std::vector<Edge> edges_to(const std::string& node_id) const;
    size_t edge_count() const { return edges_.size(); }

    // ---- is-a / part-of 传递闭包（图小，沿边 BFS 按需计算） ----

    bool is_a(const std::string& sub_id, const std::string& super_id) const;
    /// IS_A 上溯闭包（不含自身），近祖先在前。
    std::vector<std::string> ancestors(const std::string& concept_id) const;
    /// IS_A 下行闭包（不含自身）。
    std::vector<std::string> descendants(const std::string& concept_id) const;
    /// PART_OF 上溯闭包（不含自身）。
    std::vector<std::string> part_of_ancestors(const std::string& concept_id) const;
    /// IS_A 上溯路径（含两端），sub 不是 super 的子类时返回空。推理路径可解释用。
    std::vector<std::string> is_a_path(const std::string& sub_id,
                                       const std::string& super_id) const;

    // ---- 规则库 ----

    void add_rule(InferenceRule r);   // 同 rule_id 去重
    const std::vector<InferenceRule>& rules() const { return rules_; }
    /// 概念自身 + is-a 祖先上挂载的全部规则（规则沿 is-a 继承）。
    std::vector<InferenceRule> rules_for(const std::string& concept_id) const;

    // ---- 版本化资产加载（运行时只读，变更需升版本） ----

    void load(const std::string& path);
    void load_json(const json& j);
    const std::string& version() const { return version_; }

    // ---- 运行时图 JSON 快照（崩溃可重建；事实源仍在 storage，重新 ingest 亦可） ----

    json snapshot() const;
    void load_snapshot(const json& j);

private:
    std::vector<std::string> bfs_up(const std::string& concept_id, RelType type) const;

    std::string version_;
    std::unordered_map<std::string, ConceptNode> concepts_;         // id -> node
    std::unordered_map<std::string, InstanceNode> instances_;
    std::unordered_map<std::string, std::string> concept_by_name_;  // name -> id
    std::unordered_map<std::string, std::string> instance_by_name_;
    std::vector<Edge> edges_;
    std::unordered_map<std::string, std::vector<size_t>> out_;      // 邻接：node -> 边下标
    std::unordered_map<std::string, std::vector<size_t>> in_;
    std::unordered_map<std::string, size_t> edge_key_;              // "from|to|type" -> 下标
    std::vector<InferenceRule> rules_;
};

} // namespace knowledge
