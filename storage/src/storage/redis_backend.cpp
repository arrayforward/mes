#include "storage/backends/redis_backend.h"

#include <cstdio>
#include <stdexcept>

#include "../../src/storage/redis/resp_client.h"
#include "storage/record_codec.h"

namespace storage {
namespace {

[[noreturn]] void redis_throw(const RespReply& r, const char* what) {
    throw std::runtime_error(std::string("redis ") + what + ": " + r.str);
}

} // namespace

RedisBackend::RedisBackend(const std::string& addr, std::string key_prefix)
    : prefix_(std::move(key_prefix)) {
    auto colon = addr.rfind(':');
    if (colon == std::string::npos)
        throw std::runtime_error("redis addr must be host:port");
    std::string host = addr.substr(0, colon);
    uint16_t port = (uint16_t)std::stoul(addr.substr(colon + 1));
    cli_ = std::make_unique<RespClient>(host, port);
    auto pong = cli_->command({"PING"});
    if (pong.is_error()) redis_throw(pong, "ping");
}

RedisBackend::~RedisBackend() = default;

std::string RedisBackend::encode_value(const Value& v) const {
    if (is_null(v)) return "n";
    if (auto* i = std::get_if<int64_t>(&v)) return "i:" + std::to_string(*i);
    if (auto* d = std::get_if<double>(&v)) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "r:%.17g", *d);
        return buf;
    }
    return "s:" + as_text(v);
}

Value RedisBackend::decode_value(const std::string& s) const {
    if (s == "n") return vnull();
    if (s.rfind("i:", 0) == 0) return vint(std::stoll(s.substr(2)));
    if (s.rfind("r:", 0) == 0) return vreal(std::stod(s.substr(2)));
    if (s.rfind("s:", 0) == 0) return vtext(s.substr(2));
    throw std::runtime_error("redis backend: bad encoded value");
}

std::string RedisBackend::index_key(const TableSchema& s,
                                    const std::vector<std::string>& fields,
                                    const Record& rec) const {
    json vals = json::array();
    for (const auto& f : fields) {
        auto it = rec.find(f);
        vals.push_back(encode_value(it != rec.end() ? it->second : vnull()));
    }
    return key("idx:" + s.name + ":" + fields.front() + ":" + vals.dump());
}

const TableSchema& RedisBackend::schema_of(const std::string& table) {
    auto it = schemas_.find(table);
    if (it != schemas_.end()) return it->second;
    // 尝试从 redis 加载（重连既有数据库的场景）
    auto r = cli_->command({"GET", key("sch:" + table)});
    if (r.is_error()) redis_throw(r, "get schema");
    if (r.type == RespReply::Type::BulkString) {
        TableSchema s = schema_from_json(json::parse(r.str));
        auto [ins, _] = schemas_.emplace(s.name, std::move(s));
        return ins->second;
    }
    throw std::runtime_error("redis backend: unknown table " + table);
}

void RedisBackend::create_table(const TableSchema& schema) {
    if (schemas_.count(schema.name)) return;  // 幂等（本进程内）
    // 远端已存在同名牌则沿用（重连场景），否则写入
    auto r = cli_->command({"GET", key("sch:" + schema.name)});
    if (r.is_error()) redis_throw(r, "get schema");
    if (r.type != RespReply::Type::BulkString) {
        auto s = cli_->command({"SET", key("sch:" + schema.name), schema_to_json(schema).dump()});
        if (s.is_error()) redis_throw(s, "set schema");
    }
    schemas_.emplace(schema.name, schema);
}

void RedisBackend::insert_row(const TableSchema& s, const Record& rec) {
    std::string pk_enc = encode_value(rec.at(s.pk));
    std::string row_key = key("row:" + s.name + ":" + pk_enc);
    std::vector<std::string> cmd = {"HSET", row_key};
    for (const auto& f : s.fields) {
        auto it = rec.find(f.name);
        cmd.push_back(f.name);
        cmd.push_back(encode_value(it != rec.end() ? it->second : vnull()));
    }
    auto r = cli_->command(cmd);
    if (r.is_error()) redis_throw(r, "hset");
    auto r2 = cli_->command({"SADD", key("pks:" + s.name), pk_enc});
    if (r2.is_error()) redis_throw(r2, "sadd pks");
    for (const auto& index : s.indexes) {
        auto r3 = cli_->command({"SADD", index_key(s, index, rec), pk_enc});
        if (r3.is_error()) redis_throw(r3, "sadd idx");
    }
}

void RedisBackend::delete_row(const TableSchema& s, const std::string& pk_enc,
                              const Record& old) {
    auto r = cli_->command({"DEL", key("row:" + s.name + ":" + pk_enc)});
    if (r.is_error()) redis_throw(r, "del row");
    auto r2 = cli_->command({"SREM", key("pks:" + s.name), pk_enc});
    if (r2.is_error()) redis_throw(r2, "srem pks");
    for (const auto& index : s.indexes) {
        auto r3 = cli_->command({"SREM", index_key(s, index, old), pk_enc});
        if (r3.is_error()) redis_throw(r3, "srem idx");
    }
}

void RedisBackend::put(const std::string& table, const Record& rec) {
    const TableSchema& s = schema_of(table);
    std::string pk_enc = encode_value(rec.at(s.pk));
    // upsert：先删旧行（含旧索引桶），再插新行
    if (auto old = get(table, rec.at(s.pk))) delete_row(s, pk_enc, *old);
    insert_row(s, rec);
}

int64_t RedisBackend::append(const std::string& table, Record rec) {
    const TableSchema& s = schema_of(table);
    auto r = cli_->command({"INCR", key("seq:" + table)});
    if (r.is_error()) redis_throw(r, "incr");
    int64_t seq = r.integer;
    rec[s.pk] = vint(seq);
    insert_row(s, rec);
    return seq;
}

std::vector<Record> RedisBackend::rows_of(const std::vector<std::string>& pks,
                                          const std::string& table) {
    std::vector<Record> out;
    for (const auto& pk_enc : pks) {
        auto r = cli_->command({"HGETALL", key("row:" + table + ":" + pk_enc)});
        if (r.is_error()) redis_throw(r, "hgetall");
        if (r.type != RespReply::Type::Array || r.array.empty()) continue;
        Record rec;
        for (size_t i = 0; i + 1 < r.array.size(); i += 2)
            rec[r.array[i].str] = decode_value(r.array[i + 1].str);
        out.push_back(std::move(rec));
    }
    return out;
}

std::optional<Record> RedisBackend::get(const std::string& table, const Value& pk) {
    std::string pk_enc = encode_value(pk);
    auto rows = rows_of({pk_enc}, table);
    if (rows.empty()) return std::nullopt;
    return rows[0];
}

bool RedisBackend::remove(const std::string& table, const Value& pk) {
    const TableSchema& s = schema_of(table);
    auto old = get(table, pk);
    if (!old) return false;
    delete_row(s, encode_value(pk), *old);
    return true;
}

std::vector<Record> RedisBackend::query(const std::string& table,
                                        std::vector<Condition> conds,
                                        std::vector<Ordering> order,
                                        int limit) {
    const TableSchema& s = schema_of(table);

    // 候选主键：Eq 全覆盖的最长索引桶，否则全量主键
    std::vector<std::string> pks;
    if (const auto* best = best_eq_index(s, conds)) {
        Record probe;
        for (const auto& c : conds)
            if (c.op == Op::Eq) probe[c.field] = c.value;
        auto r = cli_->command({"SMEMBERS", index_key(s, *best, probe)});
        if (r.is_error()) redis_throw(r, "smembers idx");
        if (r.type == RespReply::Type::Array)
            for (const auto& m : r.array) pks.push_back(m.str);
    } else {
        auto r = cli_->command({"SMEMBERS", key("pks:" + table)});
        if (r.is_error()) redis_throw(r, "smembers pks");
        if (r.type == RespReply::Type::Array)
            for (const auto& m : r.array) pks.push_back(m.str);
    }

    std::vector<Record> out;
    for (auto& rec : rows_of(pks, table))
        if (record_matches(rec, conds)) out.push_back(std::move(rec));
    sort_records(out, order);
    if (limit > 0 && (int)out.size() > limit) out.resize((size_t)limit);
    return out;
}

void RedisBackend::update_where(const std::string& table,
                                std::vector<Condition> conds,
                                Record patch) {
    const TableSchema& s = schema_of(table);
    auto rows = query(table, conds, {}, 0);
    for (auto& rec : rows) {
        Record old = rec;
        for (const auto& [f, v] : patch) rec[f] = v;
        delete_row(s, encode_value(old.at(s.pk)), old);
        insert_row(s, rec);
    }
}

} // namespace storage
