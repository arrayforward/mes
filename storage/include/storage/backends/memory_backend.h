#pragma once

#include <unordered_map>

#include "storage/record_backend.h"

namespace storage {

/// 内存后端：参考实现，亦用于测试。
/// 写时维护声明过的二级索引（一次写入、多次索引）；Eq 条件命中索引桶，
/// 其余条件全表过滤。upsert 时先从旧值索引桶摘除再入新桶。
class MemoryBackend : public RecordBackend {
public:
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

    /// 原样插入（恢复/回放用）：不改写任何字段（含自增主键），
    /// auto_seq 表计数器推进到不小于记录主键值。
    void insert_raw(const std::string& table, Record rec);

private:
    struct Table {
        TableSchema schema;
        std::map<std::string, Record> rows;  // pk_key -> 记录
        // 索引：字段名 -> (值编码 -> 主键集合)
        std::unordered_map<std::string, std::map<std::string, std::vector<std::string>>> idx;
        int64_t seq_counter = 0;
    };

    static std::string value_key(const Value& v);        // 值 → 桶键
    static bool matches(const Record& rec, const Condition& c);
    static int compare_values(const Value& a, const Value& b);

    Table& table_of(const std::string& name);
    void index_insert(Table& t, const std::string& pk_key, const Record& rec);
    void index_remove(Table& t, const std::string& pk_key, const Record& rec);

    std::unordered_map<std::string, Table> tables_;
};

} // namespace storage
