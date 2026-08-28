#pragma once

#include <string>

#include "storage/record_backend.h"

struct sqlite3;  // 前向声明

namespace storage {

/// SQLite 后端：统一 SQL 参照实现。
/// TableSchema 直接翻译成 CREATE TABLE / CREATE INDEX；
/// MySQL/Postgres 方言化时以本类为蓝本（建表 SQL、占位符、分页语法）。
class SqliteBackend : public RecordBackend {
public:
    /// path 为 SQLite 文件路径；":memory:" 表示纯内存库。
    explicit SqliteBackend(const std::string& path);
    ~SqliteBackend() override;

    SqliteBackend(const SqliteBackend&) = delete;
    SqliteBackend& operator=(const SqliteBackend&) = delete;

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
    void exec(const std::string& sql);

    sqlite3* db_ = nullptr;
    std::map<std::string, TableSchema> schemas_;
};

} // namespace storage
