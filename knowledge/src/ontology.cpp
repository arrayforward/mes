// ============================================================================
// 模块：图谱本体实现。
// 本文件主体思路：内存图（哈希表 + 邻接下标表）；is-a/part-of 闭包沿边
//   BFS 按需计算（种子本体规模小，无需物化闭包表）；规则沿 is-a 继承
//   即 rules_for = 自身规则 + 各祖先规则；资产 JSON 以概念名引用、加载时
//   解析为确定性 id。
// ============================================================================

#include "knowledge/ontology.h"

#include <algorithm>
#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_set>

namespace knowledge {

// ---- 概念与实例 ----

ConceptNode& Ontology::add_concept(const std::string& name, Attrs attrs) {
    auto it = concept_by_name_.find(name);
    if (it != concept_by_name_.end()) return concepts_.at(it->second);  // 幂等复用
    std::string id = make_concept_id(name);
    concept_by_name_[name] = id;
    auto [pos, _] = concepts_.emplace(id, ConceptNode{id, name, std::move(attrs)});
    (void)_;
    return pos->second;
}

InstanceNode& Ontology::add_instance(const std::string& name, const std::string& entity_ref,
                                     Attrs attrs) {
    auto it = instance_by_name_.find(name);
    if (it != instance_by_name_.end()) {
        InstanceNode& n = instances_.at(it->second);
        if (!entity_ref.empty() && n.entity_ref.empty()) n.entity_ref = entity_ref;  // 补登记
        return n;
    }
    std::string id = make_instance_id(name);
    instance_by_name_[name] = id;
    auto [pos, _] = instances_.emplace(id, InstanceNode{id, name, entity_ref, std::move(attrs)});
    (void)_;
    return pos->second;
}

const ConceptNode* Ontology::find_concept(const std::string& name) const {
    auto it = concept_by_name_.find(name);
    return it == concept_by_name_.end() ? nullptr : &concepts_.at(it->second);
}

const ConceptNode* Ontology::concept_of(const std::string& concept_id) const {
    auto it = concepts_.find(concept_id);
    return it == concepts_.end() ? nullptr : &it->second;
}

const InstanceNode* Ontology::find_instance(const std::string& name) const {
    auto it = instance_by_name_.find(name);
    return it == instance_by_name_.end() ? nullptr : &instances_.at(it->second);
}

const InstanceNode* Ontology::instance_of(const std::string& instance_id) const {
    auto it = instances_.find(instance_id);
    return it == instances_.end() ? nullptr : &it->second;
}

const ConceptNode* Ontology::concept_by_entity_type(const std::string& type) const {
    for (const auto& [id, c] : concepts_) {
        auto it = c.attrs.find("entity_type");
        if (it != c.attrs.end() && it->second == type) return &c;
    }
    return nullptr;
}

bool Ontology::has_node(const std::string& id) const {
    return concepts_.count(id) > 0 || instances_.count(id) > 0;
}

// ---- 边 ----

static std::string make_edge_key(const std::string& from, const std::string& to, RelType t) {
    return from + "|" + to + "|" + rel_name(t);
}

void Ontology::add_edge(Edge e) {
    std::string key = make_edge_key(e.from, e.to, e.type);
    auto it = edge_key_.find(key);
    if (it != edge_key_.end()) {  // 去重合并
        Edge& cur = edges_[it->second];
        if (e.weight > cur.weight) cur.weight = e.weight;
        cur.attrs.insert(e.attrs.begin(), e.attrs.end());
        return;
    }
    edge_key_[key] = edges_.size();
    out_[e.from].push_back(edges_.size());
    in_[e.to].push_back(edges_.size());
    edges_.push_back(std::move(e));
}

void Ontology::upsert_edge(const std::string& from, const std::string& to, RelType type,
                           double delta) {
    std::string key = make_edge_key(from, to, type);
    auto it = edge_key_.find(key);
    if (it != edge_key_.end()) {
        edges_[it->second].weight += delta;  // 共现一次 weight+1（镜像 ame auto_grow）
        return;
    }
    add_edge({from, to, type, delta, {}});
}

bool Ontology::has_edge(const std::string& from, const std::string& to, RelType type) const {
    return edge_key_.count(make_edge_key(from, to, type)) > 0;
}

std::vector<Edge> Ontology::edges_from(const std::string& node_id) const {
    std::vector<Edge> out;
    auto it = out_.find(node_id);
    if (it != out_.end())
        for (size_t i : it->second) out.push_back(edges_[i]);
    return out;
}

std::vector<Edge> Ontology::edges_to(const std::string& node_id) const {
    std::vector<Edge> out;
    auto it = in_.find(node_id);
    if (it != in_.end())
        for (size_t i : it->second) out.push_back(edges_[i]);
    return out;
}

// ---- 传递闭包 ----

std::vector<std::string> Ontology::bfs_up(const std::string& concept_id, RelType type) const {
    std::vector<std::string> order;
    std::unordered_set<std::string> seen{concept_id};
    std::queue<std::string> q;
    q.push(concept_id);
    while (!q.empty()) {
        std::string cur = q.front();
        q.pop();
        auto it = out_.find(cur);
        if (it == out_.end()) continue;
        for (size_t i : it->second) {
            const Edge& e = edges_[i];
            if (e.type != type || seen.count(e.to)) continue;
            seen.insert(e.to);
            order.push_back(e.to);  // BFS 序：近祖先在前
            q.push(e.to);
        }
    }
    return order;
}

bool Ontology::is_a(const std::string& sub_id, const std::string& super_id) const {
    if (sub_id == super_id) return true;
    for (const auto& a : bfs_up(sub_id, RelType::IS_A))
        if (a == super_id) return true;
    return false;
}

std::vector<std::string> Ontology::ancestors(const std::string& concept_id) const {
    return bfs_up(concept_id, RelType::IS_A);
}

std::vector<std::string> Ontology::part_of_ancestors(const std::string& concept_id) const {
    return bfs_up(concept_id, RelType::PART_OF);
}

std::vector<std::string> Ontology::is_a_path(const std::string& sub_id,
                                             const std::string& super_id) const {
    if (sub_id == super_id) return {sub_id};
    // BFS 带父指针，重建 sub -> ... -> super 的 id 路径
    std::unordered_map<std::string, std::string> parent;
    std::unordered_set<std::string> seen{sub_id};
    std::queue<std::string> q;
    q.push(sub_id);
    while (!q.empty()) {
        std::string cur = q.front();
        q.pop();
        auto it = out_.find(cur);
        if (it == out_.end()) continue;
        for (size_t i : it->second) {
            const Edge& e = edges_[i];
            if (e.type != RelType::IS_A || seen.count(e.to)) continue;
            seen.insert(e.to);
            parent[e.to] = cur;
            if (e.to == super_id) {
                std::vector<std::string> path{super_id};
                for (std::string n = super_id; n != sub_id; n = parent.at(n))
                    path.push_back(parent.at(n));
                std::reverse(path.begin(), path.end());
                return path;
            }
            q.push(e.to);
        }
    }
    return {};
}

std::vector<std::string> Ontology::descendants(const std::string& concept_id) const {
    std::vector<std::string> order;
    std::unordered_set<std::string> seen{concept_id};
    std::queue<std::string> q;
    q.push(concept_id);
    while (!q.empty()) {
        std::string cur = q.front();
        q.pop();
        auto it = in_.find(cur);  // IS_A 的反向：谁指向我
        if (it == in_.end()) continue;
        for (size_t i : it->second) {
            const Edge& e = edges_[i];
            if (e.type != RelType::IS_A || seen.count(e.from)) continue;
            seen.insert(e.from);
            order.push_back(e.from);
            q.push(e.from);
        }
    }
    return order;
}

// ---- 规则库 ----

void Ontology::add_rule(InferenceRule r) {
    for (const auto& cur : rules_)
        if (cur.rule_id == r.rule_id) return;  // 同 id 去重
    rules_.push_back(std::move(r));
}

std::vector<InferenceRule> Ontology::rules_for(const std::string& concept_id) const {
    // 规则沿 is-a 继承：x INSTANCE_OF A、A IS_A* B、规则(B,...) ⇒ 对 x 生效
    std::unordered_set<std::string> scope{concept_id};
    for (const auto& a : ancestors(concept_id)) scope.insert(a);
    std::vector<InferenceRule> out;
    for (const auto& r : rules_)
        if (scope.count(r.antecedent)) out.push_back(r);
    return out;
}

// ---- 版本化资产加载 ----

void Ontology::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw KnowledgeError("ontology: cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) throw KnowledgeError("ontology: parse error in " + path);
    load_json(j);
}

void Ontology::load_json(const json& j) {
    version_ = j.value("version", "");
    // 概念：{"name", "is_a"? , "part_of"? , "attrs"?}（父概念按名引用，可先使用后声明）
    if (j.contains("concepts")) {
        for (const auto& c : j["concepts"]) {
            Attrs attrs;
            if (c.contains("attrs"))
                for (auto& [k, v] : c["attrs"].items())
                    attrs[k] = v.is_string() ? v.get<std::string>() : v.dump();
            add_concept(c.value("name", ""), std::move(attrs));
        }
        for (const auto& c : j["concepts"]) {
            const ConceptNode* self = find_concept(c.value("name", ""));
            if (!self) continue;
            std::string id = self->concept_id;
            if (c.contains("is_a")) {
                const ConceptNode* p = find_concept(c.value("is_a", ""));
                if (!p) throw KnowledgeError("ontology: unknown is-a parent " +
                                             c.value("is_a", ""));
                add_edge({id, p->concept_id, RelType::IS_A, 1.0, {}});
            }
            if (c.contains("part_of")) {
                const ConceptNode* p = find_concept(c.value("part_of", ""));
                if (!p) throw KnowledgeError("ontology: unknown part-of whole " +
                                             c.value("part_of", ""));
                add_edge({id, p->concept_id, RelType::PART_OF, 1.0, {}});
            }
        }
    }
    // 概念间语义边：{"from","to","type","weight"?}
    if (j.contains("relations")) {
        for (const auto& r : j["relations"]) {
            const ConceptNode* f = find_concept(r.value("from", ""));
            const ConceptNode* t = find_concept(r.value("to", ""));
            auto type = rel_from_name(r.value("type", ""));
            if (!f || !t || !type)
                throw KnowledgeError("ontology: bad relation " + r.dump());
            add_edge({f->concept_id, t->concept_id, *type, r.value("weight", 1.0), {}});
        }
    }
    // 规则：{"id","antecedent","relation","consequent","confidence"?,"note"?}
    if (j.contains("rules")) {
        for (const auto& r : j["rules"]) {
            const ConceptNode* a = find_concept(r.value("antecedent", ""));
            const ConceptNode* c = find_concept(r.value("consequent", ""));
            auto rel = rel_from_name(r.value("relation", ""));
            if (!a || !c || !rel)
                throw KnowledgeError("ontology: bad rule " + r.dump());
            add_rule({r.value("id", ""), a->concept_id, *rel, c->concept_id,
                      r.value("confidence", 1.0), r.value("note", "")});
        }
    }
}

// ---- 快照 ----

json Ontology::snapshot() const {
    json j;
    j["version"] = version_;
    j["concepts"] = json::array();
    for (const auto& [id, c] : concepts_)
        j["concepts"].push_back({{"name", c.name}, {"attrs", c.attrs}});
    j["instances"] = json::array();
    for (const auto& [id, n] : instances_)
        j["instances"].push_back(
            {{"name", n.name}, {"entity_ref", n.entity_ref}, {"attrs", n.attrs}});
    j["edges"] = json::array();
    for (const auto& e : edges_)
        j["edges"].push_back({{"from", e.from},
                              {"to", e.to},
                              {"type", rel_name(e.type)},
                              {"weight", e.weight},
                              {"attrs", e.attrs}});
    j["rules"] = json::array();
    for (const auto& r : rules_)
        j["rules"].push_back({{"id", r.rule_id},
                              {"antecedent", r.antecedent},
                              {"relation", rel_name(r.relation)},
                              {"consequent", r.consequent},
                              {"confidence", r.confidence},
                              {"note", r.note}});
    return j;
}

void Ontology::load_snapshot(const json& j) {
    if (j.contains("version")) version_ = j["version"].get<std::string>();
    // id 由名字确定性派生，按名重建即与原图对齐
    for (const auto& c : j.value("concepts", json::array())) {
        Attrs attrs = c.value("attrs", json::object()).get<Attrs>();
        add_concept(c.value("name", ""), std::move(attrs));
    }
    for (const auto& n : j.value("instances", json::array())) {
        Attrs attrs = n.value("attrs", json::object()).get<Attrs>();
        add_instance(n.value("name", ""), n.value("entity_ref", ""), std::move(attrs));
    }
    for (const auto& r : j.value("rules", json::array())) {
        auto rel = rel_from_name(r.value("relation", ""));
        if (rel)
            add_rule({r.value("id", ""), r.value("antecedent", ""), *rel,
                      r.value("consequent", ""), r.value("confidence", 1.0),
                      r.value("note", "")});
    }
    for (const auto& e : j.value("edges", json::array())) {
        auto type = rel_from_name(e.value("type", ""));
        if (!type) continue;
        Attrs attrs = e.value("attrs", json::object()).get<Attrs>();
        add_edge({e.value("from", ""), e.value("to", ""), *type, e.value("weight", 1.0),
                  std::move(attrs)});
    }
}

} // namespace knowledge
