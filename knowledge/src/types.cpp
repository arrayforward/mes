// ============================================================================
// 模块：knowledge 公共类型层实现。当前仅有 id 派生辅助；类型本体见 types.h。
// 关键设计：概念/实例 id 由名字确定性派生（"cp-战斗动作"），而非随机——
//   同一种子资产多次加载 id 稳定，运行时快照因此可跨进程移植恢复。
// ============================================================================

#include "knowledge/types.h"

namespace knowledge {

// 由名字派生确定性 id（保留 storage 的前缀 id 约定：cp- 概念、in- 实例）。
std::string make_concept_id(const std::string& name) { return "cp-" + name; }
std::string make_instance_id(const std::string& name) { return "in-" + name; }

} // namespace knowledge
