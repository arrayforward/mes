// ============================================================================
// 文件: json.cpp
// 模块: stmb_vql(迷你 JSON,不引第三方库)
// 用途: 实现 JsonValue 的工厂、容器操作、递归下降解析与紧凑/缩进序列化。
// 设计思路:
//   1. 解析器持有原文与游标,逐字符递归下降:skipWs -> parseValue 分派到
//      null/true/false/number/string/array/object 六个子例程;
//   2. 所有失败路径只设置错误信息(含字符位置)并返回空,绝不抛出/崩溃;
//   3. 字符串转义支持 \" \\ \/ \b \f \n \r \t 与 \uXXXX(仅解码到 ASCII,
//      其余按 '?' 占位,UTF-8 原文不受影响);
//   4. 序列化:字符串统一转义控制字符与引号反斜杠;缩进模式按层级展开。
// 架构角色: JsonValue 的唯一实现文件。
// ============================================================================
#include "json.h"

#include <cstdio>
#include <cstdlib>

namespace stmb {
namespace {

// 伪代码:
//   1. 返回一个表示 null 的静态 JsonValue 引用(at() 缺键时复用)。
const JsonValue& nullValue() {
    static const JsonValue v = JsonValue::makeNull();
    return v;
}

// ---------------------------------------------------------------------------
// 递归下降解析器:持有原文与游标,失败时记录位置化错误
// ---------------------------------------------------------------------------
struct Parser {
    const std::string& text;
    std::size_t pos = 0;
    std::string* error = nullptr;

    explicit Parser(const std::string& t, std::string* e) : text(t), error(e) {}

    // 伪代码:
    //   1. 跳过空格/制表/换行/回车。
    void skipWs() {
        while (pos < text.size() &&
               (text[pos] == ' ' || text[pos] == '\t' ||
                text[pos] == '\n' || text[pos] == '\r')) {
            ++pos;
        }
    }

    // 伪代码:
    //   1. 若 error 指针有效且尚未记录错误,写入「位置 N: msg」。
    bool fail(const std::string& msg) {
        if (error != nullptr && error->empty()) {
            *error = "位置 " + std::to_string(pos) + ": " + msg;
        }
        return false;
    }

    // 伪代码:
    //   1. 检查剩余文本是否以前缀 s 开头,是则游标前移并返回 true。
    bool consume(const char* s) {
        const std::size_t len = std::string(s).size();
        if (text.compare(pos, len, s) == 0) {
            pos += len;
            return true;
        }
        return false;
    }

    // 伪代码(值分派):
    //   1. skipWs 后按当前字符分派:n/t/f 走字面量," 走字符串,
    //      [ 走数组,{ 走对象,其余尝试数字;
    //   2. 子例程失败则向上传播失败。
    bool parseValue(JsonValue& out) {
        skipWs();
        if (pos >= text.size()) {
            return fail("意外结束:期望一个 JSON 值");
        }
        const char c = text[pos];
        if (c == 'n') {
            if (!consume("null")) return fail("期望 null");
            out = JsonValue::makeNull();
            return true;
        }
        if (c == 't') {
            if (!consume("true")) return fail("期望 true");
            out = JsonValue::makeBool(true);
            return true;
        }
        if (c == 'f') {
            if (!consume("false")) return fail("期望 false");
            out = JsonValue::makeBool(false);
            return true;
        }
        if (c == '"') {
            std::string s;
            if (!parseString(s)) return false;
            out = JsonValue::makeString(s);
            return true;
        }
        if (c == '[') return parseArray(out);
        if (c == '{') return parseObject(out);
        return parseNumber(out);
    }

    // 伪代码(数字):
    //   1. 记录起点,接受可选负号、数字串、可选小数部分、可选指数部分;
    //   2. 没有任何数字字符则报错;
    //   3. 用 strtod 转换子串为 double。
    bool parseNumber(JsonValue& out) {
        const std::size_t start = pos;
        if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
        bool anyDigit = false;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
            anyDigit = true;
        }
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                ++pos;
                anyDigit = true;
            }
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) ++pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                ++pos;
                anyDigit = true;
            }
        }
        if (!anyDigit) {
            return fail("期望数字");
        }
        out = JsonValue::makeNumber(std::strtod(text.substr(start, pos - start).c_str(),
                                                nullptr));
        return true;
    }

    // 伪代码(字符串):
    //   1. 消费开引号;逐字符读取到闭引号;
    //   2. 反斜杠转义按表映射,\uXXXX 仅解码 ASCII 范围,其余 '?';
    //   3. 原文未闭合则报错。
    bool parseString(std::string& out) {
        ++pos;  // 跳过开引号
        out.clear();
        while (pos < text.size() && text[pos] != '"') {
            char c = text[pos++];
            if (c != '\\') {
                out.push_back(c);  // 含 UTF-8 原文透传
                continue;
            }
            if (pos >= text.size()) return fail("转义序列不完整");
            const char esc = text[pos++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 > text.size()) return fail("\\u 转义不完整");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text[pos++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code += static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code += static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code += static_cast<unsigned>(h - 'A' + 10);
                        else return fail("\\u 转义含非法十六进制字符");
                    }
                    out.push_back(code < 0x80 ? static_cast<char>(code) : '?');
                    break;
                }
                default: return fail("未知转义字符");
            }
        }
        if (pos >= text.size()) return fail("字符串未闭合");
        ++pos;  // 跳过闭引号
        return true;
    }

    // 伪代码(数组):
    //   1. 消费 '[';skipWs 后若即 ']' 则为空数组;
    //   2. 循环:parseValue 追加元素,skipWs 后 ',' 继续、']' 结束;
    //   3. 其余字符报错。
    bool parseArray(JsonValue& out) {
        ++pos;
        out = JsonValue::makeArray();
        skipWs();
        if (pos < text.size() && text[pos] == ']') {
            ++pos;
            return true;
        }
        while (true) {
            JsonValue elem;
            if (!parseValue(elem)) return false;
            out.push(std::move(elem));
            skipWs();
            if (pos >= text.size()) return fail("数组未闭合");
            if (text[pos] == ',') {
                ++pos;
                continue;
            }
            if (text[pos] == ']') {
                ++pos;
                return true;
            }
            return fail("数组中期望 ',' 或 ']'");
        }
    }

    // 伪代码(对象):
    //   1. 消费 '{';skipWs 后若即 '}' 则为空对象;
    //   2. 循环:解析键字符串、期望 ':'、parseValue,set 进对象;
    //   3. ',' 继续、'}' 结束,其余报错。
    bool parseObject(JsonValue& out) {
        ++pos;
        out = JsonValue::makeObject();
        skipWs();
        if (pos < text.size() && text[pos] == '}') {
            ++pos;
            return true;
        }
        while (true) {
            skipWs();
            if (pos >= text.size() || text[pos] != '"') {
                return fail("对象键必须是字符串");
            }
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos >= text.size() || text[pos] != ':') {
                return fail("对象键后期望 ':'");
            }
            ++pos;
            JsonValue value;
            if (!parseValue(value)) return false;
            out.set(key, std::move(value));
            skipWs();
            if (pos >= text.size()) return fail("对象未闭合");
            if (text[pos] == ',') {
                ++pos;
                continue;
            }
            if (text[pos] == '}') {
                ++pos;
                return true;
            }
            return fail("对象中期望 ',' 或 '}'");
        }
    }
};

// 伪代码(序列化辅助):
//   1. 把字符串内容按 JSON 规则转义(引号/反斜杠/控制字符),
//      用 \\u00XX 形式写控制字符,其余字节原样(UTF-8 透传)。
void escapeInto(const std::string& s, std::string& out) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

// 伪代码(递归序列化):
//   1. 按类型分派:null/bool/number 直接写字面量(number 用 %.17g 保精度);
//   2. string 走 escapeInto;
//   3. array/object:indent < 0 紧凑拼接;否则换行 + (depth+1)*indent 空格
//      展开,元素间逗号,收尾按 depth 缩进。
void dumpInto(const JsonValue& v, int indent, int depth, std::string& out) {
    switch (v.type()) {
        case JsonValue::Type::Null: out += "null"; return;
        case JsonValue::Type::Bool: out += (v.asBool() ? "true" : "false"); return;
        case JsonValue::Type::Number: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", v.asNumber());
            out += buf;
            return;
        }
        case JsonValue::Type::String:
            escapeInto(v.asString(), out);
            return;
        case JsonValue::Type::Array: {
            out.push_back('[');
            const bool pretty = indent >= 0 && !v.asArray().empty();
            for (std::size_t i = 0; i < v.asArray().size(); ++i) {
                if (i > 0) out.push_back(',');
                if (pretty) {
                    out.push_back('\n');
                    out.append(static_cast<std::size_t>((depth + 1) * indent), ' ');
                }
                dumpInto(v.asArray()[i], indent, depth + 1, out);
            }
            if (pretty) {
                out.push_back('\n');
                out.append(static_cast<std::size_t>(depth * indent), ' ');
            }
            out.push_back(']');
            return;
        }
        case JsonValue::Type::Object: {
            out.push_back('{');
            const bool pretty = indent >= 0 && !v.asObject().empty();
            for (std::size_t i = 0; i < v.asObject().size(); ++i) {
                if (i > 0) out.push_back(',');
                if (pretty) {
                    out.push_back('\n');
                    out.append(static_cast<std::size_t>((depth + 1) * indent), ' ');
                }
                escapeInto(v.asObject()[i].first, out);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                dumpInto(v.asObject()[i].second, indent, depth + 1, out);
            }
            if (pretty) {
                out.push_back('\n');
                out.append(static_cast<std::size_t>(depth * indent), ' ');
            }
            out.push_back('}');
            return;
        }
    }
}

}  // namespace

// 伪代码(工厂):按类型填充对应成员,其余保持零值。
JsonValue JsonValue::makeBool(bool v) {
    JsonValue j;
    j.type_ = Type::Bool;
    j.bool_ = v;
    return j;
}

JsonValue JsonValue::makeNumber(double v) {
    JsonValue j;
    j.type_ = Type::Number;
    j.number_ = v;
    return j;
}

JsonValue JsonValue::makeString(const std::string& v) {
    JsonValue j;
    j.type_ = Type::String;
    j.string_ = v;
    return j;
}

JsonValue JsonValue::makeArray(Array v) {
    JsonValue j;
    j.type_ = Type::Array;
    j.array_ = std::move(v);
    return j;
}

JsonValue JsonValue::makeObject(Object v) {
    JsonValue j;
    j.type_ = Type::Object;
    j.object_ = std::move(v);
    return j;
}

// 伪代码:
//   1. 把值移动到 array_ 末尾(调用方需保证类型为 Array)。
void JsonValue::push(JsonValue v) {
    array_.push_back(std::move(v));
}

// 伪代码:
//   1. 在 object_ 中线性查找同键项:找到则覆盖值;
//   2. 否则把 (key, value) 追加到末尾。
void JsonValue::set(const std::string& key, JsonValue v) {
    for (auto& [k, val] : object_) {
        if (k == key) {
            val = std::move(v);
            return;
        }
    }
    object_.emplace_back(key, std::move(v));
}

// 伪代码:
//   1. 线性查找键是否存在。
bool JsonValue::has(const std::string& key) const {
    for (const auto& [k, val] : object_) {
        if (k == key) {
            return true;
        }
    }
    return false;
}

// 伪代码:
//   1. 线性查找并返回值引用;不存在返回静态 null 值。
const JsonValue& JsonValue::at(const std::string& key) const {
    for (const auto& [k, val] : object_) {
        if (k == key) {
            return val;
        }
    }
    return nullValue();
}

// 伪代码:
//   1. 构造 Parser 并解析首个值;
//   2. 成功后 skipWs,若尚有非空白字符则报「末尾多余内容」;
//   3. 任一步失败返回空 optional。
std::optional<JsonValue> JsonValue::parse(const std::string& text, std::string* error) {
    Parser p(text, error);
    JsonValue out;
    if (!p.parseValue(out)) {
        return std::nullopt;
    }
    p.skipWs();
    if (p.pos != text.size()) {
        p.fail("末尾存在多余内容");
        return std::nullopt;
    }
    return out;
}

// 伪代码:
//   1. 调用 dumpInto 从深度 0 开始递归序列化,返回结果。
std::string JsonValue::dump(int indent) const {
    std::string out;
    dumpInto(*this, indent, 0, out);
    return out;
}

}  // namespace stmb
