#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "storage/backends/memory_backend.h"

namespace storage {

/// 文件目录后端：内存索引（MemoryBackend）+ 每表持久化。
/// 布局：
///   <root>/<table>/schema.json   表结构（建表时写入）
///   <root>/<table>/wal.jsonl     append-only 日志（put/remove 两种操作）
/// 打开时回放全部 WAL 重建内存索引；update_where 先查后改、
/// 按整行 put 形式落日志，因此回放只需两种操作。
/// 单线程使用；每次写操作落盘并 flush。
class FileBackend : public RecordBackend {
public:
    /// root 为数据目录，不存在则创建；存在则加载全部表并回放 WAL。
    explicit FileBackend(const std::string& root);
    ~FileBackend() override = default;

    FileBackend(const FileBackend&) = delete;
    FileBackend& operator=(const FileBackend&) = delete;

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
    std::string table_dir(const std::string& table) const;
    void wal_log(const std::string& table, const nlohmann::json& op);
    void load();

    MemoryBackend mem_;
    std::string root_;
    std::map<std::string, TableSchema> schemas_;
};

} // namespace storage
