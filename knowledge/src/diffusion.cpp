// ============================================================================
// 模块：加权扩散引擎实现（镜像 ame M6 diffusion_engine.cpp 的逐层波前扩展）。
// 伪代码：
//   步骤1：深度钳位 [2,3]；初始化结果表、当前波前、已扩展集合。
//   步骤2：装入种子——节点不存在则跳过；能量累加并生成单点路径，hop=0 入波前。
//   步骤3：逐层扩散，直到波前为空：
//     3.1 波前节点 hop 达上限跳过；已扩展数达 budget 整层停止；同节点只扩展一次；
//     3.2 遍历邻居（图按无向处理：出边 + 入边反向）：
//         trans = 传导系数 × 边权（weight>5 按 5+log1p(w-5) 软化高频边）；
//         child_e = 能量 × trans × (hop+1)/(hop+2) / 出度；<0.01 枯竭剪枝；
//         路径追加本步边，累加进邻居能量，top3 截断；同层去重入下一层波前。
//   步骤4：返回 node_id -> 能量 + top3 路径。
// ============================================================================

#include "knowledge/diffusion.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "knowledge/conduction.h"

namespace knowledge {

namespace {

// 邻居：(边, 邻居id, 是否反向)。图按无向加权处理（联想回忆沿边双向流动）。
struct Neighbor {
    Edge edge;
    std::string nb_id;
};

std::vector<Neighbor> neighbors_of(const Ontology& ont, const std::string& id) {
    std::vector<Neighbor> out;
    for (const auto& e : ont.edges_from(id))
        if (ont.has_node(e.to)) out.push_back({e, e.to});
    for (const auto& e : ont.edges_to(id))
        if (ont.has_node(e.from)) out.push_back({e, e.from});
    return out;
}

} // namespace

DiffusionResult diffuse(const Ontology& ont, const std::vector<DiffusionSeed>& seeds,
                        int depth, int budget) {
    if (depth < 2) depth = 2;  // 深度硬限 2~3（镜像 ame）
    if (depth > 3) depth = 3;

    DiffusionResult result;
    struct Frontier {
        std::string uid;
        double energy;
        int hop;
        Path path;
    };
    std::vector<Frontier> cur;
    std::unordered_set<std::string> expanded;
    int expanded_count = 0;

    for (const auto& s : seeds) {
        if (!ont.has_node(s.node_id)) continue;
        auto& ne = result[s.node_id];
        ne.energy += s.weight;
        Path p;
        p.energy = s.weight;
        ne.paths.push_back(p);
        cur.push_back({s.node_id, s.weight, 0, p});
    }

    while (!cur.empty()) {
        std::vector<Frontier> next;
        for (const auto& f : cur) {
            if (f.hop >= depth) continue;
            if (expanded_count >= budget) break;  // budget 限制扩展节点数
            if (expanded.count(f.uid)) continue;
            expanded.insert(f.uid);
            ++expanded_count;

            auto nbs = neighbors_of(ont, f.uid);
            const double deg = (double)nbs.size();  // PageRank 式出度归一，防枢纽喷灌
            if (deg == 0.0) continue;
            for (const auto& nb : nbs) {
                double w = nb.edge.weight;
                if (w > 5.0) w = 5.0 + std::log1p(w - 5.0);  // 高频边软化
                const double trans = conduction_coeff(nb.edge.type) * w;
                const double child_e =
                    f.energy * trans * (double)(f.hop + 1) / (double)(f.hop + 2) / deg;
                if (child_e < 0.01) continue;  // 能量枯竭剪枝

                Path p = f.path;
                p.steps.push_back({f.uid, nb.nb_id, nb.edge.type});
                p.energy = child_e;

                auto& ne = result[nb.nb_id];
                bool seen = false;  // 同一节点同一 hop 内避免重复入队
                for (const auto& x : next)
                    if (x.uid == nb.nb_id) {
                        seen = true;
                        break;
                    }
                ne.energy += child_e;
                ne.paths.push_back(p);
                std::sort(ne.paths.begin(), ne.paths.end(),
                          [](const Path& a, const Path& b) { return a.energy > b.energy; });
                if (ne.paths.size() > 3) ne.paths.resize(3);  // 每节点保留 top3 路径
                if (!seen && f.hop + 1 <= depth)
                    next.push_back({nb.nb_id, child_e, f.hop + 1, p});
            }
        }
        cur = std::move(next);
    }
    return result;
}

} // namespace knowledge
