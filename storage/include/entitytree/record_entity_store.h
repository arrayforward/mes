#pragma once

#include <memory>

#include "entitytree/entity_store.h"
#include "storage/record_backend.h"

namespace entitytree {

/// EntityStore 的统一层实现：把实体搜索树模型（观测/侧写/实体/绑定）
/// 映射到 6 张 Record 表，跑在任意 storage::RecordBackend 之上。
/// 后端专用子类（MemoryEntityStore/SqlEntityStore/...）只需注入对应 backend。
class RecordEntityStore : public EntityStore {
public:
    explicit RecordEntityStore(std::unique_ptr<storage::RecordBackend> backend);

    int64_t append_observation(const Observation& o) override;
    Observation get_observation(const std::string& observation_id) override;
    std::vector<Observation> observations_of(const std::string& anchor_ref) override;
    std::vector<Observation> query_observations(
        std::optional<std::string> attribute_key = {},
        std::optional<std::string> source_id = {},
        std::optional<std::string> anchor_ref = {}) override;

    void        upsert_profile(const AttrProfile& p) override;
    AttrProfile get_profile(const std::string& profile_id) override;
    std::vector<AttrProfile> query_profiles(
        std::optional<std::string> anchor_ref = {},
        std::optional<int64_t> time_bucket_from = {},
        std::optional<int64_t> time_bucket_to = {},
        std::optional<std::string> status = {},
        std::optional<int> level = {}) override;
    std::vector<AttrProfile> find_profiles_by_attribute(
        const std::string& key, const std::string& value) override;

    void       upsert_entity(const EntityNode& e) override;
    EntityNode get_entity(const std::string& entity_id) override;
    std::vector<EntityNode> find_entities_by_attribute(
        const std::string& key, const std::string& value) override;

    int64_t append_binding(const EntityBinding& b) override;
    std::vector<EntityBinding> bindings_of(const std::string& profile_id) override;
    std::optional<EntityBinding> effective_binding(const std::string& profile_id) override;
    std::vector<AttrProfile> profiles_of_entity(const std::string& entity_id) override;

    void upsert_source(const SourceRecord& s) override;
    std::optional<SourceRecord> get_source(const std::string& source_id) override;
    std::vector<SourceRecord> list_sources() override;

protected:
    storage::RecordBackend& backend() { return *backend_; }

private:
    std::unique_ptr<storage::RecordBackend> backend_;
};

} // namespace entitytree
