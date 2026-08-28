#pragma once

#include <string>

#include "storage/backends/file_backend.h"
#include "voxelstore/record_voxel_store.h"

namespace voxelstore {

/// 文件目录后端的时空块存储：RecordVoxelStore + storage::FileBackend。
class FileVoxelStore : public RecordVoxelStore {
public:
    explicit FileVoxelStore(const std::string& root)
        : RecordVoxelStore(std::make_unique<storage::FileBackend>(root)) {}
};

} // namespace voxelstore
