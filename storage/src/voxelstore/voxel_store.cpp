#include "voxelstore/voxel_store.h"

namespace voxelstore {

std::optional<VoxelVersion> VoxelStore::version_at(uint64_t block_id, int64_t t) {
    for (const auto& v : versions_of(block_id)) {
        if (v.valid_from > t) continue;
        if (v.valid_to && *v.valid_to <= t) continue;
        return v;  // valid_from <= t < valid_to（valid_to 空 = 当前生效）
    }
    return std::nullopt;
}

} // namespace voxelstore
