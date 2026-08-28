#pragma once

#include <string>

#include "storage/record_backend.h"

typedef struct PGconn PGconn;  // 前向声明，避免在公共头中暴露 libpq-fe.h

namespace storage {

/// PostgreSQL 后端：原生 libpq，不依赖 ODBC；
/// Linux（apt 装 libpq-dev）与 Windows 均可构建。
/// CMake 找不到库时本后端自动跳过。
///
/// 方言要点（与 sqlite 参照实现的差异）：
///   - 自增主键：BIGSERIAL PRIMARY KEY；
///   - upsert：INSERT ... ON CONFLICT (pk) DO UPDATE；
///   - 参数化：PQexecParams + $n 占位符（不做字符串拼接）；
///   - append 用 INSERT ... RETURNING 取回自增 seq。
class PgBackend : public RecordBackend {
public:
    /// conninfo 为 libpq 标准连接串，如
    /// "host=127.0.0.1 port=5432 user=postgres password=xxx dbname=world"。
    explicit PgBackend(const std::string& conninfo);
    ~PgBackend() override;

    PgBackend(const PgBackend&) = delete;
    PgBackend& operator=(const PgBackend&) = delete;

    void create_table(const TableSchema& schema) override;

    void put(const std::string& table, const Record& rec) override;
    int64_t append(const std::string& table, Record rec) override;

    std::optional<Record> get(const std::string& table, const Value& pk) override;
    bool remove(const std::string& table, const Value& pk) override;

    std::vector<Record> query(const std::string& table,
                              std::vector<Condition> conds,
                              std::vector<Ordering> order,
                              int limit) override;

    void update_where(const std::string& table,
                      std::vector<Condition> conds,
                      Record patch) override;

private:
    const TableSchema& schema_of(const std::string& table) const;

    PGconn* conn_ = nullptr;
    std::map<std::string, TableSchema> schemas_;
};

} // namespace storage
