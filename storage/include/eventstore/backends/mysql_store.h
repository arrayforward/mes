#pragma once

// 仅在 CMake 找到 libmysqlclient（UNISTORE_WITH_MYSQL）时可用。

#ifdef UNISTORE_WITH_MYSQL

#include <string>

#include "eventstore/record_event_store.h"
#include "storage/backends/mysql_backend.h"

namespace eventstore {

/// MySQL/MariaDB 后端的事件树存储：RecordEventStore + storage::MySqlBackend。
/// dsn 形式："mysql://user:password@host:port/dbname"。
class MySqlStore : public RecordEventStore {
public:
    explicit MySqlStore(const std::string& dsn)
        : RecordEventStore(std::make_unique<storage::MySqlBackend>(dsn)) {}
};

} // namespace eventstore

#endif // UNISTORE_WITH_MYSQL
