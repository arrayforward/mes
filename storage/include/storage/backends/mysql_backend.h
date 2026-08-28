#pragma once

#include <string>

#include "storage/record_backend.h"

typedef struct st_mysql MYSQL;  // 前向声明，避免在公共头中暴露 mysql.h

namespace storage {

/// MySQL / MariaDB 后端：原生 C API（libmysqlclient 或 MariaDB Connector/C），
/// 不依赖 ODBC；Linux（apt 装 libmysqlclient-dev / libmariadb-dev）与
/// Windows 均可构建。CMake 找不到库时本后端自动跳过。
///
/// 方言要点（与 sqlite 参照实现的差异）：
///   - 自增主键：BIGINT AUTO_INCREMENT PRIMARY KEY；
///   - TEXT 不能做主键/索引：主键与索引字段用 VARCHAR(191)，其余 TEXT；
///   - upsert：REPLACE INTO；
///   - 值一律经 mysql_real_escape_string 转义后拼 SQL。
class MySqlBackend : public RecordBackend {
public:
    /// dsn 形式："mysql://user:password@host:port/dbname"（port 可省，默认 3306）。
    explicit MySqlBackend(const std::string& dsn);
    ~MySqlBackend() override;

    MySqlBackend(const MySqlBackend&) = delete;
    MySqlBackend& operator=(const MySqlBackend&) = delete;

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
    std::string literal(const Value& v);          // Value → SQL 字面量（转义）
    std::string where_clause(const std::vector<Condition>& conds);
    std::vector<Record> select(const std::string& table, const std::string& sql);

    MYSQL* conn_ = nullptr;
    std::map<std::string, TableSchema> schemas_;
};

} // namespace storage
