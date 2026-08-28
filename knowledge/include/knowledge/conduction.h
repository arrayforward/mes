#pragma once

// ============================================================================
// 模块：传导系数表——全库唯一的扩散传导系数来源（镜像 ame core/types）。
// 扩散引擎、推理器都从这里取系数，禁止各处自写字面量。
// ============================================================================

#include <optional>
#include <string>

#include "knowledge/types.h"

namespace knowledge {

/// 各关系类型的默认传导系数（语义强度）。
double conduction_coeff(RelType t);

/// 关系类型 <-> 字符串名（快照序列化用）。
const char* rel_name(RelType t);
std::optional<RelType> rel_from_name(const std::string& s);

/// 双向边判定：相似/对立/共现为双向，其余有向。
bool rel_undirected(RelType t);

} // namespace knowledge
