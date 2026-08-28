#pragma once

// 仅在 CMake 找到 libpq（UNISTORE_WITH_PG）时可用。

#ifdef UNISTORE_WITH_PG

#include <string>

#include "eventstore/record_event_store.h"
#include "storage/backends/pg_backend.h"

namespace eventstore {

/// PostgreSQL 后端的事件树存储：RecordEventStore + storage::PgBackend。
/// conninfo 为 libpq 标准连接串。
class PgStore : public RecordEventStore {
public:
    explicit PgStore(const std::string& conninfo)
        : RecordEventStore(std::make_unique<storage::PgBackend>(conninfo)) {}
};

} // namespace eventstore

#endif // UNISTORE_WITH_PG
