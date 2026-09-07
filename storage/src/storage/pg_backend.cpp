#include "storage/backends/pg_backend.h"

#include <libpq-fe.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace storage {
namespace {

/// 校验结果状态；失败时抛出（不负责 PQclear，由调用方清理）。
void check_result(PGresult* res, const char* what) {
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
        std::string msg = PQresultErrorMessage(res);
        throw std::runtime_error(std::string("pg ") + what + ": " + msg);
    }
}

const char* type_name(FieldType t) {
    switch (t) {
        case FieldType::kInt: return "BIGINT";
        case FieldType::kReal: return "DOUBLE PRECISION";
        case FieldType::kText: return "TEXT";
    }
    return "TEXT";
}

/// Value → 参数文本（libpq 全文本协议）；null 由调用方单独处理。
std::string param_text(const Value& v) {
    if (auto* i = std::get_if<int64_t>(&v)) return std::to_string(*i);
    if (auto* d = std::get_if<double>(&v)) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", *d);
        return buf;
    }
    return as_text(v);
}

/// 前缀 → LIKE 模式：转义 \ % _ 后追加 %（配合 ESCAPE '\' 使用）。
std::string prefix_pattern(const std::string& prefix) {
    std::string pat;
    for (char ch : prefix) {
        if (ch == '\\' || ch == '%' || ch == '_') pat += '\\';
        pat += ch;
    }
    return pat + "%";
}

/// 一次参数化执行。params 中 nullopt 表示 SQL NULL。
struct PgResultGuard {
    PGresult* res = nullptr;
    ~PgResultGuard() { if (res) PQclear(res); }
};

PgResultGuard exec_params(PGconn* conn, const std::string& sql,
                          const std::vector<std::optional<std::string>>& params) {
    std::vector<std::string> storage;
    std::vector<const char*> values;
    storage.reserve(params.size());
    values.reserve(params.size());
    for (const auto& p : params) {
        if (p) {
            storage.push_back(*p);
            values.push_back(storage.back().c_str());
        } else {
            values.push_back(nullptr);
        }
    }
    PGresult* res = PQexecParams(conn, sql.c_str(), (int)values.size(), nullptr,
                                 values.data(), nullptr, nullptr, 0);
    try {
        check_result(res, "exec");
    } catch (...) {
        PQclear(res);
        throw;
    }
    return PgResultGuard{res};
}

} // namespace

PgBackend::PgBackend(const std::string& conninfo) {
    conn_ = PQconnectdb(conninfo.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
        std::string msg = PQerrorMessage(conn_);
        PQfinish(conn_);
        conn_ = nullptr;
        throw std::runtime_error("pg connect failed: " + msg);
    }
}

PgBackend::~PgBackend() {
    if (conn_) PQfinish(conn_);
}

const TableSchema& PgBackend::schema_of(const std::string& table) const {
    auto it = schemas_.find(table);
    if (it == schemas_.end())
        throw std::runtime_error("pg backend: unknown table " + table);
    return it->second;
}

void PgBackend::create_table(const TableSchema& schema) {
    if (schemas_.count(schema.name)) return;  // 幂等
    std::string sql = "CREATE TABLE IF NOT EXISTS " + schema.name + " (";
    bool first = true;
    for (const auto& f : schema.fields) {
        if (!first) sql += ", ";
        first = false;
        if (f.name == schema.pk && schema.auto_seq) {
            sql += f.name + " BIGSERIAL PRIMARY KEY";
        } else {
            sql += f.name + " " + type_name(f.type);
            if (f.name == schema.pk) sql += " PRIMARY KEY";
        }
    }
    sql += ")";
    {
        PgResultGuard r{PQexec(conn_, sql.c_str())};
        check_result(r.res, "create table");
    }
    int n = 0;
    for (const auto& index : schema.indexes) {
        std::string cols;
        for (const auto& f : index) {
            if (!cols.empty()) cols += ", ";
            cols += f;
        }
        std::string isql = "CREATE INDEX IF NOT EXISTS idx_" + schema.name + "_" +
                           std::to_string(n++) + " ON " + schema.name + "(" + cols + ")";
        PgResultGuard r{PQexec(conn_, isql.c_str())};
        check_result(r.res, "create index");
    }
    schemas_.emplace(schema.name, schema);
}

void PgBackend::put(const std::string& table, const Record& rec) {
    const TableSchema& s = schema_of(table);
    std::string cols, marks, updates;
    std::vector<std::optional<std::string>> params;
    int n = 0;
    for (const auto& f : s.fields) {
        if (!cols.empty()) { cols += ", "; marks += ", "; }
        cols += f.name;
        marks += "$" + std::to_string(++n);
        auto it = rec.find(f.name);
        const Value& v = it != rec.end() ? it->second : vnull();
        params.push_back(is_null(v) ? std::nullopt
                                    : std::optional<std::string>(param_text(v)));
        if (f.name != s.pk) {
            if (!updates.empty()) updates += ", ";
            updates += f.name + " = EXCLUDED." + f.name;
        }
    }
    std::string sql = "INSERT INTO " + table + "(" + cols + ") VALUES(" + marks + ")" +
                      " ON CONFLICT (" + s.pk + ") DO UPDATE SET " + updates;
    exec_params(conn_, sql, params);
}

int64_t PgBackend::append(const std::string& table, Record rec) {
    const TableSchema& s = schema_of(table);
    std::string cols, marks;
    std::vector<std::optional<std::string>> params;
    int n = 0;
    for (const auto& f : s.fields) {
        if (f.name == s.pk) continue;  // 自增主键不写
        if (!cols.empty()) { cols += ", "; marks += ", "; }
        cols += f.name;
        marks += "$" + std::to_string(++n);
        auto it = rec.find(f.name);
        const Value& v = it != rec.end() ? it->second : vnull();
        params.push_back(is_null(v) ? std::nullopt
                                    : std::optional<std::string>(param_text(v)));
    }
    std::string sql = "INSERT INTO " + table + "(" + cols + ") VALUES(" + marks + ")" +
                      " RETURNING " + s.pk;
    auto r = exec_params(conn_, sql, params);
    return std::strtoll(PQgetvalue(r.res, 0, 0), nullptr, 10);
}

std::optional<Record> PgBackend::get(const std::string& table, const Value& pk) {
    const TableSchema& s = schema_of(table);
    auto r = exec_params(conn_, "SELECT * FROM " + table + " WHERE " + s.pk + " = $1",
                         {is_null(pk) ? std::nullopt
                                      : std::optional<std::string>(param_text(pk))});
    if (PQntuples(r.res) == 0) return std::nullopt;
    Record rec;
    for (size_t i = 0; i < s.fields.size(); ++i) {
        if (PQgetisnull(r.res, 0, (int)i)) {
            rec[s.fields[i].name] = vnull();
            continue;
        }
        const char* val = PQgetvalue(r.res, 0, (int)i);
        switch (s.fields[i].type) {
            case FieldType::kInt:
                rec[s.fields[i].name] = vint((int64_t)std::strtoll(val, nullptr, 10));
                break;
            case FieldType::kReal:
                rec[s.fields[i].name] = vreal(std::strtod(val, nullptr));
                break;
            case FieldType::kText:
                rec[s.fields[i].name] = vtext(std::string(val));
                break;
        }
    }
    return rec;
}

bool PgBackend::remove(const std::string& table, const Value& pk) {
    const TableSchema& s = schema_of(table);
    auto r = exec_params(conn_, "DELETE FROM " + table + " WHERE " + s.pk + " = $1",
                         {is_null(pk) ? std::nullopt
                                      : std::optional<std::string>(param_text(pk))});
    return std::string(PQcmdTuples(r.res)) != "0";
}

std::vector<Record> PgBackend::query(const std::string& table,
                                     std::vector<Condition> conds,
                                     std::vector<Ordering> order,
                                     int limit) {
    const TableSchema& s = schema_of(table);
    std::string sql = "SELECT * FROM " + table;
    std::vector<std::optional<std::string>> params;
    int n = 0;
    if (!conds.empty()) {
        sql += " WHERE ";
        bool first = true;
        for (const auto& c : conds) {
            if (!first) sql += " AND ";
            first = false;
            switch (c.op) {
                case Op::Eq: sql += c.field + " = $" + std::to_string(++n); break;
                case Op::Ne: sql += c.field + " != $" + std::to_string(++n); break;  // NULL 行不命中，与 Le/Ge 一致
                case Op::Le: sql += c.field + " <= $" + std::to_string(++n); break;
                case Op::Ge: sql += c.field + " >= $" + std::to_string(++n); break;
                case Op::Prefix:  // LIKE $n ESCAPE '\'；NULL 行不命中（LIKE 对 NULL 求值为 NULL）
                    sql += c.field + " LIKE $" + std::to_string(++n) + " ESCAPE '\\'";
                    break;
                case Op::IsNull: sql += c.field + " IS NULL"; break;
                case Op::NotNull: sql += c.field + " IS NOT NULL"; break;
            }
            if (c.op == Op::Prefix)
                params.push_back(prefix_pattern(as_text(c.value)));
            else if (c.op != Op::IsNull && c.op != Op::NotNull)
                params.push_back(is_null(c.value)
                                     ? std::nullopt
                                     : std::optional<std::string>(param_text(c.value)));
        }
    }
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

    auto r = exec_params(conn_, sql, params);
    std::vector<Record> out;
    int rows = PQntuples(r.res);
    for (int row = 0; row < rows; ++row) {
        Record rec;
        for (size_t i = 0; i < s.fields.size(); ++i) {
            if (PQgetisnull(r.res, row, (int)i)) {
                rec[s.fields[i].name] = vnull();
                continue;
            }
            const char* val = PQgetvalue(r.res, row, (int)i);
            switch (s.fields[i].type) {
                case FieldType::kInt:
                    rec[s.fields[i].name] = vint((int64_t)std::strtoll(val, nullptr, 10));
                    break;
                case FieldType::kReal:
                    rec[s.fields[i].name] = vreal(std::strtod(val, nullptr));
                    break;
                case FieldType::kText:
                    rec[s.fields[i].name] = vtext(std::string(val));
                    break;
            }
        }
        out.push_back(std::move(rec));
    }
    return out;
}

void PgBackend::update_where(const std::string& table,
                             std::vector<Condition> conds,
                             Record patch) {
    schema_of(table);  // 校验表存在
    std::string sql = "UPDATE " + table + " SET ";
    std::vector<std::optional<std::string>> params;
    int n = 0;
    bool first = true;
    for (const auto& [f, v] : patch) {
        if (!first) sql += ", ";
        first = false;
        sql += f + " = $" + std::to_string(++n);
        params.push_back(is_null(v) ? std::nullopt
                                    : std::optional<std::string>(param_text(v)));
    }
    if (!conds.empty()) {
        sql += " WHERE ";
        first = true;
        for (const auto& c : conds) {
            if (!first) sql += " AND ";
            first = false;
            switch (c.op) {
                case Op::Eq: sql += c.field + " = $" + std::to_string(++n); break;
                case Op::Ne: sql += c.field + " != $" + std::to_string(++n); break;  // NULL 行不命中，与 Le/Ge 一致
                case Op::Le: sql += c.field + " <= $" + std::to_string(++n); break;
                case Op::Ge: sql += c.field + " >= $" + std::to_string(++n); break;
                case Op::Prefix:  // LIKE $n ESCAPE '\'；NULL 行不命中（LIKE 对 NULL 求值为 NULL）
                    sql += c.field + " LIKE $" + std::to_string(++n) + " ESCAPE '\\'";
                    break;
                case Op::IsNull: sql += c.field + " IS NULL"; break;
                case Op::NotNull: sql += c.field + " IS NOT NULL"; break;
            }
            if (c.op == Op::Prefix)
                params.push_back(prefix_pattern(as_text(c.value)));
            else if (c.op != Op::IsNull && c.op != Op::NotNull)
                params.push_back(is_null(c.value)
                                     ? std::nullopt
                                     : std::optional<std::string>(param_text(c.value)));
        }
    }
    exec_params(conn_, sql, params);
}

} // namespace storage
