#pragma once

// ============================================================================
// 模块：外观 facade（engine）——语义图谱树的统一入口：
//   define_ontology / apply_lexicon / load_ame_lexicon / ingest_narrative /
//   understand / infer / snapshot + load。
// 引擎同时持有两层：
//   - 本体层（knowledge::Ontology）：is-a 层级、规则、实例、OBSERVED_IN 锚点；
//   - 词汇-联想层（vendored ame::Storage + ame::KeywordManager）：真实词典
//     驱动的别名归一、近义词表、RELATED 边与扩散软通道。
// ============================================================================

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "knowledge/ingest.h"
#include "knowledge/lexicon.h"
#include "knowledge/ontology.h"
#include "knowledge/reasoner.h"

namespace eventstore {
class EventStore;
}

namespace ame {
class Storage;
class KeywordManager;
}

namespace knowledge {

using nlohmann::json;

class Engine {
public:
    Engine();
    ~Engine();  // 成员单位为不完整类型的 unique_ptr，析构在 .cpp 定义

    Ontology& ontology() { return ont_; }
    const Ontology& ontology() const { return ont_; }
    Lexicon& lexicon() { return lex_; }

    /// ame 词汇层访问（内置别名表始终可用；真实词典经 load_ame_lexicon 灌入）。
    ame::KeywordManager& ame_keywords() { return *ame_kw_; }
    const ame::KeywordManager& ame_keywords() const { return *ame_kw_; }
    ame::Storage& ame_storage() { return *ame_storage_; }

    /// 加载版本化本体资产（概念/is-a/语义边/规则）。运行时只读，变更需升版本。
    void define_ontology(const std::string& path);
    /// 加载种子词典：本体侧三分流建边 + 同文件灌入 ame 词汇层。返回本体侧建边数。
    int apply_lexicon(const std::string& path);
    /// 把词典灌入 ame 词汇层（三分流；幂等，多文件并集）。返回灌入条目数。
    int load_ame_lexicon(const std::string& path);

    /// 理解一条侧写：摄入（幂等）并返回分类链 + 规则推理 + 锚点。
    IngestResult understand(eventstore::EventStore& store, const std::string& profile_id);
    /// 摄入某叙事全部侧写（timeline 拓扑序）。
    std::vector<IngestResult> ingest_narrative(eventstore::EventStore& store,
                                               const std::string& narrative_id);
    /// 摄入 store 内全部侧写。
    std::vector<IngestResult> ingest_all(eventstore::EventStore& store);

    /// 推理查询：种子 → 符号扩展 → 扩散 → 融合 ReasoningChain。
    ReasoningChain infer(const std::string& query);

    /// 运行时图 JSON 快照存/取（崩溃可重建；事实源在 storage）。
    json snapshot() const { return ont_.snapshot(); }
    void load_snapshot(const json& j) { ont_.load_snapshot(j); }
    void save_snapshot(const std::string& path) const;
    void load_snapshot(const std::string& path);

private:
    Ontology ont_;
    Lexicon lex_;
    std::unique_ptr<ame::Storage> ame_storage_;           // 词汇-联想层图存储
    std::unique_ptr<ame::KeywordManager> ame_kw_;         // 归一/近义/RELATED 边入口
    std::unordered_set<std::string> ame_lexicon_loaded_;  // load_ame_lexicon 幂等
    Ingester ingester_;
    Reasoner reasoner_;
};

} // namespace knowledge
