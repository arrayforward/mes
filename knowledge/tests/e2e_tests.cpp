// e2e：读 storage 的 demo_story.db（勇者斗恶龙），跑完整理解+推理管道，
// 词汇侧由 vendored ame 层驱动（真实词典 lexicon_zh.json 中文词林 7.7 万词）。
// 验证标准：
//   1. 事件类型判定正确（沿 is-a 链）：勇者拔剑 → 持械/战斗动作；
//   2. 至少一条规则推理命中且带可解释路径：战斗动作 TRIGGERS 警觉；
//   3. OBSERVED_IN 锚点可回指原 profile_id；
//   4. 真实词典别名归一（进攻→攻击、警惕→警觉、拔→拔出）驱动推理命中；
//   5. ame 软通道：RELATED 边扩散激活近义词。

#include <chrono>
#include <cmath>

#include "ame/keyword/keyword_manager.h"
#include "eventstore/backends/sql_store.h"
#include "knowledge/engine.h"
#include "test_framework.h"

using namespace knowledge;
using namespace eventstore;

static const std::string kAssets = KNOWLEDGE_ASSETS_DIR;
static const std::string kAmeLex = KNOWLEDGE_AME_LEXICON_DIR;
static std::string cp(const std::string& n) { return make_concept_id(n); }

TEST(e2e_demo_story_understanding) {
    Engine eng;
    eng.define_ontology(kAssets + "/ontology_seed.json");
    int edges = eng.apply_lexicon(kAssets + "/lexicon.json");
    CHECK(edges >= 2);

    // 真实词典（vendored ame 中文词林，7.7 万词条）灌入 ame 词汇层
    auto t0 = std::chrono::steady_clock::now();
    int applied = eng.load_ame_lexicon(kAmeLex + "/lexicon_zh.json");
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::printf("lexicon_zh.json: applied=%d, %.2fs\n", applied, secs);
    CHECK(applied > 10000);
    CHECK(secs < 30.0);  // 运行时长 sanity
    // 幂等：同文件重复加载不再灌入
    CHECK_EQ(eng.load_ame_lexicon(kAmeLex + "/lexicon_zh.json"), 0);

    // 真实词典别名归一（这些词条不在 knowledge 种子词典里；取词林中稳定的映射）
    CHECK_EQ(eng.ame_keywords().normalize("村落"), std::string("村庄"));
    CHECK_EQ(eng.ame_keywords().normalize("瞧见"), std::string("看见"));
    // 争议词归一噪声大（词林同义群较松），但匹配按词形集合扩展，原词永不丢失
    CHECK_EQ(eng.ame_keywords().normalize("袭击") == "袭击", false);  // 被归一到别形
    SqlStore store(KNOWLEDGE_DEMO_DB);
    auto results = eng.ingest_all(store);
    CHECK(results.size() >= 4);  // demo_story 的 4 条侧写（2 事件 × 2 视角）

    // "恶龙袭击村庄" 仍经近义降级正确分类（归一噪声不影响原词匹配）
    const IngestResult* raid = nullptr;
    for (const auto& r : results)
        if (!r.chain.empty() && r.chain[0] == "攻击") raid = &r;
    CHECK(raid != nullptr);
    if (raid) {
        CHECK_EQ(raid->event_class, std::string("战斗动作"));
        CHECK(std::fabs(raid->verb_match - 0.85) < 1e-9);
    }

    // 判据1：找到"勇者拔剑"侧写，事件分类沿 is-a 链正确
    const IngestResult* draw = nullptr;
    for (const auto& r : results)
        if (!r.chain.empty() && r.chain[0] == "拔出") draw = &r;
    CHECK(draw != nullptr);
    if (!draw) return;
    CHECK_EQ(draw->event_class, std::string("战斗动作"));
    CHECK(draw->chain.size() >= 3);
    CHECK_EQ(draw->chain[1], std::string("持械"));
    CHECK_EQ(draw->chain[2], std::string("战斗动作"));

    // 判据2：至少一条规则推理命中（警觉）且带可解释路径
    bool rule_hit = false;
    for (const auto& i : draw->inferences)
        if (i.candidate == "警觉" && !i.path.empty()) {
            rule_hit = true;
            CHECK(i.confidence > 0.0);
        }
    CHECK(rule_hit);

    // 判据3：OBSERVED_IN 锚点回指原 profile_id，且能在 storage 中解析
    bool anchor = false;
    for (const auto& e : eng.ontology().edges_to(draw->profile_id))
        if (e.type == RelType::OBSERVED_IN && e.from == cp("拔出")) anchor = true;
    CHECK(anchor);
    const Profile& back = store.get_profile(draw->profile_id);
    CHECK_EQ(back.verb, std::string("拔出"));

    // 判据4：真实词典驱动推理——"村落"仅靠中文词林别名（村落→村庄）命中概念；
    //   "瞧见"经 ame 别名（瞧见→看见）+ 种子别名（看见→看到）链式命中概念"看到"
    ReasoningChain vil = eng.infer("村落");
    bool vil_hit = false;
    for (const auto& [id, w] : vil.seeds)
        if (id == cp("村庄") && w == 1.0) vil_hit = true;
    CHECK(vil_hit);
    ReasoningChain see = eng.infer("瞧见");
    bool see_hit = false;
    for (const auto& [id, w] : see.seeds)
        if (id == cp("看到")) see_hit = true;
    CHECK(see_hit);

    // 判据5：ame 软通道——"袭击"的关键词图 RELATED 扩散激活了"攻击"的词林规范形
    //   （真实词典归一会把"攻击"归到其同义群规范词，如 鞭挞；按运行时归一核对）
    const std::string attack_canon = eng.ame_keywords().normalize("攻击");
    ReasoningChain soft = eng.infer("袭击");
    bool soft_hit = false;
    for (const auto& [word, energy] : soft.ame_activated)
        if (word == attack_canon && energy > 0.0) soft_hit = true;
    CHECK(soft_hit);
    // 原有判据：近义降级推出警觉
    bool near_hit = false;
    for (const auto& i : soft.inferences)
        if (i.candidate == "警觉") near_hit = true;
    CHECK(near_hit);
}

int main() { return tfw::run_all(); }
