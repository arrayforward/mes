#pragma once

#include <memory>

#include "storage/record_backend.h"
#include "voxelstore/voxel_store.h"

namespace voxelstore {

/// VoxelStore 的统一层实现：把时空块模型（块当前态 / 版本链 / 动态实例）
/// 映射到 Record 表，跑在任意 storage::RecordBackend 之上。
class RecordVoxelStore : public VoxelStore {
public:
    explicit RecordVoxelStore(std::unique_ptr<storage::RecordBackend> backend);

    void put_block(const VoxelBlock& b) override;
    std::optional<VoxelBlock> get_block(uint64_t id) override;
    bool remove_block(uint64_t id) override;
    std::vector<VoxelBlock> all_blocks() override;
    std::vector<VoxelBlock> query_blocks(std::optional<Aabb> region,
                                         std::optional<int64_t> time_from,
                                         std::optional<int64_t> time_to,
                                         std::optional<int> level) override;

    void append_version(const VoxelVersion& v) override;
    void seal_version(uint64_t block_id, int64_t valid_to) override;
    std::vector<VoxelVersion> versions_of(uint64_t block_id) override;

    void put_instance(const VoxelInstance& i) override;
    std::optional<VoxelInstance> get_instance(uint64_t id) override;
    std::vector<VoxelInstance> all_instances() override;

    void set_meta(const std::string& key, int64_t value) override;
    int64_t get_meta(const std::string& key, int64_t default_value) override;

private:
    std::unique_ptr<storage::RecordBackend> backend_;
};

} // namespace voxelstore
