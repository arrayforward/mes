// ============================================================================
// 模块：符号推理器实现——"软→硬→软"管道：
//   步骤1（种子）：查询词词典归一 → 硬种子（概念/实例精确命中，权重 1.0）
//                 + 近义降级扩展（×0.4×score）；
//   步骤2（符号扩展，硬）：概念种子沿 is-a 闭包做规则前向链（限深 3），
//        结论概念入队继续触发——确定性、可审计；实例种子先沿 INSTANCE_OF
//        找到类型概念再入链；
//   步骤3（扩散，软）：从种子沿带类型边加权 BFS（见 diffusion）；
//   步骤4（融合）：规则命中 + 图能量汇成 ReasoningChain，标注矛盾。
// ============================================================================

#include "knowledge/reasoner.h"

#include <algorithm>
#include <queue>
#include <unordered_set>

#include "ame/diffusion/diffusion_engine.h"
#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"
#include "knowledge/ame_lexicon.h"
#include "knowledge/ingest.h"  // mark_conflicts

namespace knowledge {

ReasoningChain Reasoner::infer(const std::string& query, int depth) {
    if (depth < 1) depth = 1;
    if (depth > 3) depth = 3;

    ReasoningChain chain;
    chain.query = query;

    // 步骤1：种子——遍历查询词的词形集合（原词/ame 归一/种子归一）做精确命中，
    //   再按近义词降级扩展（×0.4×score）。实例命名保留原始 surface，故原词优先。
    std::vector<DiffusionSeed> dseeds;
    std::unordered_set<std::string> seed_ids;
    auto add_seed = [&](const std::string& id, double w) {
        if (seed_ids.count(id)) return;
        seed_ids.insert(id);
        chain.seeds.push_back({id, w});
        dseeds.push_back({id, w});
    };
    for (const auto& form : surface_forms(ame_kw_, lex_, query)) {
        if (const ConceptNode* c = ont_.find_concept(form)) add_seed(c->concept_id, 1.0);
        if (const InstanceNode* n = ont_.find_instance(form)) add_seed(n->instance_id, 1.0);
    }
    for (const auto& [w, score] : near_candidates(ame_kw_, lex_, query)) {  // 近义降级扩展
        for (const auto& form : surface_forms(ame_kw_, lex_, w)) {
            if (const ConceptNode* c = ont_.find_concept(form))
                add_seed(c->concept_id, 0.4 * score);
            if (const InstanceNode* n = ont_.find_instance(form))
                add_seed(n->instance_id, 0.4 * score);
        }
    }
    if (dseeds.empty()) return chain;  // 无种子：查询词不在图谱中

    // 步骤2：符号扩展——规则前向链（限深，结论概念继续触发）
    struct Fact {
        std::string concept_id;         // 已成立的概念
        std::string subject;            // 推理主体（查询词/实例名）
        double weight;                  // 到达该概念的置信度
        std::vector<std::string> path;  // 到达路径（人读步骤）
        int depth;
    };
    std::queue<Fact> q;
    std::unordered_set<std::string> fired;  // concept_id 已触发过规则（防环）
    auto enqueue_concept = [&](const std::string& cid, const std::string& subject, double w,
                               std::vector<std::string> path, int d) {
        if (fired.count(cid)) return;
        fired.insert(cid);
        q.push({cid, subject, w, std::move(path), d});
    };
    for (const auto& [id, w] : chain.seeds) {
        if (const ConceptNode* c = ont_.concept_of(id)) {
            enqueue_concept(id, c->name, w, {c->name}, 0);
        } else if (const InstanceNode* n = ont_.instance_of(id)) {
            for (const auto& e : ont_.edges_from(id))  // 实例 → 类型概念入链
                if (e.type == RelType::INSTANCE_OF)
                    if (const ConceptNode* c = ont_.concept_of(e.to))
                        enqueue_concept(e.to, n->name, w * e.weight,
                                        {n->name, "INSTANCE_OF", c->name}, 0);
        }
    }
    while (!q.empty()) {
        Fact f = q.front();
        q.pop();
        if (f.depth >= depth) continue;
        for (const auto& rule : ont_.rules_for(f.concept_id)) {  // 含 is-a 祖先继承
            const ConceptNode* cons = ont_.concept_of(rule.consequent);
            if (!cons) continue;
            Inference inf;
            inf.rule_id = rule.rule_id;
            inf.subject = f.subject;
            inf.candidate = cons->name;
            inf.relation = rule.relation;
            inf.confidence = rule.confidence * f.weight;
            inf.path = f.path;
            for (const auto& id : ont_.is_a_path(f.concept_id, rule.antecedent)) {
                const ConceptNode* c = ont_.concept_of(id);
                if (!c || c->name == inf.path.back()) continue;
                inf.path.push_back("IS_A");
                inf.path.push_back(c->name);
            }
            inf.path.push_back(rel_name(rule.relation));
            inf.path.push_back(cons->name);
            chain.inferences.push_back(inf);
            // 结论概念成为新事实，继续前向链（置信度沿规则衰减）
            enqueue_concept(rule.consequent, f.subject, inf.confidence, inf.path, f.depth + 1);
        }
    }
    mark_conflicts(chain.inferences);

    // 步骤3：扩散（软）
    chain.activated = diffuse(ont_, dseeds, depth);

    // 步骤3（续）：ame 软通道——查询词（任一词形）的关键词节点存在时，沿 ame
    //   关键词图的 RELATED 边做真实扩散（镜像 ame M6），激活词作为软证据附在链上。
    if (ame_storage_ && ame_kw_) {
        // 各词形的关键词节点都作种子：别名归一是双向的（袭击↔袭取互为别名），
        // 单一词形可能落在休眠/无连边的节点上
        std::vector<ame::DiffusionSeed> ame_seeds;
        std::unordered_set<std::string> ame_seed_uids;
        for (const auto& form : surface_forms(ame_kw_, lex_, query)) {
            const ame::Node* kn = ame_kw_->find(form);
            if (kn && ame_seed_uids.insert(kn->uid).second)
                ame_seeds.push_back({kn->uid, 1.0});
        }
        if (!ame_seeds.empty()) {
            ame::DiffusionEngine ame_diff(*ame_storage_, ame_kw_);
            auto res = ame_diff.diffuse(ame_seeds, /*depth=*/2, nullptr, nullptr,
                                        /*budget=*/64);
            for (const auto& [uid, ne] : res) {
                if (ame_seed_uids.count(uid)) continue;
                const ame::Node* node = ame_storage_->get_node(uid);
                if (node && node->kind == ame::NodeKind::Keyword)
                    chain.ame_activated.push_back({node->name, ne.energy});
            }
            std::sort(chain.ame_activated.begin(), chain.ame_activated.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            if (chain.ame_activated.size() > 8) chain.ame_activated.resize(8);
        }
    }

    // 步骤4：融合置信度——有规则命中取最高命中置信度，否则按种子权重打折
    double conf = 0.0;
    for (const auto& inf : chain.inferences)
        if (inf.confidence > conf) conf = inf.confidence;
    if (conf == 0.0)
        for (const auto& [id, w] : chain.seeds)
            if (w * 0.5 > conf) conf = w * 0.5;
    chain.confidence = conf;
    return chain;
}

} // namespace knowledge
