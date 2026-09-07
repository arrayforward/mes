#include "storage/backends/sqlite_backend.h"

#include <sqlite3.h>

#include <stdexcept>

namespace storage {
namespace {

[[noreturn]] void sql_error(sqlite3* db, const char* what) {
    throw std::runtime_error(std::string("sqlite ") + what + ": " + sqlite3_errmsg(db));
}

class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
            sql_error(db_, "prepare");
    }
    ~Stmt() { sqlite3_finalize(stmt_); }

    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    void bind_value(int i, const Value& v) {
        if (is_null(v)) sqlite3_bind_null(stmt_, i);
        else if (auto* x = std::get_if<int64_t>(&v)) sqlite3_bind_int64(stmt_, i, *x);
        else if (auto* x = std::get_if<double>(&v)) sqlite3_bind_double(stmt_, i, *x);
        else {
            const std::string& s = as_text(v);
            sqlite3_bind_text(stmt_, i, s.c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    bool step() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        sql_error(db_, "step");
    }

    Value column(int col, FieldType t) const {
        if (sqlite3_column_type(stmt_, col) == SQLITE_NULL) return vnull();
        switch (t) {
            case FieldType::kInt: return vint(sqlite3_column_int64(stmt_, col));
            case FieldType::kReal: return vreal(sqlite3_column_double(stmt_, col));
            case FieldType::kText: {
                const unsigned char* p = sqlite3_column_text(stmt_, col);
                return vtext(p ? reinterpret_cast<const char*>(p) : "");
            }
        }
        return vnull();
    }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

const char* type_name(FieldType t) {
    switch (t) {
        case FieldType::kInt: return "INTEGER";
        case FieldType::kReal: return "REAL";
        case FieldType::kText: return "TEXT";
    }
    return "TEXT";
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

} // namespace

SqliteBackend::SqliteBackend(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("sqlite open failed: " + msg);
    }
}

SqliteBackend::~SqliteBackend() {
    if (db_) sqlite3_close(db_);
}

void SqliteBackend::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec: " + msg);
    }
}

const TableSchema& SqliteBackend::schema_of(const std::string& table) const {
    auto it = schemas_.find(table);
    if (it == schemas_.end())
        throw std::runtime_error("sqlite backend: unknown table " + table);
    return it->second;
}

void SqliteBackend::create_table(const TableSchema& schema) {
    if (schemas_.count(schema.name)) return;  // 幂等
    std::string sql = "CREATE TABLE IF NOT EXISTS " + schema.name + " (";
    bool first = true;
    for (const auto& f : schema.fields) {
        if (!first) sql += ", ";
        first = false;
        sql += f.name + " " + type_name(f.type);
        if (f.name == schema.pk) {
            sql += " PRIMARY KEY";
            if (schema.auto_seq) sql += " AUTOINCREMENT";
        }
    }
    sql += ")";
    exec(sql);
    int n = 0;
    for (const auto& index : schema.indexes) {
        std::string cols;
        for (const auto& f : index) {
            if (!cols.empty()) cols += ", ";
            cols += f;
        }
        exec("CREATE INDEX IF NOT EXISTS idx_" + schema.name + "_" + std::to_string(n++) +
             " ON " + schema.name + "(" + cols + ")");
    }
    schemas_.emplace(schema.name, schema);
}

void SqliteBackend::put(const std::string& table, const Record& rec) {
    const TableSchema& s = schema_of(table);
    std::string cols, marks;
    for (const auto& f : s.fields) {
        if (!cols.empty()) { cols += ", "; marks += ", "; }
        cols += f.name;
        marks += "?";
    }
    Stmt st(db_, "INSERT OR REPLACE INTO " + table + "(" + cols + ") VALUES(" + marks + ")");
    int i = 1;
    for (const auto& f : s.fields) {
        auto it = rec.find(f.name);
        st.bind_value(i++, it != rec.end() ? it->second : vnull());
    }
    st.step();
}

int64_t SqliteBackend::append(const std::string& table, Record rec) {
    const TableSchema& s = schema_of(table);
    std::string cols, marks;
    for (const auto& f : s.fields) {
        if (f.name == s.pk) continue;  // 自增主键不写
        if (!cols.empty()) { cols += ", "; marks += ", "; }
        cols += f.name;
        marks += "?";
    }
    Stmt st(db_, "INSERT INTO " + table + "(" + cols + ") VALUES(" + marks + ")");
    int i = 1;
    for (const auto& f : s.fields) {
        if (f.name == s.pk) continue;
        auto it = rec.find(f.name);
        st.bind_value(i++, it != rec.end() ? it->second : vnull());
    }
    st.step();
    return sqlite3_last_insert_rowid(db_);
}

std::optional<Record> SqliteBackend::get(const std::string& table, const Value& pk) {
    const TableSchema& s = schema_of(table);
    Stmt st(db_, "SELECT * FROM " + table + " WHERE " + s.pk + " = ?");
    st.bind_value(1, pk);
    if (!st.step()) return std::nullopt;
    Record rec;
    for (size_t i = 0; i < s.fields.size(); ++i)
        rec[s.fields[i].name] = st.column((int)i, s.fields[i].type);
    return rec;
}

bool SqliteBackend::remove(const std::string& table, const Value& pk) {
    const TableSchema& s = schema_of(table);
    Stmt st(db_, "DELETE FROM " + table + " WHERE " + s.pk + " = ?");
    st.bind_value(1, pk);
    st.step();
    return sqlite3_changes(db_) > 0;
}

std::vector<Record> SqliteBackend::query(const std::string& table,
                                         std::vector<Condition> conds,
                                         std::vector<Ordering> order,
                                         int limit) {
    const TableSchema& s = schema_of(table);
    std::string sql = "SELECT * FROM " + table;
    if (!conds.empty()) {
        sql += " WHERE ";
        bool first = true;
        for (const auto& c : conds) {
            if (!first) sql += " AND ";
            first = false;
            switch (c.op) {
                case Op::Eq: sql += c.field + " = ?"; break;
                case Op::Ne: sql += c.field + " != ?"; break;  // NULL 行不命中（!= 对 NULL 求值为 NULL），与 Le/Ge 一致
                case Op::Le: sql += c.field + " <= ?"; break;
                case Op::Ge: sql += c.field + " >= ?"; break;
                case Op::Prefix: sql += c.field + " LIKE ? ESCAPE '\\'"; break;  // NULL 行不命中（LIKE 对 NULL 求值为 NULL）
                case Op::IsNull: sql += c.field + " IS NULL"; break;
                case Op::NotNull: sql += c.field + " IS NOT NULL"; break;
            }
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

    Stmt st(db_, sql);
    int i = 1;
    for (const auto& c : conds) {
        if (c.op == Op::IsNull || c.op == Op::NotNull) continue;
        if (c.op == Op::Prefix) st.bind_value(i++, vtext(prefix_pattern(as_text(c.value))));
        else st.bind_value(i++, c.value);
    }

    std::vector<Record> out;
    while (st.step()) {
        Record rec;
        for (size_t j = 0; j < s.fields.size(); ++j)
            rec[s.fields[j].name] = st.column((int)j, s.fields[j].type);
        out.push_back(std::move(rec));
    }
    return out;
}

void SqliteBackend::update_where(const std::string& table,
                                 std::vector<Condition> conds,
                                 Record patch) {
    schema_of(table);  // 校验表存在
    std::string sql = "UPDATE " + table + " SET ";
    bool first = true;
    for (const auto& [f, v] : patch) {
        if (!first) sql += ", ";
        first = false;
        sql += f + " = ?";
    }
    if (!conds.empty()) {
        sql += " WHERE ";
        first = true;
        for (const auto& c : conds) {
            if (!first) sql += " AND ";
            first = false;
            switch (c.op) {
                case Op::Eq: sql += c.field + " = ?"; break;
                case Op::Ne: sql += c.field + " != ?"; break;  // NULL 行不命中，与 Le/Ge 一致
                case Op::Le: sql += c.field + " <= ?"; break;
                case Op::Ge: sql += c.field + " >= ?"; break;
                case Op::Prefix: sql += c.field + " LIKE ? ESCAPE '\\'"; break;
                case Op::IsNull: sql += c.field + " IS NULL"; break;
                case Op::NotNull: sql += c.field + " IS NOT NULL"; break;
            }
        }
    }
    Stmt st(db_, sql);
    int i = 1;
    for (const auto& [f, v] : patch) st.bind_value(i++, v);
    for (const auto& c : conds) {
        if (c.op == Op::IsNull || c.op == Op::NotNull) continue;
        if (c.op == Op::Prefix) st.bind_value(i++, vtext(prefix_pattern(as_text(c.value))));
        else st.bind_value(i++, c.value);
    }
    st.step();
}

} // namespace storage
