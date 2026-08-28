#pragma once

// storage：统一数据存储层。
// 所有领域模型（事件树 eventstore、时空块 voxelstore、以及未来的模型）
// 都映射到同一套"记录"抽象上；数据库 / Redis / 文件各自只实现一次
// RecordBackend，全部领域模型即自动获得全部后端。
//
// 核心概念：
//   Value      类型化字段值：null / int64 / double / text（JSON 一律存 text）
//   Record     一条记录 = 字段名 → Value
//   TableSchema 表结构：字段类型、主键（或自增 seq）、二级索引声明
//   Condition  查询条件：Eq / Le / Ge / IsNull / NotNull
//   Ordering   排序：字段 + 升降序

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace storage {

/// 类型化字段值。bool 用 int64 0/1；JSON 子文档用 text（dump 后的字符串）。
using Value = std::variant<std::nullptr_t, int64_t, double, std::string>;

/// 一条记录：字段名 → 值。
using Record = std::map<std::string, Value>;

enum class FieldType { kInt, kReal, kText };

struct FieldDef {
    std::string name;
    FieldType type;
};

/// 表结构声明。
/// auto_seq = true：自增主键表（append-only，主键字段由后端分配），
/// 否则以 pk 指定主键字段（upsert 语义）。
struct TableSchema {
    std::string name;
    std::vector<FieldDef> fields;          // 含主键字段
    std::string pk;                        // 主键字段名（auto_seq 时也必须给出，即 seq 字段名）
    bool auto_seq = false;                 // true = append-only 自增主键表
    std::vector<std::vector<std::string>> indexes;  // 二级索引（可复合）
};

enum class Op { Eq, Le, Ge, IsNull, NotNull };

struct Condition {
    std::string field;
    Op op;
    Value value = nullptr;  // IsNull/NotNull 时忽略
};

struct Ordering {
    std::string field;
    bool desc = false;
};

// ---- Value 便捷构造 ----
inline Value vint(int64_t v) { return Value{v}; }
inline Value vreal(double v) { return Value{v}; }
inline Value vtext(std::string v) { return Value{std::move(v)}; }
inline Value vnull() { return Value{nullptr}; }

// ---- Value 读取（类型不符抛 std::bad_variant_access） ----
inline int64_t as_int(const Value& v) { return std::get<int64_t>(v); }
inline double as_real(const Value& v) {
    if (auto* d = std::get_if<double>(&v)) return *d;
    return (double)std::get<int64_t>(v);  // int 可升 double
}
inline const std::string& as_text(const Value& v) { return std::get<std::string>(v); }
inline bool is_null(const Value& v) { return std::holds_alternative<std::nullptr_t>(v); }

/// 可选字段便捷写入：有值取值，无值写 null。
template <typename T>
Value vopt(const std::optional<T>& o) {
    if (!o) return vnull();
    if constexpr (std::is_same_v<T, std::string>) return vtext(*o);
    else if constexpr (std::is_integral_v<T>) return vint((int64_t)*o);
    else return vreal((double)*o);
}

} // namespace storage
