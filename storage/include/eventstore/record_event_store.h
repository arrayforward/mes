#pragma once

#include <memory>

#include "eventstore/event_store.h"
#include "storage/record_backend.h"

namespace eventstore {

/// EventStore 的统一层实现：把事件树模型（叙事/事件/侧写/边/实体/绑定）
/// 映射到 Record 表，跑在任意 storage::RecordBackend 之上。
/// 后端专用子类（MemoryStore/SqlStore/...）只需注入对应 backend。
class RecordEventStore : public EventStore {
public:
    explicit RecordEventStore(std::unique_ptr<storage::RecordBackend> backend);

    void    append_narrative(const Narrative& n) override;
    void    append_event(const Event& e) override;
    int64_t append_profile(const Profile& p) override;
    void    append_link(const Link& l) override;

    Narrative get_narrative(const std::string& id) override;
    Event     get_event(const std::string& id) override;
    Profile   get_profile(const std::string& id) override;

    std::vector<Event>   events_of(const std::string& narrative_id) override;
    std::vector<Profile> profiles_of(const std::string& event_id) override;
    std::vector<Link>    links_from(const std::string& profile_id) override;
    std::vector<Link>    links_to(const std::string& profile_id) override;
    std::vector<Profile> query_profiles(std::optional<std::string> subject = {},
                                        std::optional<std::string> verb = {},
                                        std::optional<std::string> perspective = {},
                                        std::optional<std::string> place = {},
                                        std::optional<std::string> object = {}) override;
    std::vector<Profile> find_profiles_by_modifier(
        std::optional<std::string> text = {},
        std::optional<std::string> kind = {},
        std::optional<std::string> target = {}) override;

    void   append_entity(const Entity& e) override;
    Entity get_entity(const std::string& id) override;
    std::vector<Entity> find_entities_by_name(const std::string& name) override;

    int64_t append_binding(const Binding& b) override;
    std::vector<Binding> bindings_of(const std::string& profile_id) override;
    std::vector<Binding> bindings_for(const std::string& profile_id,
                                      const std::string& slot) override;
    std::optional<Binding> effective_binding(const std::string& profile_id,
                                             const std::string& slot) override;
    std::vector<Profile> profiles_of_entity(const std::string& entity_id,
                                            std::optional<std::string> slot = {}) override;

protected:
    storage::RecordBackend& backend() { return *backend_; }

private:
    std::unique_ptr<storage::RecordBackend> backend_;
};

} // namespace eventstore
