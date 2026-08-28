#include "storage/backends/mysql_backend.h"

#include <mysql.h>  // libmysqlclient 或 MariaDB Connector/C（include 路径由 CMake 探测）

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace storage {
namespace {

[[noreturn]] void mysql_error_throw(MYSQL* conn, const char* what) {
    throw std::runtime_error(std::string("mysql ") + what + ": " + mysql_error(conn));
}

/// 解析 "mysql://user:password@host:port/dbname"（port 可省）。
struct Dsn {
    std::string user, password, host = "127.0.0.1", database;
    unsigned int port = 3306;
};

Dsn parse_dsn(const std::string& dsn) {
    const std::string prefix = "mysql://";
    if (dsn.rfind(prefix, 0) != 0)
        throw std::runtime_error("mysql dsn must start with mysql://");
    std::string rest = dsn.substr(prefix.size());
    Dsn d;
    auto at = rest.rfind('@');
    if (at == std::string::npos) throw std::runtime_error("mysql dsn missing @");
    std::string auth = rest.substr(0, at);
    std::string hostpart = rest.substr(at + 1);
    auto colon = auth.find(':');
    if (colon == std::string::npos) throw std::runtime_error("mysql dsn missing password");
    d.user = auth.substr(0, colon);
    d.password = auth.substr(colon + 1);
    auto slash = hostpart.find('/');
    if (slash == std::string::npos) throw std::runtime_error("mysql dsn missing /dbname");
    d.database = hostpart.substr(slash + 1);
    std::string hostport = hostpart.substr(0, slash);
    auto hcolon = hostport.find(':');
    if (hcolon != std::string::npos) {
        d.host = hostport.substr(0, hcolon);
        d.port = (unsigned int)std::stoul(hostport.substr(hcolon + 1));
    } else if (!hostport.empty()) {
        d.host = hostport;
    }
    if (d.database.empty()) throw std::runtime_error("mysql dsn empty dbname");
    return d;
}

const char* type_name(FieldType t, bool indexed) {
    switch (t) {
        case FieldType::kInt: return "BIGINT";
        case FieldType::kReal: return "DOUBLE";
        case FieldType::kText: return indexed ? "VARCHAR(191)" : "TEXT";
    }
    return "TEXT";
}

} // namespace

MySqlBackend::MySqlBackend(const std::string& dsn) {
    Dsn d = parse_dsn(dsn);
    conn_ = mysql_init(nullptr);
    if (!conn_) throw std::runtime_error("mysql init failed");
    if (!mysql_real_connect(conn_, d.host.c_str(), d.user.c_str(), d.password.c_str(),
                            d.database.c_str(), d.port, nullptr, 0)) {
        std::string msg = mysql_error(conn_);
        mysql_close(conn_);
        conn_ = nullptr;
        throw std::runtime_error("mysql connect failed: " + msg);
    }
}

MySqlBackend::~MySqlBackend() {
    if (conn_) mysql_close(conn_);
}

const TableSchema& MySqlBackend::schema_of(const std::string& table) const {
    auto it = schemas_.find(table);
    if (it == schemas_.end())
        throw std::runtime_error("mysql backend: unknown table " + table);
    return it->second;
}

std::string MySqlBackend::literal(const Value& v) {
    if (is_null(v)) return "NULL";
    if (auto* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
    if (auto* d = std::get_if<double>(&v)) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", *d);
        return buf;
    }
    const std::string& s = as_text(v);
    std::string buf;
    buf.resize(s.size() * 2 + 1);
    unsigned long n = mysql_real_escape_string(conn_, buf.data(), s.c_str(),
                                               (unsigned long)s.size());
    return "'" + buf.substr(0, n) + "'";
}

void MySqlBackend::create_table(const TableSchema& schema) {
    if (schemas_.count(schema.name)) return;  // 幂等
    // 主键与索引字段：TEXT 降级为 VARCHAR(191)（MySQL TEXT 不能做键）
    std::map<std::string, bool> indexed;
    indexed[schema.pk] = true;
    for (const auto& index : schema.indexes)
        for (const auto& f : index) indexed[f] = true;

    std::string sql = "CREATE TABLE IF NOT EXISTS " + schema.name + " (";
    bool first = true;
    for (const auto& f : schema.fields) {
        if (!first) sql += ", ";
        first = false;
        sql += f.name + " " + type_name(f.type, indexed.count(f.name) > 0);
        if (f.name == schema.pk) {
            sql += " PRIMARY KEY";
            if (schema.auto_seq) sql += " AUTO_INCREMENT";
        }
    }
    sql += ") DEFAULT CHARSET=utf8mb4";
    if (mysql_query(conn_, sql.c_str()) != 0) mysql_error_throw(conn_, "create table");

    int n = 0;
    for (const auto& index : schema.indexes) {
        std::string cols;
        for (const auto& f : index) {
            if (!cols.empty()) cols += ", ";
            cols += f;
        }
        std::string isql = "CREATE INDEX idx_" + schema.name + "_" + std::to_string(n++) +
                           " ON " + schema.name + "(" + cols + ")";
        if (mysql_query(conn_, isql.c_str()) != 0) {
            // 索引已存在（错误 1061）时忽略，保持幂等
            if (mysql_errno(conn_) != 1061) mysql_error_throw(conn_, "create index");
        }
    }
    schemas_.emplace(schema.name, schema);
}

void MySqlBackend::put(const std::string& table, const Record& rec) {
    const TableSchema& s = schema_of(table);
    std::string cols, vals;
    for (const auto& f : s.fields) {
        if (!cols.empty()) { cols += ", "; vals += ", "; }
        cols += f.name;
        auto it = rec.find(f.name);
        vals += literal(it != rec.end() ? it->second : vnull());
    }
    std::string sql = "REPLACE INTO " + table + "(" + cols + ") VALUES(" + vals + ")";
    if (mysql_query(conn_, sql.c_str()) != 0) mysql_error_throw(conn_, "put");
}

int64_t MySqlBackend::append(const std::string& table, Record rec) {
    const TableSchema& s = schema_of(table);
    std::string cols, vals;
    for (const auto& f : s.fields) {
        if (f.name == s.pk) continue;  // 自增主键不写
        if (!cols.empty()) { cols += ", "; vals += ", "; }
        cols += f.name;
        auto it = rec.find(f.name);
        vals += literal(it != rec.end() ? it->second : vnull());
    }
    std::string sql = "INSERT INTO " + table + "(" + cols + ") VALUES(" + vals + ")";
    if (mysql_query(conn_, sql.c_str()) != 0) mysql_error_throw(conn_, "append");
    return (int64_t)mysql_insert_id(conn_);
}

std::vector<Record> MySqlBackend::select(const std::string& table, const std::string& sql) {
    const TableSchema& s = schema_of(table);
    if (mysql_query(conn_, sql.c_str()) != 0) mysql_error_throw(conn_, "select");
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) mysql_error_throw(conn_, "store result");
    std::vector<Record> out;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        Record rec;
        for (size_t i = 0; i < s.fields.size(); ++i) {
            if (!row[i]) {
                rec[s.fields[i].name] = vnull();
                continue;
            }
            switch (s.fields[i].type) {
                case FieldType::kInt:
                    rec[s.fields[i].name] = vint((int64_t)std::strtoll(row[i], nullptr, 10));
                    break;
                case FieldType::kReal:
                    rec[s.fields[i].name] = vreal(std::strtod(row[i], nullptr));
                    break;
                case FieldType::kText:
                    rec[s.fields[i].name] = vtext(std::string(row[i]));
                    break;
            }
        }
        out.push_back(std::move(rec));
    }
    mysql_free_result(res);
    return out;
}

std::string MySqlBackend::where_clause(const std::vector<Condition>& conds) {
    std::string sql;
    bool first = true;
    for (const auto& c : conds) {
        if (!first) sql += " AND ";
        first = false;
        switch (c.op) {
            case Op::Eq: sql += c.field + " = " + literal(c.value); break;
            case Op::Le: sql += c.field + " <= " + literal(c.value); break;
            case Op::Ge: sql += c.field + " >= " + literal(c.value); break;
            case Op::IsNull: sql += c.field + " IS NULL"; break;
            case Op::NotNull: sql += c.field + " IS NOT NULL"; break;
        }
    }
    return sql;
}

std::optional<Record> MySqlBackend::get(const std::string& table, const Value& pk) {
    const TableSchema& s = schema_of(table);
    auto rows = select(table, "SELECT * FROM " + table + " WHERE " + s.pk + " = " +
                              literal(pk) + " LIMIT 1");
    if (rows.empty()) return std::nullopt;
    return rows[0];
}

bool MySqlBackend::remove(const std::string& table, const Value& pk) {
    const TableSchema& s = schema_of(table);
    std::string sql = "DELETE FROM " + table + " WHERE " + s.pk + " = " + literal(pk);
    if (mysql_query(conn_, sql.c_str()) != 0) mysql_error_throw(conn_, "remove");
    return mysql_affected_rows(conn_) > 0;
}

std::vector<Record> MySqlBackend::query(const std::string& table,
                                        std::vector<Condition> conds,
                                        std::vector<Ordering> order,
                                        int limit) {
    std::string sql = "SELECT * FROM " + table;
    if (!conds.empty()) sql += " WHERE " + where_clause(conds);
    if (!order.empty()) {
        sql += " ORDER BY ";
        bool first = true;
        for (const auto& o : order) {
            if (!first) sql += ", ";
            first = false;
            sql += o.field + (o.desc ? " DESC" : " ASC");
        }
    }
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);
    return select(table, sql);
}

void MySqlBackend::update_where(const std::string& table,
                                std::vector<Condition> conds,
                                Record patch) {
    schema_of(table);  // 校验表存在
    std::string sql = "UPDATE " + table + " SET ";
    bool first = true;
    for (const auto& [f, v] : patch) {
        if (!first) sql += ", ";
        first = false;
        sql += f + " = " + literal(v);
    }
    if (!conds.empty()) sql += " WHERE " + where_clause(conds);
    if (mysql_query(conn_, sql.c_str()) != 0) mysql_error_throw(conn_, "update");
}

} // namespace storage
