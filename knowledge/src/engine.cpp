// ============================================================================
// 模块：外观 facade 实现——六动词之外的装配全部在此，保持各组件可独立测试。
// ============================================================================

#include "knowledge/engine.h"

#include <fstream>
#include <sstream>

#include "ame/keyword/keyword_manager.h"
#include "ame/storage/storage.h"
#include "knowledge/ame_lexicon.h"

namespace knowledge {

Engine::Engine()
    : ame_storage_(std::make_unique<ame::Storage>()),
      ame_kw_(std::make_unique<ame::KeywordManager>(*ame_storage_)),
      ingester_(ont_, lex_),
      reasoner_(ont_, lex_) {
    ingester_.set_ame_layer(ame_kw_.get());
    reasoner_.set_ame_layer(ame_storage_.get(), ame_kw_.get());
}

Engine::~Engine() = default;

void Engine::define_ontology(const std::string& path) { ont_.load(path); }

int Engine::apply_lexicon(const std::string& path) {
    lex_.load(path);
    load_ame_lexicon(path);  // 同一 schema，种子词典一并灌入 ame 词汇层
    return knowledge::apply_lexicon(ont_, lex_);
}

int Engine::load_ame_lexicon(const std::string& path) {
    if (!ame_lexicon_loaded_.insert(path).second) return 0;  // 幂等：同文件不重复灌
    return knowledge::load_ame_lexicon(*ame_kw_, path);
}

IngestResult Engine::understand(eventstore::EventStore& store, const std::string& profile_id) {
    return ingester_.ingest_profile(store, profile_id);
}

std::vector<IngestResult> Engine::ingest_narrative(eventstore::EventStore& store,
                                                   const std::string& narrative_id) {
    return ingester_.ingest_narrative(store, narrative_id);
}

std::vector<IngestResult> Engine::ingest_all(eventstore::EventStore& store) {
    return ingester_.ingest_all(store);
}

ReasoningChain Engine::infer(const std::string& query) { return reasoner_.infer(query); }

void Engine::save_snapshot(const std::string& path) const {
    std::ofstream f(path);
    if (!f) throw KnowledgeError("engine: cannot write " + path);
    f << ont_.snapshot().dump(2);
}

void Engine::load_snapshot(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw KnowledgeError("engine: cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) throw KnowledgeError("engine: parse error in " + path);
    ont_.load_snapshot(j);
}

} // namespace knowledge
