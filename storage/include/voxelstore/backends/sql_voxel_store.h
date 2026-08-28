#pragma once

#include <string>

#include "storage/backends/sqlite_backend.h"
#include "voxelstore/record_voxel_store.h"

namespace voxelstore {

/// SQLite 后端的时空块存储：统一层 RecordVoxelStore + storage::SqliteBackend。
class SqlVoxelStore : public RecordVoxelStore {
public:
    /// path 为 SQLite 文件路径；":memory:" 表示纯内存库。
    explicit SqlVoxelStore(const std::string& path)
        : RecordVoxelStore(std::make_unique<storage::SqliteBackend>(path)) {}
};

} // namespace voxelstore
