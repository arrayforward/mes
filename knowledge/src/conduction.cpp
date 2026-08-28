// ============================================================================
// 模块：传导系数表实现（镜像 ame core/src/types.cpp）。
// 本文件主体思路：switch 查表返回扩散传导系数；rel_from_name 用枚举区间
//   线性反查；rel_undirected 判定双向边。
// ============================================================================

#include "knowledge/conduction.h"

namespace knowledge {

// 系数语义：本体层级与因果/触发为强传导（0.85~0.95），组成/前置次之，
//   相似/时序中等，共现偏弱，对立/抑制/锚点最低档（防反义与证据边被当强联想）。
double conduction_coeff(RelType t) {
    switch (t) {
        case RelType::IS_A:        return 0.95;  // 本体层级（最强结构边）
        case RelType::PART_OF:     return 0.8;   // 组成
        case RelType::CAUSES:      return 0.9;   // 因果
        case RelType::TRIGGERS:    return 0.85;  // 触发（规则推理主通道）
        case RelType::REQUIRES:    return 0.8;   // 前置条件
        case RelType::PREVENTS:    return 0.4;   // 抑制
        case RelType::SIMILAR:     return 0.7;   // 相似
        case RelType::OPPOSITE:    return 0.5;   // 对立（单列，防反义被当强联想）
        case RelType::COOCCUR:     return 0.6;   // 共现
        case RelType::BEFORE:      return 0.7;   // 时序（镜像 ame NEXT）
        case RelType::INSTANCE_OF: return 0.9;   // 实例 -> 概念
        case RelType::OBSERVED_IN: return 0.5;   // 侧写锚点（证据回指，弱传导）
    }
    return 0.5;  // 理论兜底（switch 已穷尽）
}

const char* rel_name(RelType t) {
    switch (t) {
        case RelType::IS_A:        return "IS_A";
        case RelType::PART_OF:     return "PART_OF";
        case RelType::CAUSES:      return "CAUSES";
        case RelType::TRIGGERS:    return "TRIGGERS";
        case RelType::REQUIRES:    return "REQUIRES";
        case RelType::PREVENTS:    return "PREVENTS";
        case RelType::SIMILAR:     return "SIMILAR";
        case RelType::OPPOSITE:    return "OPPOSITE";
        case RelType::COOCCUR:     return "COOCCUR";
        case RelType::BEFORE:      return "BEFORE";
        case RelType::INSTANCE_OF: return "INSTANCE_OF";
        case RelType::OBSERVED_IN: return "OBSERVED_IN";
    }
    return "UNKNOWN";
}

std::optional<RelType> rel_from_name(const std::string& s) {
    for (int i = 0; i <= (int)RelType::OBSERVED_IN; ++i) {
        RelType t = (RelType)i;
        if (s == rel_name(t)) return t;
    }
    return std::nullopt;
}

bool rel_undirected(RelType t) {
    switch (t) {
        case RelType::SIMILAR:
        case RelType::OPPOSITE:
        case RelType::COOCCUR:
            return true;
        default:
            return false;
    }
}

} // namespace knowledge
