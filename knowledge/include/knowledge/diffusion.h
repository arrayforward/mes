#pragma once

// ============================================================================
// 模块：加权扩散引擎（diffusion）——镜像 ame M6：从种子节点沿带类型边做
//   加权能量扩散，产出各节点激活能量与可解释 top3 传播路径。
//   边传导 = 传导系数 × 边权；能量按 (hop+1)/(hop+2) 衰减并按出度归一；
//   budget 限流，能量枯竭剪枝。本库无 embedding，图即全部通道。
// ============================================================================

#include <string>
#include <unordered_map>
#include <vector>

#include "knowledge/ontology.h"
#include "knowledge/types.h"

namespace knowledge {

struct DiffusionSeed {
    std::string node_id;
    double weight = 1.0;
};

struct PathStep {
    std::string from, to;
    RelType type;
};

struct Path {
    double energy = 0.0;
    std::vector<PathStep> steps;
};

struct NodeEnergy {
    double energy = 0.0;
    std::vector<Path> paths;  // top3，按能量降序
};

using DiffusionResult = std::unordered_map<std::string, NodeEnergy>;

/// 加权逐层 BFS。depth 硬限 [2,3]；budget 限制已扩展节点总数。
/// 扩散把图当无向加权图处理（联想回忆沿边双向流动），OBSERVED_IN 锚点
/// 指向非图节点自然终止。
DiffusionResult diffuse(const Ontology& ont, const std::vector<DiffusionSeed>& seeds,
                        int depth = 3, int budget = 256);

} // namespace knowledge
