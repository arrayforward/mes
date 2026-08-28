#pragma once

// ============================================================================
// 模块：符号推理器（reasoner）——镜像 ame "软→硬→软"管道：
//   种子（ame 词汇层归一 + 硬命中 + 近义降级）→ 符号扩展（is-a 闭包 + 规则
//   前向链，限深 3，确定性可审计）→ 扩散（软：本体图扩散 + ame 关键词图
//   RELATED 扩散）→ 融合输出 ReasoningChain。
// ============================================================================

#include <string>
#include <utility>
#include <vector>

#include "knowledge/diffusion.h"
#include "knowledge/lexicon.h"
#include "knowledge/ontology.h"
#include "knowledge/types.h"

namespace ame {
class Storage;
class KeywordManager;
}

namespace knowledge {

/// 推理链：一次 infer() 的完整可解释产出。
struct ReasoningChain {
    std::string query;
    std::vector<std::pair<std::string, double>> seeds;  // 命中种子（节点 id + 权重）
    std::vector<Inference> inferences;                  // 规则命中（含可解释路径）
    DiffusionResult activated;                          // 本体图扩散激活（节点 id -> 能量+top3路径）
    /// ame 软通道：关键词图 RELATED 扩散的激活词（词名 + 能量，降序，无 ame 层时为空）。
    std::vector<std::pair<std::string, double>> ame_activated;
    double confidence = 0.0;                            // 综合置信度
};

class Reasoner {
public:
    Reasoner(Ontology& ont, const Lexicon& lex) : ont_(ont), lex_(lex) {}

    /// 接入 ame 词汇层（可空）：查询词归一/近义降级走真实词典，并追加 ame
    /// 扩散软通道（RELATED 边传播）。
    void set_ame_layer(ame::Storage* storage, ame::KeywordManager* kw) {
        ame_storage_ = storage;
        ame_kw_ = kw;
    }

    ReasoningChain infer(const std::string& query, int depth = 3);

private:
    Ontology& ont_;
    const Lexicon& lex_;
    ame::Storage* ame_storage_ = nullptr;
    ame::KeywordManager* ame_kw_ = nullptr;
};

} // namespace knowledge
