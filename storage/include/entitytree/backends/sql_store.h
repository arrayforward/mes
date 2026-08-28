#pragma once

#include <string>

#include "entitytree/record_entity_store.h"
#include "storage/backends/sqlite_backend.h"

namespace entitytree {

/// SQLite 后端的实体搜索树存储：统一层 RecordEntityStore + storage::SqliteBackend。
/// MySQL/Postgres 支持在统一层新增对应 RecordBackend 后自动获得。
class SqlEntityStore : public RecordEntityStore {
public:
    /// path 为 SQLite 文件路径；":memory:" 表示纯内存库。
    explicit SqlEntityStore(const std::string& path)
        : RecordEntityStore(std::make_unique<storage::SqliteBackend>(path)) {}
};

} // namespace entitytree
