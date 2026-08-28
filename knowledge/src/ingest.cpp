// ============================================================================
// 模块：侧写摄入实现——八步管道（镜像 ame "写入即生长"，事实源在 storage）：
//   1. 读 Profile + effective_binding（subject/object/place → entity_id）；
//   2. 五要素词典归一化（别名表 O(1)）；
//   3. surface → InstanceNode（已存在则复用），挂 INSTANCE_OF 概念
//      （按名命中，否则按 Entity.type 映射的初始类型概念）；
//      事件本身作为动词概念的实例（事件 INSTANCE_OF 动作概念）；
//   4. 事件类型判定：沿动词概念的 IS_A 链上溯 → 事件分类；
//   5. 锚点回写：概念/实例挂 OBSERVED_IN {profile_id, event_id} 边；
//   6. 自动生长：同侧写五要素节点两两 COOCCUR upsert；共现≥3 结晶提示；
//   7. Link.causes/before 同步为事件实例级 CAUSES/BEFORE 边（sync_links）；
//   8. 规则前向链：沿 is-a 继承匹配规则，产出 Inference 候选，
//      置信度 = 规则置信度 × 绑定/匹配置信度。
// ============================================================================

#include "knowledge/ingest.h"

#include <algorithm>
#include <unordered_set>

#include "ame/keyword/keyword_manager.h"
#include "eventstore/event_store.h"
#include "knowledge/ame_lexicon.h"

namespace knowledge {

void mark_conflicts(std::vector<Inference>& inferences) {
    for (size_t i = 0; i < inferences.size(); ++i) {
        for (size_t j = i + 1; j < inferences.size(); ++j) {
            const Inference& a = inferences[i];
            const Inference& b = inferences[j];
            if (a.subject != b.subject || a.candidate != b.candidate) continue;
            bool a_block = a.relation == RelType::PREVENTS;
            bool b_block = b.relation == RelType::PREVENTS;
            if (a_block != b_block) {  // 一推一抑 ⇒ 矛盾标注（供审计，不删除）
                inferences[i].conflict = true;
                inferences[j].conflict = true;
            }
        }
    }
}

namespace {

// 绑定置信度：无绑定视为 1.0（确定性 surface）。
double binding_conf(eventstore::EventStore& store, const std::string& profile_id,
                    const std::string& slot) {
    auto b = store.effective_binding(profile_id, slot);
    return b ? b->confidence : 1.0;
}

std::string binding_entity(eventstore::EventStore& store, const std::string& profile_id,
                           const std::string& slot) {
    auto b = store.effective_binding(profile_id, slot);
    return b ? b->entity_id : "";
}

} // namespace

IngestResult Ingester::ingest_profile(eventstore::EventStore& store,
                                      const std::string& profile_id) {
    if (auto it = results_.find(profile_id); it != results_.end()) return it->second;

    IngestResult res;
    res.profile_id = profile_id;

    // 步骤1：读 Profile + 有效绑定
    const eventstore::Profile p = store.get_profile(profile_id);

    // 步骤2：五要素保留原始 surface（实例命名的基准）；词典归一只作匹配扩展——
    //   真实大词典（中文词林）的同义群较松，争议词归一噪声大（如 勇者→硬汉），
    //   故实例名用原词，概念匹配时遍历词形集合（原词/ame 归一/种子归一）。
    const std::string& subj = p.subject;
    const std::string& verb = p.verb;
    const std::string& obj = p.object;
    const std::string& place = p.place;

    // 步骤2（续）：ame 侧生长——五要素各词形落 Keyword 节点，近义词补
    //   REL_SIMILAR 边并唤醒（故事词汇作为活跃词汇参与 ame 扩散软通道）。
    if (ame_) {
        for (const std::string& raw : {subj, verb, obj, place}) {
            if (raw.empty()) continue;
            for (const std::string& w : surface_forms(ame_, lex_, raw)) {
                ame_->get_or_create(w, "element");
                for (const auto& [nw, score] : ame_->near_words(w, 8)) {
                    ame_->get_or_create(nw, "near");
                    if (ame_->link_related(w, nw, ame::RelType::REL_SIMILAR, score) ==
                        ame::Err::Ok) {
                        ame_->wake_keyword(w);
                        ame_->wake_keyword(nw);
                    }
                }
            }
        }
    }

    // 步骤3：事件实例（五要素合成）+ 主客体实例
    std::string ev_name = subj + verb + obj;
    Attrs ev_attrs{{"event_id", p.event_id},
                   {"profile_id", p.profile_id},
                   {"perspective", p.perspective},
                   {"time", p.time.value}};
    InstanceNode& ev = ont_.add_instance(ev_name, p.event_id, std::move(ev_attrs));
    res.event_instance_id = ev.instance_id;
    event_node_of_[profile_id] = ev.instance_id;
    res.instance_ids.push_back(ev.instance_id);

    auto make_slot_instance = [&](const std::string& surface, const std::string& slot,
                                  double& conf_out) -> std::string {
        if (surface.empty()) return "";
        conf_out = binding_conf(store, profile_id, slot);
        InstanceNode& n = ont_.add_instance(surface, binding_entity(store, profile_id, slot));
        // 挂类型概念：各词形按名命中，否则按 storage Entity.type 映射
        const ConceptNode* type = nullptr;
        for (const auto& form : surface_forms(ame_, lex_, surface)) {
            type = ont_.find_concept(form);
            if (type) break;
        }
        if (!type && !n.entity_ref.empty()) {
            const eventstore::Entity e = store.get_entity(n.entity_ref);
            type = ont_.concept_by_entity_type(e.type);
        }
        if (type && !ont_.has_edge(n.instance_id, type->concept_id, RelType::INSTANCE_OF))
            ont_.add_edge({n.instance_id, type->concept_id, RelType::INSTANCE_OF, conf_out, {}});
        res.instance_ids.push_back(n.instance_id);
        return n.instance_id;
    };
    double subj_conf = 1.0, obj_conf = 1.0, place_conf = 1.0;
    const std::string subj_id = make_slot_instance(subj, "subject", subj_conf);
    const std::string obj_id = make_slot_instance(obj, "object", obj_conf);
    const std::string place_id = make_slot_instance(place, "place", place_conf);

    // 动词概念解析：各词形精确命中优先，再按近义词降级扩展（×score；ame 层 ∪ 种子词典）
    const ConceptNode* vc = nullptr;
    for (const auto& form : surface_forms(ame_, lex_, verb)) {
        vc = ont_.find_concept(form);
        if (vc) break;
    }
    res.verb_match = vc ? 1.0 : 0.0;
    if (!vc) {
        double best = 0.0;
        for (const auto& [w, score] : near_candidates(ame_, lex_, verb)) {
            for (const auto& form : surface_forms(ame_, lex_, w)) {
                const ConceptNode* c = ont_.find_concept(form);
                if (c && score > best) {
                    vc = c;
                    best = score;
                    break;
                }
            }
        }
        res.verb_match = best;
    }
    if (vc) {
        res.verb_concept_id = vc->concept_id;
        res.instance_ids.push_back(vc->concept_id);
        // 事件 INSTANCE_OF 动作概念（规则沿 is-a 继承的统一入口）
        if (!ont_.has_edge(ev.instance_id, vc->concept_id, RelType::INSTANCE_OF))
            ont_.add_edge({ev.instance_id, vc->concept_id, RelType::INSTANCE_OF,
                           res.verb_match, {}});
    }

    // 步骤4：事件类型判定——沿动词概念的 IS_A 链上溯
    if (vc) {
        res.chain.push_back(vc->name);
        std::string top;
        for (const auto& a : ont_.ancestors(vc->concept_id)) {
            const ConceptNode* c = ont_.concept_of(a);
            if (!c) continue;
            res.chain.push_back(c->name);
            if (ont_.ancestors(a).empty()) {
                // 顶类 = 根的直接子类（链上根的前一个）
                res.event_class = top.empty() ? c->name : top;
            } else {
                top = c->name;
            }
        }
        if (res.event_class.empty()) res.event_class = vc->name;  // 动词概念自身即根
    }

    // 步骤5：锚点回写——OBSERVED_IN {profile_id, event_id}，可审计回指 storage
    Attrs anchor{{"event_id", p.event_id}};
    ont_.add_edge({ev.instance_id, profile_id, RelType::OBSERVED_IN, 1.0, anchor});
    if (vc) ont_.add_edge({vc->concept_id, profile_id, RelType::OBSERVED_IN, 1.0, anchor});
    for (const std::string& id : {subj_id, obj_id, place_id})
        if (!id.empty()) ont_.add_edge({id, profile_id, RelType::OBSERVED_IN, 1.0, anchor});

    // 步骤6：自动生长——同侧写五要素节点两两 COOCCUR upsert；共现≥3 结晶提示
    std::vector<std::string> participants;
    for (const std::string& id : {subj_id, obj_id, place_id})
        if (!id.empty()) participants.push_back(id);
    if (vc) participants.push_back(vc->concept_id);
    for (size_t i = 0; i < participants.size(); ++i) {
        for (size_t j = i + 1; j < participants.size(); ++j) {
            std::string a = participants[i], b = participants[j];
            if (b < a) std::swap(a, b);  // 无向共现：规范方向去重
            ont_.upsert_edge(a, b, RelType::COOCCUR, 1.0);
            for (const auto& e : ont_.edges_from(a))
                if (e.type == RelType::COOCCUR && e.to == b && e.weight >= 3.0 &&
                    e.attrs.count("crystallized") == 0) {
                    ont_.add_edge({a, b, RelType::COOCCUR, e.weight,
                                   {{"crystallized", "true"}}});  // 结晶提示（镜像 ame）
                }
        }
    }

    // 步骤8：规则前向链（事件级：动词概念沿 is-a 继承的规则）
    if (vc) {
        for (const auto& rule : ont_.rules_for(vc->concept_id)) {
            const ConceptNode* cons = ont_.concept_of(rule.consequent);
            if (!cons) continue;
            Inference inf;
            inf.rule_id = rule.rule_id;
            inf.subject = ev.name;
            inf.candidate = cons->name;
            inf.relation = rule.relation;
            inf.confidence = rule.confidence * res.verb_match;
            inf.path.push_back(ev.name);
            for (const auto& id : ont_.is_a_path(vc->concept_id, rule.antecedent)) {
                const ConceptNode* c = ont_.concept_of(id);
                if (!c) continue;
                if (inf.path.size() > 1) inf.path.push_back("IS_A");
                inf.path.push_back(c->name);
            }
            inf.path.push_back(rel_name(rule.relation));
            inf.path.push_back(cons->name);
            res.inferences.push_back(std::move(inf));
        }
    }
    // 步骤8（续）：实例级规则——x INSTANCE_OF A、A IS_A* B、规则(B,...) ⇒ x 可能…
    auto instance_rules = [&](const std::string& inst_id) {
        if (inst_id.empty()) return;
        const InstanceNode* n = ont_.instance_of(inst_id);
        for (const auto& e : ont_.edges_from(inst_id)) {
            if (e.type != RelType::INSTANCE_OF) continue;
            for (const auto& rule : ont_.rules_for(e.to)) {
                const ConceptNode* cons = ont_.concept_of(rule.consequent);
                if (!cons || !n) continue;
                Inference inf;
                inf.rule_id = rule.rule_id;
                inf.subject = n->name;
                inf.candidate = cons->name;
                inf.relation = rule.relation;
                inf.confidence = rule.confidence * e.weight;  // × 绑定置信度（边权）
                inf.path = {n->name, "INSTANCE_OF"};
                for (const auto& id : ont_.is_a_path(e.to, rule.antecedent)) {
                    const ConceptNode* c = ont_.concept_of(id);
                    if (!c) continue;
                    if (inf.path.size() > 2) inf.path.push_back("IS_A");
                    inf.path.push_back(c->name);
                }
                inf.path.push_back(rel_name(rule.relation));
                inf.path.push_back(cons->name);
                res.inferences.push_back(std::move(inf));
            }
        }
    };
    instance_rules(subj_id);
    instance_rules(obj_id);
    mark_conflicts(res.inferences);

    // 步骤7：同步本侧写出发的 Link（目标已摄入才建边；批量摄入后还有统一兜底）
    sync_links(store, profile_id);

    results_[profile_id] = res;
    return res;
}

void Ingester::sync_links(eventstore::EventStore& store, const std::string& profile_id) {
    auto from_it = event_node_of_.find(profile_id);
    if (from_it == event_node_of_.end()) return;
    for (const auto& l : store.links_from(profile_id)) {
        auto to_it = event_node_of_.find(l.to_profile_id);
        if (to_it == event_node_of_.end()) continue;  // 目标未摄入，跳过（批量后兜底）
        if (l.relation == "causes")
            ont_.upsert_edge(from_it->second, to_it->second, RelType::CAUSES, 1.0);
        else if (l.relation == "before")
            ont_.upsert_edge(from_it->second, to_it->second, RelType::BEFORE, 1.0);
    }
}

std::vector<IngestResult> Ingester::ingest_all(eventstore::EventStore& store) {
    std::vector<IngestResult> out;
    std::vector<std::string> ids;
    for (const auto& p : store.query_profiles()) {  // 无过滤 = 全部侧写（seq 升序）
        out.push_back(ingest_profile(store, p.profile_id));
        ids.push_back(p.profile_id);
    }
    for (const auto& id : ids) sync_links(store, id);  // 兜底：前向 Link 此刻目标已齐
    return out;
}

std::vector<IngestResult> Ingester::ingest_narrative(eventstore::EventStore& store,
                                                     const std::string& narrative_id) {
    std::vector<IngestResult> out;
    std::vector<std::string> ids;
    for (const auto& p : store.timeline(narrative_id)) {  // before/causes 拓扑序
        out.push_back(ingest_profile(store, p.profile_id));
        ids.push_back(p.profile_id);
    }
    for (const auto& id : ids) sync_links(store, id);
    return out;
}

} // namespace knowledge
