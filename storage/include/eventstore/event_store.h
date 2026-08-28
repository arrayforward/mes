#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "eventstore/model.h"

namespace eventstore {

/// 事件树存储抽象接口（append-only）。
/// 接口只提供 append_*，不提供 update/delete：
/// 纠正 = 追加带 refines/retracts 关系的新侧写；重消歧 = 追加新 Binding。
class EventStore {
public:
    virtual ~EventStore() = default;

    // ---- 叙事 / 事件 / 侧写 / 先后边 ----

    virtual void    append_narrative(const Narrative& n) = 0;
    virtual void    append_event(const Event& e) = 0;
    /// 追加侧写，由存储层分配全局自增 seq 并返回。
    virtual int64_t append_profile(const Profile& p) = 0;
    virtual void    append_link(const Link& l) = 0;

    virtual Narrative get_narrative(const std::string& id) = 0;
    virtual Event     get_event(const std::string& id) = 0;
    virtual Profile   get_profile(const std::string& id) = 0;

    virtual std::vector<Event>   events_of(const std::string& narrative_id) = 0;
    virtual std::vector<Profile> profiles_of(const std::string& event_id) = 0;
    virtual std::vector<Link>    links_from(const std::string& profile_id) = 0;
    virtual std::vector<Link>    links_to(const std::string& profile_id) = 0;

    /// 索引查询（按 surface 字符串），五个条件可任意组合（std::nullopt = 不限制）。
    /// 结果按 seq 升序。
    virtual std::vector<Profile> query_profiles(
        std::optional<std::string> subject = {},
        std::optional<std::string> verb = {},
        std::optional<std::string> perspective = {},
        std::optional<std::string> place = {},
        std::optional<std::string> object = {}) = 0;

    /// 按修饰语查询：条件须落在同一修饰语上（如 text="锋利" 且 kind="quality"）。
    /// 三个条件可任意组合（std::nullopt = 不限制）。结果按 seq 升序。
    virtual std::vector<Profile> find_profiles_by_modifier(
        std::optional<std::string> text = {},
        std::optional<std::string> kind = {},
        std::optional<std::string> target = {}) = 0;

    /// 叙事时间线：取叙事下全部侧写，按 before/causes 边做拓扑排序（seq 兜底）。
    /// refines/retracts 边不参与排序。检测到环时抛 EventStoreError。
    std::vector<Profile> timeline(const std::string& narrative_id);

    // ---- 实体与指代消解（人名问题的专门设计） ----

    virtual void   append_entity(const Entity& e) = 0;
    virtual Entity get_entity(const std::string& id) = 0;
    /// 按规范名或别名检索实体。重名全部返回（同名不同实体 id 不同）。
    virtual std::vector<Entity> find_entities_by_name(const std::string& name) = 0;

    /// 追加指代消解记录，由存储层分配 seq 并返回。
    virtual int64_t append_binding(const Binding& b) = 0;
    /// 某侧写的全部消解历史（所有槽位），按 seq 升序。
    virtual std::vector<Binding> bindings_of(const std::string& profile_id) = 0;
    /// 某侧写某槽位的全部消解历史，按 seq 升序。
    virtual std::vector<Binding> bindings_for(const std::string& profile_id,
                                              const std::string& slot) = 0;
    /// 有效指代：该槽位 seq 最新的 Binding（latest wins）。无绑定返回 std::nullopt。
    virtual std::optional<Binding> effective_binding(const std::string& profile_id,
                                                     const std::string& slot) = 0;

    /// 实体→侧写反查：当前有效绑定指向该实体的全部侧写，按 seq 升序。
    /// 语义是"有效绑定"而非"历史绑定"：被重消歧纠正走的侧写不再返回。
    /// slot 可限定只查某个槽位（std::nullopt = 任意槽位）。
    virtual std::vector<Profile> profiles_of_entity(
        const std::string& entity_id, std::optional<std::string> slot = {}) = 0;
};

} // namespace eventstore
