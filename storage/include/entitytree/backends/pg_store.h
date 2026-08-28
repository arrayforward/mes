#pragma once

// 仅在 CMake 找到 libpq（UNISTORE_WITH_PG）时可用。

#ifdef UNISTORE_WITH_PG

#include <string>

#include "entitytree/record_entity_store.h"
#include "storage/backends/pg_backend.h"

namespace entitytree {

/// PostgreSQL 后端的实体搜索树存储：RecordEntityStore + storage::PgBackend。
/// conninfo 为 libpq 标准连接串。
class PgEntityStore : public RecordEntityStore {
public:
    explicit PgEntityStore(const std::string& conninfo)
        : RecordEntityStore(std::make_unique<storage::PgBackend>(conninfo)) {}
};

} // namespace entitytree

#endif // UNISTORE_WITH_PG
