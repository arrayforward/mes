#pragma once

// 仅在 CMake 找到 libmysqlclient（UNISTORE_WITH_MYSQL）时可用。

#ifdef UNISTORE_WITH_MYSQL

#include <string>

#include "storage/backends/mysql_backend.h"
#include "voxelstore/record_voxel_store.h"

namespace voxelstore {

/// MySQL/MariaDB 后端的时空块存储。
class MySqlVoxelStore : public RecordVoxelStore {
public:
    explicit MySqlVoxelStore(const std::string& dsn)
        : RecordVoxelStore(std::make_unique<storage::MySqlBackend>(dsn)) {}
};

} // namespace voxelstore

#endif // UNISTORE_WITH_MYSQL
