#pragma once

#include <string>

#include "storage/backends/redis_backend.h"
#include "voxelstore/record_voxel_store.h"

namespace voxelstore {

/// Redis 后端的时空块存储：RecordVoxelStore + storage::RedisBackend。
class RedisVoxelStore : public RecordVoxelStore {
public:
    explicit RedisVoxelStore(const std::string& addr, std::string key_prefix = "ust")
        : RecordVoxelStore(
              std::make_unique<storage::RedisBackend>(addr, std::move(key_prefix))) {}
};

} // namespace voxelstore
