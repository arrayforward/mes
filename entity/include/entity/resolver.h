#pragma once

// entity 组件：实体搜索树的算法与变化逻辑层。
// 职责划分：本组件 = 算法（浮现/合并/拆分/检索排序/时效衰减/来源可靠性闭环）；
// storage（entitytree）= 增删改查（存储落地 + 检索原语）；
// geocore = 空间几何（锚点 AnchorPath 层级邻近）。

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "entitytree/entity_store.h"

namespace entity {

using entitytree::json;

/// 解析引擎参数：全部阈值/权重/半衰期/来源奖惩可配置，
/// 默认值保持"不衰减、不共享来源、单级时间桶、不校验轨迹"的朴素行为（向后兼容）。
struct ResolverConfig {
    int64_t bucket_size = 300;        // 时间桶粒度（秒）；bucket_sizes 为空时的唯一层级
    std::vector<int64_t> bucket_sizes; // 多分辨率时间桶梯子（level 0 最细，
                                      // 如 {300, 3600, 86400}）；空 = 单级（用 bucket_size）
    double  theta_form = 3.0;         // 浮现阈值（信息量）
    double  theta_merge = 0.75;       // 合并阈值
    double  theta_split = 0.30;       // 新建阈值；两者之间为存疑区
    int     min_lca_depth = 0;        // LCA 邻近阈值：两锚点（AnchorPath 路径码）
                                      // 最近公共祖先深度 ≥ 此值即算邻近；0=只要求同根
    std::map<std::string, double> attribute_weight; // 属性区分度权重，缺省 1.0
    double  alpha = 0.7;              // 检索排序：匹配度权重
    double  beta  = 0.3;              // 检索排序：可信度权重

    // ---- 时效衰减（stmb 纪律：查询返回有效值，存储基准值不改写） ----
    double  credibility_half_life = 0;              // 实体可信度半衰期（秒），0=不衰减
    std::map<std::string, double> attribute_half_life; // 按 attribute_key 的属性
                                                       // 半衰期（秒），缺省 0=不衰减

    // ---- 来源可靠性（entitytree ↔ stmb 共享 et_sources） ----
    double  source_reward  = 0.02;    // merge 成功：涉及来源 reliability 上调幅度（封顶 1.0）
    double  source_penalty = 0.05;    // split 发生：被拆侧写来源 reliability 下调幅度（封底 0.05）

    // ---- 轨迹连续性校验（vmax 物理可达，默认关闭） ----
    double      max_speed = 0;        // 物理可达最大速度（m/s），0=关闭校验
    std::string trajectory_check = "veto";   // "veto"=违反的候选直接排除 /
                                             // "penalty"=score 乘 trajectory_penalty
    double      trajectory_penalty = 0.5;    // penalty 模式的压分因子（0~1）
};

/// 检索命中：final = alpha*match_score + beta*credibility（均为查询时有效值）。
struct SearchHit {
    std::string entity_id;
    double      match_score = 0.0;
    double      credibility = 0.0;   // 有效可信度（未配半衰期时 = 基准值）
    double      final_score = 0.0;
    json        explanation = json::array(); // 每个命中属性的贡献明细
};

/// 实体解析引擎：观测 → 侧写聚合 → 浮现 → 合并/新建/存疑 → 拆分 → 检索。
/// 纯逻辑层：只依赖 entitytree::EntityStore 接口与 geocore 路径码纯函数，
/// 任意存储后端行为一致。
class Resolver {
public:
    Resolver(entitytree::EntityStore& store, ResolverConfig config = {});

    const ResolverConfig& config() const { return cfg_; }

    /// 摄入一条观测：按 (anchor_ref, timestamp/bucket_size) 找到或创建侧写，
    /// 追加属性条目、重算 info_score；达 θ_form 且仍在 pool → 置 emerged 并触发 resolve。
    /// 返回所属侧写 profile_id。
    std::string ingest(const entitytree::Observation& obs);

    /// 解析一个侧写：blocking 召回候选实体 → 打分 →
    /// ≥θ_merge 合并 / ≤θ_split 新建实体 / 中间候选实体标 disputed。
    void resolve(const std::string& profile_id);

    /// 拆分：把指定侧写从当前实体解绑（追加指向新实体的 split 绑定），
    /// 双实体各自重建 merged view 与 credibility。返回新实体 entity_id。
    std::string split(const std::string& profile_id, const std::string& new_entity_name = "");

    /// 检索：多路属性召回取并集 → 加权 match_score → final 排序取 topk，
    /// 过滤有效 credibility < min_credibility。
    /// now > 0 时启用时效语义：排序用有效可信度，命中属性乘新鲜度因子；
    /// now = 0（默认）行为与不加衰减时完全一致。
    std::vector<SearchHit> search(
        const std::vector<std::pair<std::string, std::string>>& query_attrs,
        size_t topk, double min_credibility = 0.0, int64_t now = 0);

    /// 查询时有效可信度：base × 0.5^((now−updated_at)/half_life)。
    /// half_life ≤ 0 或 now ≤ updated_at 时原样返回基准值（存储基准值不改写）。
    double effective_credibility(const std::string& entity_id, int64_t now);

    /// 重建某实体的 merged view（聚合全部有效侧写的属性，值按有效置信度加权）
    /// 并重算 credibility 基准值与 updated_at（= 最新观测时间）。view_version +1。
    void rebuild_entity(const std::string& entity_id);

    /// 上卷（多分辨率时间桶）：把某实体在 level-1 层的全部已链接侧写按
    /// level 层粗桶分组，每组聚合出一个粗桶侧写（属性并集含 ts/source、
    /// info_score 重算、status=linked、level 记录在侧写上），追加指向同实体
    /// 的 binding（kind="merge"，note 标 rollup）。细桶侧写原样保留
    /// （镜像不销毁）。返回新建的粗桶侧写 id 列表；单级配置或 level 越界返回空。
    /// 调用时机由上层维护任务决定（如每小时/每天跑一次）。
    std::vector<std::string> rollup(const std::string& entity_id, int level);

    /// 距离提供者：两锚点在同一参考系下的直线距离（米）；
    /// 返回 std::nullopt 表示无距离信息（轨迹校验跳过，不是判负）。
    using DistanceProvider =
        std::function<std::optional<double>(const std::string& anchor_a,
                                            const std::string& anchor_b)>;
    /// 设置距离提供者（默认空 = 跳过轨迹校验）。max_speed=0 时无需设置。
    void set_distance_provider(DistanceProvider provider);

private:
    double weight_of(const std::string& key) const;
    double reliability_of(const std::string& source_id) const;
    int64_t bucket_span(int level) const;      // 某层时间桶长度（秒）
    int     level_count() const;               // 时间桶层级数（≥1）
    double compute_info_score(const entitytree::AttrProfile& p) const;
    double score_candidate(const entitytree::AttrProfile& p,
                           const entitytree::EntityNode& e);
    void   adjust_sources(const entitytree::AttrProfile& p, double delta,
                          int64_t now);

    entitytree::EntityStore& store_;
    ResolverConfig           cfg_;
    DistanceProvider         distance_provider_;  // 可空 = 跳过轨迹校验
};

} // namespace entity
