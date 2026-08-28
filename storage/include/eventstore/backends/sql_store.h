#pragma once

#include <string>

#include "eventstore/record_event_store.h"
#include "storage/backends/sqlite_backend.h"

namespace eventstore {

/// SQLite 后端的事件树存储：统一层 RecordEventStore + storage::SqliteBackend。
/// MySQL/Postgres 支持在统一层新增对应 RecordBackend 后自动获得。
class SqlStore : public RecordEventStore {
public:
    /// path 为 SQLite 文件路径；":memory:" 表示纯内存库。
    explicit SqlStore(const std::string& path)
        : RecordEventStore(std::make_unique<storage::SqliteBackend>(path)) {}
};

} // namespace eventstore
