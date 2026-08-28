#pragma once

// Record / TableSchema / Condition 与 JSON 的互转，以及跨后端复用的
// 记录匹配与排序工具。file 后端（schema.json / wal.jsonl）与
// redis 后端（schema 存储、C++ 侧过滤）共用。

#include <nlohmann/json.hpp>

#include "storage/record.h"

namespace storage {

using nlohmann::json;

// ---- Value / Record / TableSchema 编解码 ----

json value_to_json(const Value& v);          // {"t":"n"} / {"t":"i"|"r"|"s","v":...}
Value value_from_json(const json& j);

json record_to_json(const Record& rec);
Record record_from_json(const json& j);

json schema_to_json(const TableSchema& s);
TableSchema schema_from_json(const json& j);

// ---- 跨后端复用的查询工具（供全表扫描型后端使用） ----

/// 记录是否命中全部条件（AND）。
bool record_matches(const Record& rec, const std::vector<Condition>& conds);

/// 就地按 Ordering 多级稳定排序。
void sort_records(std::vector<Record>& recs, const std::vector<Ordering>& order);

/// 选出"字段数最多且被 Eq 条件全覆盖"的索引，未命中返回 nullptr。
const std::vector<std::string>* best_eq_index(const TableSchema& schema,
                                              const std::vector<Condition>& conds);

} // namespace storage
