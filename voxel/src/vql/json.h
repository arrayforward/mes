// ============================================================================
// 文件: json.h
// 模块: stmb_vql(迷你 JSON,不引第三方库)
// 用途: 声明 JsonValue —— null/bool/double/string/array/object 的变体类型,
//       以及递归下降解析器与紧凑/缩进两种序列化。
// 设计思路:
//   1. object 用 vector<pair> 保序存储(写出顺序即构造顺序,对工具 schema
//      这类需要可读输出的场景更友好),查找为线性扫描(数据量小);
//   2. 解析器对非法输入返回空 optional + 错误信息(含位置),绝不崩溃;
//      字符串支持常见转义,非 ASCII(UTF-8)字节原样透传;
//   3. 数字一律按 double 存储(VQL/工具调用的参数精度足够)。
// 架构角色: vql 模块的基础工具,供 VQL 结果封装与 Function Calling 使用。
// ============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace stmb {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<JsonValue>;
    using Object = std::vector<std::pair<std::string, JsonValue>>;  // 保序

    JsonValue() : type_(Type::Null), bool_(false), number_(0.0) {}

    // 工厂:各类型构造
    static JsonValue makeNull() { return JsonValue(); }
    static JsonValue makeBool(bool v);
    static JsonValue makeNumber(double v);
    static JsonValue makeString(const std::string& v);
    static JsonValue makeArray(Array v = {});
    static JsonValue makeObject(Object v = {});

    // 类型判定与访问(类型不符时访问返回零值/空引用,调用方先判定)
    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool() const { return bool_; }
    double asNumber() const { return number_; }
    const std::string& asString() const { return string_; }
    const Array& asArray() const { return array_; }
    const Object& asObject() const { return object_; }
    Array& asArray() { return array_; }
    Object& asObject() { return object_; }

    // 容器便捷操作
    void push(JsonValue v);                       // 向 array 追加
    void set(const std::string& key, JsonValue v);  // 向 object 设置(覆盖同键)
    bool has(const std::string& key) const;       // object 是否含键
    const JsonValue& at(const std::string& key) const;  // 取 object 键值(不存在返回 null 静态值)

    // 解析:非法输入返回空 optional 并填充 error(含字符位置),不崩溃
    static std::optional<JsonValue> parse(const std::string& text, std::string* error);

    // 序列化:indent < 0 为紧凑模式,否则按给定缩进宽度美化输出
    std::string dump(int indent = -1) const;

private:
    Type type_;
    bool bool_;
    double number_;
    std::string string_;
    Array array_;
    Object object_;
};

}  // namespace stmb
