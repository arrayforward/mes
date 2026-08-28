#pragma once

// 仅在 CMake 找到 libmysqlclient（UNISTORE_WITH_MYSQL）时可用。

#ifdef UNISTORE_WITH_MYSQL

#include <string>

#include "entitytree/record_entity_store.h"
#include "storage/backends/mysql_backend.h"

namespace entitytree {

/// MySQL/MariaDB 后端的实体搜索树存储：RecordEntityStore + storage::MySqlBackend。
/// dsn 形式："mysql://user:password@host:port/dbname"。
class MySqlEntityStore : public RecordEntityStore {
public:
    explicit MySqlEntityStore(const std::string& dsn)
        : RecordEntityStore(std::make_unique<storage::MySqlBackend>(dsn)) {}
};

} // namespace entitytree

#endif // UNISTORE_WITH_MYSQL
