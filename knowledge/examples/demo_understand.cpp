// 演示：读 storage 的 demo_story.db（勇者斗恶龙），用语义图谱树做事件理解与推理。
// 词汇侧由 vendored ame 层驱动：真实词典 lexicon_zh.json（中文词林 7.7 万词）提供
// 别名归一与近义扩展，ame 关键词图提供 RELATED 扩散软通道。
// 依次验证：事件分类（is-a 链）、规则推理（可解释路径）、OBSERVED_IN 锚点回指、
// 真实词典别名归一、ame 近义扩散。

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"
#include "eventstore/backends/sql_store.h"
#include "knowledge/engine.h"

// windows.h 须在 ame 头文件之后包含：其 IN/NEAR 宏与 ame RelType 枚举成员冲突。
#ifdef _WIN32
#include <windows.h>
#endif

using namespace knowledge;
using namespace eventstore;

static void print_path(const std::vector<std::string>& path) {
    for (size_t i = 0; i < path.size(); ++i)
        std::printf("%s%s", i ? " → " : "", path[i].c_str());
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const std::string db = argc > 1 ? argv[1] : KNOWLEDGE_DEMO_DB;
    const std::string assets = KNOWLEDGE_ASSETS_DIR;
    const std::string ame_lex = KNOWLEDGE_AME_LEXICON_DIR;

    Engine eng;
    eng.define_ontology(assets + "/ontology_seed.json");
    int lex_edges = eng.apply_lexicon(assets + "/lexicon.json");
    std::printf("本体版本 %s，种子词典灌库建边 %d 条\n", eng.ontology().version().c_str(),
                lex_edges);

    // 真实词典灌入 ame 词汇层（vendored 中文词林，7.7 万词条）
    auto t0 = std::chrono::steady_clock::now();
    int applied = eng.load_ame_lexicon(ame_lex + "/lexicon_zh.json");
    auto t1 = std::chrono::steady_clock::now();
    std::printf("ame 词汇层：lexicon_zh.json 灌入 %d 条（%.2fs）\n\n", applied,
                std::chrono::duration<double>(t1 - t0).count());

    std::printf("=== 真实词典别名归一（中文词林）===\n");
    // 稳定映射（匹配扩展有效）与争议映射（同义群较松）都如实展示——
    // 摄入/推理按词形集合匹配，争议归一不会丢失原词。
    for (const char* w : {"村落", "瞧见", "袭击", "勇者"})
        std::printf("  %s → %s\n", w, eng.ame_keywords().normalize(w).c_str());
    std::printf("\n");

    SqlStore store(db);
    auto results = eng.ingest_all(store);
    std::printf("=== 事件理解（%zu 条侧写）===\n", results.size());
    for (const auto& r : results) {
        const Profile& p = store.get_profile(r.profile_id);
        std::printf("\n[%s] %s %s %s @ %s\n", p.perspective.c_str(), p.subject.c_str(),
                    p.verb.c_str(), p.object.c_str(), p.place.c_str());
        if (r.chain.empty()) {
            std::printf("  分类：动词「%s」未命中本体概念\n", p.verb.c_str());
        } else {
            std::printf("  分类链：");
            print_path(r.chain);
            std::printf("\n  事件类型：%s（动词匹配权重 %.2f）\n", r.event_class.c_str(),
                        r.verb_match);
        }
        for (const auto& i : r.inferences) {
            std::printf("  推理：%s 可能%s「%s」（置信度 %.2f，规则 %s%s）\n  路径：",
                        i.subject.c_str(), i.relation == RelType::PREVENTS ? "被抑制" : "",
                        i.candidate.c_str(), i.confidence, i.rule_id.c_str(),
                        i.conflict ? "，矛盾!" : "");
            print_path(i.path);
            std::printf("\n");
        }
    }

    std::printf("\n=== 推理查询 ===\n");
    for (const char* q : {"袭击", "抽出", "村落", "瞧见"}) {
        ReasoningChain c = eng.infer(q);
        std::printf("\n查询「%s」（综合置信度 %.2f）：\n", q, c.confidence);
        for (const auto& [id, w] : c.seeds)
            std::printf("  种子：%s（权重 %.2f）\n", id.c_str(), w);
        for (const auto& i : c.inferences) {
            std::printf("  推出：%s（%.2f）\n  路径：", i.candidate.c_str(), i.confidence);
            print_path(i.path);
            std::printf("\n");
        }
        // 本体图扩散激活 top5
        std::vector<std::pair<std::string, double>> act;
        for (const auto& [id, ne] : c.activated) act.push_back({id, ne.energy});
        std::sort(act.begin(), act.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("  本体扩散 top5：");
        for (size_t i = 0; i < act.size() && i < 5; ++i)
            std::printf("%s(%.3f) ", act[i].first.c_str(), act[i].second);
        // ame 关键词图 RELATED 扩散（软通道）
        std::printf("\n  ame  RELATED 扩散：");
        for (const auto& [w, e] : c.ame_activated) std::printf("%s(%.3f) ", w.c_str(), e);
        std::printf("\n");
    }

    std::printf("\n=== 验证标准 ===\n");
    int pass = 0;
    const IngestResult* draw = nullptr;
    for (const auto& r : results)
        if (!r.chain.empty() && r.chain[0] == "拔出") draw = &r;
    bool c1 = draw && draw->event_class == "战斗动作";
    std::printf("[%s] 1. 事件分类沿 is-a 链正确（勇者拔剑 → 持械/战斗动作）\n",
                c1 ? "PASS" : "FAIL");
    pass += c1;
    bool c2 = false;
    if (draw)
        for (const auto& i : draw->inferences)
            if (i.candidate == "警觉" && !i.path.empty()) c2 = true;
    std::printf("[%s] 2. 至少一条规则推理命中且带可解释路径\n", c2 ? "PASS" : "FAIL");
    pass += c2;
    bool c3 = false;
    if (draw)
        for (const auto& e : eng.ontology().edges_to(draw->profile_id))
            if (e.type == RelType::OBSERVED_IN) c3 = true;
    std::printf("[%s] 3. OBSERVED_IN 锚点回指原 profile_id\n", c3 ? "PASS" : "FAIL");
    pass += c3;
    // 真实词典驱动：村落（仅中文词林别名 村落→村庄）命中概念；瞧见经 ame+种子
    // 别名链（瞧见→看见→看到）命中概念
    bool c4 = false, c4b = false;
    for (const auto& [id, w] : eng.infer("村落").seeds)
        if (id == "cp-村庄") c4 = true;
    for (const auto& [id, w] : eng.infer("瞧见").seeds)
        if (id == "cp-看到") c4b = true;
    std::printf("[%s] 4. 真实词典别名归一驱动概念命中（村落→村庄、瞧见→看到）\n",
                (c4 && c4b) ? "PASS" : "FAIL");
    pass += (c4 && c4b);
    // ame 软通道：袭击 ⇒ "攻击"的词林规范形（真实词典归一噪声如实呈现）
    const std::string attack_canon = eng.ame_keywords().normalize("攻击");
    bool c5 = false;
    for (const auto& [w, e] : eng.infer("袭击").ame_activated)
        if (w == attack_canon && e > 0.0) c5 = true;
    std::printf("[%s] 5. ame 软通道 RELATED 扩散激活近义词（袭击 ⇒ %s）\n",
                c5 ? "PASS" : "FAIL", attack_canon.c_str());
    pass += c5;

    eng.save_snapshot("knowledge_snapshot.json");
    std::printf("\n快照已存 knowledge_snapshot.json（本体图 %zu 条边，ame 关键词图 %zu 节点）\n",
                eng.ontology().edge_count(), eng.ame_storage().node_count());
    return pass == 5 ? 0 : 1;
}
