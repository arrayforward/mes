#pragma once

#include "storage/backends/memory_backend.h"
#include "voxelstore/record_voxel_store.h"

namespace voxelstore {

/// 内存后端的时空块存储：统一层 RecordVoxelStore + storage::MemoryBackend。
class MemoryVoxelStore : public RecordVoxelStore {
public:
    MemoryVoxelStore() : RecordVoxelStore(std::make_unique<storage::MemoryBackend>()) {}
};

} // namespace voxelstore
