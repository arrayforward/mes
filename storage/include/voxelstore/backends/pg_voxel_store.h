#pragma once

// 仅在 CMake 找到 libpq（UNISTORE_WITH_PG）时可用。

#ifdef UNISTORE_WITH_PG

#include <string>

#include "storage/backends/pg_backend.h"
#include "voxelstore/record_voxel_store.h"

namespace voxelstore {

/// PostgreSQL 后端的时空块存储。
class PgVoxelStore : public RecordVoxelStore {
public:
    explicit PgVoxelStore(const std::string& conninfo)
        : RecordVoxelStore(std::make_unique<storage::PgBackend>(conninfo)) {}
};

} // namespace voxelstore

#endif // UNISTORE_WITH_PG
