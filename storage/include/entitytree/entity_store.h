#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "entitytree/model.h"

namespace entitytree {

/// 实体搜索树存储抽象接口。
/// 三层模型：观测（append-only 证据）→ 侧写（时空窗口聚合的属性包，镜像永久保留）
/// → 实体（侧写的归并视图，当前态可变的缓存）。
/// append-only 铁律：观测与绑定只有追加；合并/拆分 = 追加新绑定（latest wins）。
/// 侧写与实体是"当前态"（upsert 语义），实体 merged view 是带版本号的缓存。
class EntityStore {
public:
    virtual ~EntityStore() = default;

    // ---- 观测（append-only） ----

    /// 追加观测，由存储层分配自增 seq 并返回。
    virtual int64_t append_observation(const Observation& o) = 0;
    virtual Observation get_observation(const std::string& observation_id) = 0;
    /// 某空间锚点的全部观测，按 seq 升序。
    virtual std::vector<Observation> observations_of(const std::string& anchor_ref) = 0;
    /// 索引查询：三个条件任意组合（std::nullopt = 不限制）。结果按 seq 升序。
    virtual std::vector<Observation> query_observations(
        std::optional<std::string> attribute_key = {},
        std::optional<std::string> source_id = {},
        std::optional<std::string> anchor_ref = {}) = 0;

    // ---- 侧写（upsert 当前态 + 镜像永久保留） ----

    virtual void        upsert_profile(const AttrProfile& p) = 0;
    virtual AttrProfile get_profile(const std::string& profile_id) = 0;
    /// 时空窗口 + 状态 + 层级组合查询（std::nullopt = 不限制）。结果按 seq 升序。
    /// level 过滤时间桶分辨率层级（0=最细；缺省不过滤，兼容旧调用）。
    virtual std::vector<AttrProfile> query_profiles(
        std::optional<std::string> anchor_ref = {},
        std::optional<int64_t> time_bucket_from = {},
        std::optional<int64_t> time_bucket_to = {},
        std::optional<std::string> status = {},
        std::optional<int> level = {}) = 0;
    /// 属性召回：attributes 里含 (key, value) 的全部侧写（走 et_profile_attrs 等值查询）。
    virtual std::vector<AttrProfile> find_profiles_by_attribute(
        const std::string& key, const std::string& value) = 0;

    // ---- 实体（upsert 当前态，merged view 缓存） ----

    virtual void      upsert_entity(const EntityNode& e) = 0;
    virtual EntityNode get_entity(const std::string& entity_id) = 0;
    /// 实体属性召回：merged view 里含 (key, value) 的全部实体（走 et_entity_attrs）。
    virtual std::vector<EntityNode> find_entities_by_attribute(
        const std::string& key, const std::string& value) = 0;

    // ---- 绑定（append-only，latest wins） ----

    /// 追加绑定，由存储层分配 seq 并返回。
    virtual int64_t append_binding(const EntityBinding& b) = 0;
    /// 某侧写的全部绑定历史，按 seq 升序。
    virtual std::vector<EntityBinding> bindings_of(const std::string& profile_id) = 0;
    /// 有效绑定：该侧写 seq 最新的一条（latest wins）。无绑定返回 std::nullopt。
    virtual std::optional<EntityBinding> effective_binding(const std::string& profile_id) = 0;
    /// 实体→侧写反查：当前有效绑定指向该实体的全部侧写，按 seq 升序。
    /// 语义是"有效绑定"而非"历史绑定"：被拆分纠正走的侧写不再返回。
    virtual std::vector<AttrProfile> profiles_of_entity(const std::string& entity_id) = 0;

    // ---- 来源可靠性（et_sources，entitytree 与 stmb 共享） ----

    /// upsert 来源可靠性记录（source_id 主键）。
    virtual void upsert_source(const SourceRecord& s) = 0;
    /// 按 source_id 取记录；未注册返回 std::nullopt（调用方按默认可靠性处理）。
    virtual std::optional<SourceRecord> get_source(const std::string& source_id) = 0;
    /// 全部来源记录，按 source_id 升序。
    virtual std::vector<SourceRecord> list_sources() = 0;
};

} // namespace entitytree
