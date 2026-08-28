#include "storage/backends/memory_backend.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace storage {
namespace {

[[noreturn]] void no_table(const std::string& name) {
    throw std::runtime_error("memory backend: unknown table " + name);
}

} // namespace

MemoryBackend::Table& MemoryBackend::table_of(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end()) no_table(name);
    return it->second;
}

void MemoryBackend::create_table(const TableSchema& schema) {
    if (tables_.count(schema.name)) return;  // 幂等
    Table t;
    t.schema = schema;
    tables_.emplace(schema.name, std::move(t));
}

std::string MemoryBackend::value_key(const Value& v) {
    if (is_null(v)) return "n:";
    if (auto* i = std::get_if<int64_t>(&v)) return "i:" + std::to_string(*i);
    if (auto* d = std::get_if<double>(&v)) {
        // 按位编码，避免浮点格式差异
        uint64_t bits;
        std::memcpy(&bits, d, sizeof(bits));
        return "r:" + std::to_string(bits);
    }
    return "s:" + as_text(v);
}

void MemoryBackend::index_insert(Table& t, const std::string& pk_key, const Record& rec) {
    for (const auto& index : t.schema.indexes) {
        // 复合索引：各字段值编码拼接
        std::string key;
        for (const auto& field : index) {
            auto it = rec.find(field);
            key += value_key(it != rec.end() ? it->second : vnull()) + "|";
        }
        t.idx[index.front()][key].push_back(pk_key);
    }
}

void MemoryBackend::index_remove(Table& t, const std::string& pk_key, const Record& rec) {
    for (const auto& index : t.schema.indexes) {
        std::string key;
        for (const auto& field : index) {
            auto it = rec.find(field);
            key += value_key(it != rec.end() ? it->second : vnull()) + "|";
        }
        auto& bucket = t.idx[index.front()][key];
        bucket.erase(std::remove(bucket.begin(), bucket.end(), pk_key), bucket.end());
    }
}

void MemoryBackend::put(const std::string& table, const Record& rec) {
    Table& t = table_of(table);
    if (t.schema.auto_seq)
        throw std::runtime_error("memory backend: put on auto_seq table " + table);
    auto pk_it = rec.find(t.schema.pk);
    if (pk_it == rec.end())
        throw std::runtime_error("memory backend: missing pk in put on " + table);
    std::string key = value_key(pk_it->second);
    auto old = t.rows.find(key);
    if (old != t.rows.end()) index_remove(t, key, old->second);  // upsert：先摘旧索引
    index_insert(t, key, rec);
    t.rows[key] = rec;
}

int64_t MemoryBackend::append(const std::string& table, Record rec) {
    Table& t = table_of(table);
    if (!t.schema.auto_seq)
        throw std::runtime_error("memory backend: append on non-auto_seq table " + table);
    int64_t seq = ++t.seq_counter;
    rec[t.schema.pk] = vint(seq);
    std::string key = value_key(rec[t.schema.pk]);
    index_insert(t, key, rec);
    t.rows[key] = std::move(rec);
    return seq;
}

std::optional<Record> MemoryBackend::get(const std::string& table, const Value& pk) {
    Table& t = table_of(table);
    auto it = t.rows.find(value_key(pk));
    if (it == t.rows.end()) return std::nullopt;
    return it->second;
}

bool MemoryBackend::remove(const std::string& table, const Value& pk) {
    Table& t = table_of(table);
    std::string key = value_key(pk);
    auto it = t.rows.find(key);
    if (it == t.rows.end()) return false;
    index_remove(t, key, it->second);
    t.rows.erase(it);
    return true;
}

bool MemoryBackend::matches(const Record& rec, const Condition& c) {
    auto it = rec.find(c.field);
    const Value& v = it != rec.end() ? it->second : vnull();
    switch (c.op) {
        case Op::Eq:
            if (is_null(v) || is_null(c.value)) return is_null(v) && is_null(c.value);
            return value_key(v) == value_key(c.value);
        case Op::Le:
            return !is_null(v) && compare_values(v, c.value) <= 0;
        case Op::Ge:
            return !is_null(v) && compare_values(v, c.value) >= 0;
        case Op::IsNull:
            return is_null(v);
        case Op::NotNull:
            return !is_null(v);
    }
    return false;
}

int MemoryBackend::compare_values(const Value& a, const Value& b) {
    // null 最小；数值（int/real 混合）按数值；文本按字典序
    if (is_null(a) || is_null(b)) {
        if (is_null(a) && is_null(b)) return 0;
        return is_null(a) ? -1 : 1;
    }
    bool a_num = std::holds_alternative<int64_t>(a) || std::holds_alternative<double>(a);
    bool b_num = std::holds_alternative<int64_t>(b) || std::holds_alternative<double>(b);
    if (a_num && b_num) {
        double x = as_real(a), y = as_real(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    const std::string& x = as_text(a);
    const std::string& y = as_text(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

std::vector<Record> MemoryBackend::query(const std::string& table,
                                         std::vector<Condition> conds,
                                         std::vector<Ordering> order,
                                         int limit) {
    Table& t = table_of(table);

    // 收集 Eq 条件
    std::map<std::string, const Value*> eq;
    for (const auto& c : conds)
        if (c.op == Op::Eq) eq[c.field] = &c.value;

    // 候选集：选字段数最多且被 Eq 条件全覆盖的索引（复合索引须全部字段都有 Eq），
    // 否则全表扫描
    const std::vector<std::string>* best = nullptr;
    for (const auto& index : t.schema.indexes) {
        bool covered = true;
        for (const auto& f : index)
            if (!eq.count(f)) { covered = false; break; }
        if (covered && (!best || index.size() > best->size())) best = &index;
    }

    std::vector<const Record*> candidates;
    if (best) {
        std::string key;
        for (const auto& f : *best) key += value_key(*eq[f]) + "|";
        auto idx_it = t.idx.find(best->front());
        if (idx_it != t.idx.end()) {
            auto bucket = idx_it->second.find(key);
            if (bucket != idx_it->second.end())
                for (const auto& pk_key : bucket->second) {
                    auto row = t.rows.find(pk_key);
                    if (row != t.rows.end()) candidates.push_back(&row->second);
                }
        }
    } else {
        for (const auto& [k, r] : t.rows) candidates.push_back(&r);
    }

    std::vector<Record> out;
    for (const Record* r : candidates) {
        bool ok = true;
        for (const auto& c : conds)
            if (!matches(*r, c)) { ok = false; break; }
        if (ok) out.push_back(*r);
    }

    for (int i = (int)order.size() - 1; i >= 0; --i) {  // 稳定多级排序
        const Ordering& o = order[(size_t)i];
        std::stable_sort(out.begin(), out.end(), [&](const Record& x, const Record& y) {
            auto xv = x.find(o.field), yv = y.find(o.field);
            const Value& a = xv != x.end() ? xv->second : vnull();
            const Value& b = yv != y.end() ? yv->second : vnull();
            int cmp = compare_values(a, b);
            return o.desc ? cmp > 0 : cmp < 0;
        });
    }

    if (limit > 0 && (int)out.size() > limit) out.resize((size_t)limit);
    return out;
}

void MemoryBackend::update_where(const std::string& table,
                                 std::vector<Condition> conds,
                                 Record patch) {
    Table& t = table_of(table);
    for (auto& [key, rec] : t.rows) {
        bool ok = true;
        for (const auto& c : conds)
            if (!matches(rec, c)) { ok = false; break; }
        if (!ok) continue;
        index_remove(t, key, rec);
        for (const auto& [f, v] : patch) rec[f] = v;
        index_insert(t, key, rec);
    }
}

void MemoryBackend::insert_raw(const std::string& table, Record rec) {
    Table& t = table_of(table);
    auto pk_it = rec.find(t.schema.pk);
    if (pk_it == rec.end())
        throw std::runtime_error("memory backend: missing pk in insert_raw on " + table);
    if (t.schema.auto_seq) {
        int64_t seq = as_int(pk_it->second);
        if (seq > t.seq_counter) t.seq_counter = seq;
    }
    std::string key = value_key(pk_it->second);
    auto old = t.rows.find(key);
    if (old != t.rows.end()) index_remove(t, key, old->second);
    index_insert(t, key, rec);
    t.rows[key] = std::move(rec);
}

} // namespace storage
