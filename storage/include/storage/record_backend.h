#pragma once

#include "storage/record.h"

namespace storage {

/// 统一存储后端 SPI。数据库（sqlite/mysql/postgres）、Redis、文件各自实现一次，
/// 上层所有领域模型（事件树、时空块……）自动获得全部后端。
///
/// 语义约定：
///   put          按主键 upsert（可变当前态表）；
///   append       自增主键表追加（append-only 表），返回分配的 seq；
///   update_where 受限更新：命中条件的行套用 patch（仅用于版本链 seal 等
///                "关闭区间"类操作，不是通用 update）。
class RecordBackend {
public:
    virtual ~RecordBackend() = default;

    /// 建表（幂等：已存在则忽略）。
    virtual void create_table(const TableSchema& schema) = 0;

    virtual void put(const std::string& table, const Record& rec) = 0;
    virtual int64_t append(const std::string& table, Record rec) = 0;

    virtual std::optional<Record> get(const std::string& table, const Value& pk) = 0;
    /// 返回是否删除了行。
    virtual bool remove(const std::string& table, const Value& pk) = 0;

    /// 条件查询。条件之间为 AND；order 为空时顺序不限；limit = 0 表示不限。
    virtual std::vector<Record> query(const std::string& table,
                                      std::vector<Condition> conds = {},
                                      std::vector<Ordering> order = {},
                                      int limit = 0) = 0;

    /// 受限更新：命中条件的行套用 patch（不得改主键）。
    virtual void update_where(const std::string& table,
                              std::vector<Condition> conds,
                              Record patch) = 0;
};

} // namespace storage
