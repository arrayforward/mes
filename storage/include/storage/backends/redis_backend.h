#pragma once

#include <memory>
#include <string>

#include "storage/record_backend.h"

namespace storage {

class RespClient;

/// Redis 后端：自研 RESP2 客户端（Winsock2 / POSIX sockets），无第三方依赖。
///
/// key 布局（{p} = key_prefix，默认 "ust"）：
///   {p}:sch:{table}                 STRING  表结构 JSON
///   {p}:pks:{table}                 SET     全量主键（全表扫描用）
///   {p}:row:{table}:{pk}            HASH    一条记录（字段 → 带类型标签的值）
///   {p}:idx:{table}:{field}:{vals}  SET     索引桶（值为复合索引各字段编码的 JSON 数组）
///   {p}:seq:{table}                 STRING  自增计数（INCR）
///
/// 查询：Eq 命中索引桶（SMEMBERS）否则全表扫描（SMEMBERS pks + HGETALL），
/// 条件过滤 / 排序 / limit 在 C++ 侧完成（与 memory 后端同一套 record_codec 工具）。
class RedisBackend : public RecordBackend {
public:
    /// addr 形式 "host:port"；key_prefix 用于多租户/测试隔离。
    explicit RedisBackend(const std::string& addr, std::string key_prefix = "ust");
    ~RedisBackend() override;

    RedisBackend(const RedisBackend&) = delete;
    RedisBackend& operator=(const RedisBackend&) = delete;

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
    const TableSchema& schema_of(const std::string& table);

    std::string key(const std::string& parts) const { return prefix_ + ":" + parts; }
    std::string encode_value(const Value& v) const;   // 值 → 带类型标签串
    Value decode_value(const std::string& s) const;
    std::string index_key(const TableSchema& s, const std::vector<std::string>& fields,
                          const Record& rec) const;

    std::vector<Record> rows_of(const std::vector<std::string>& pks,
                                const std::string& table);
    void insert_row(const TableSchema& s, const Record& rec);   // 含索引维护
    void delete_row(const TableSchema& s, const std::string& pk_enc, const Record& old);

    std::unique_ptr<RespClient> cli_;
    std::string prefix_;
    std::map<std::string, TableSchema> schemas_;
};

} // namespace storage
