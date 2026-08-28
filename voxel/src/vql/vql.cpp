// ============================================================================
// 文件: vql.cpp
// 模块: stmb_vql(VQL 查询语言,依赖 service)
// 用途: 实现 VQL 的词法分析、语法分析与执行。
// 设计思路:
//   1. 词法:数字(含负/小数/指数)、单引号字符串、标识符(关键字大小写
//      不敏感,统一转大写比较)、括号与逗号;
//   2. 语法:递归下降,按首 token 分派五种语句;可选子句用「期望-接受」
//      循环消费,遇到未知标识符即报「位置 N: 未知子句/关键字」;
//   3. 语义校验:STATE 值必须是 Pending|Stable|Changing,REGION/DURING
//      参数个数必须精确,错误信息携带位置与期望内容;
//   4. 执行:FindBlocks 按 level/summary 选择 query 路径,再应用
//      STATE/PAYLOAD/LIMIT 后过滤;AT 对每个命中改查 getAt 版本内容。
// 架构角色: VqlEngine 的唯一实现文件。
// ============================================================================
#include "vql.h"

#include "functions.h"

#include <cctype>
#include <cstdlib>
#include <utility>
#include <vector>

namespace stmb {
namespace {

// ---------------------------------------------------------------------------
// 词法单元
// ---------------------------------------------------------------------------
struct Token {
    enum class Kind { Number, String, Ident, LParen, RParen, Comma, Tilde, End };
    Kind kind = Kind::End;
    double number = 0.0;
    std::string text;     // String 内容或 Ident 原文
    std::size_t pos = 0;  // 在原文中的位置(报错用)
};

// 伪代码(词法器):
//   1. 逐字符扫描:空白跳过;'(' ')' ',' '~' 单字符 token;' 引号字符串;
//      字母/下划线开头读标识符;数字(可选负号/小数/指数)读 number;
//   2. 无法识别的字符产出 End 让语法层报错。
std::vector<Token> tokenize(const std::string& text) {
    std::vector<Token> tokens;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++pos;
            continue;
        }
        if (c == '(') { tokens.push_back(Token{Token::Kind::LParen, 0.0, "", pos++}); continue; }
        if (c == ')') { tokens.push_back(Token{Token::Kind::RParen, 0.0, "", pos++}); continue; }
        if (c == ',') { tokens.push_back(Token{Token::Kind::Comma, 0.0, "", pos++}); continue; }
        if (c == '~') { tokens.push_back(Token{Token::Kind::Tilde, 0.0, "", pos++}); continue; }
        if (c == '\'') {
            const std::size_t start = pos++;
            std::string s;
            while (pos < text.size() && text[pos] != '\'') {
                s.push_back(text[pos++]);
            }
            if (pos < text.size()) ++pos;  // 跳过闭引号(未闭合由语法层兜底)
            tokens.push_back(Token{Token::Kind::String, 0.0, s, start});
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const std::size_t start = pos;
            while (pos < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[pos])) || text[pos] == '_')) {
                ++pos;
            }
            tokens.push_back(Token{Token::Kind::Ident, 0.0, text.substr(start, pos - start), start});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '.' ||
            c == '+') {
            const std::size_t start = pos;
            if (c == '-' || c == '+') ++pos;
            while (pos < text.size() &&
                   (std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '.' ||
                    text[pos] == 'e' || text[pos] == 'E' || text[pos] == '-' || text[pos] == '+')) {
                ++pos;
            }
            tokens.push_back(Token{Token::Kind::Number,
                                   std::strtod(text.substr(start, pos - start).c_str(), nullptr),
                                   "", start});
            continue;
        }
        ++pos;  // 无法识别的字符:跳过(由 End/期望匹配兜底报错)
    }
    tokens.push_back(Token{Token::Kind::End, 0.0, "", text.size()});
    return tokens;
}

// 标识符转大写(关键字大小写不敏感)
std::string upperOf(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 语句结构
// ---------------------------------------------------------------------------
struct Statement {
    enum class Kind { FindBlocks, FindHistory, FindDynamic, FindTrajectory, Stats };
    Kind kind = Kind::Stats;
    AABB region{};
    TimeRange range{0, 0};
    int level = -1;                       // -1 = 缺省(最细层级)
    std::optional<BlockState> state;      // STATE 后过滤
    std::string payloadContains;          // PAYLOAD ~ 后过滤
    TimeStamp at = 0;
    bool hasAt = false;
    bool summary = false;
    std::size_t limit = 0;                // 0 = 不限
    BlockId blockId = 0;                  // HISTORY OF
    InstanceId instanceId = 0;            // TRAJECTORY OF
};

// ---------------------------------------------------------------------------
// 语法分析器
// ---------------------------------------------------------------------------
struct VqlParser {
    std::vector<Token> tokens;
    std::size_t pos = 0;

    explicit VqlParser(std::vector<Token> t) : tokens(std::move(t)) {}

    // 伪代码:
    //   1. 返回当前 token(越界时返回末尾 End token)。
    const Token& peek() const { return tokens[pos]; }

    // 伪代码:
    //   1. 返回当前 token 并前移游标。
    const Token& take() { return tokens[pos++]; }

    // 伪代码:
    //   1. 当前 token 是指定种类则消费并返回 true,否则返回 false。
    bool accept(Token::Kind kind) {
        if (peek().kind == kind) {
            ++pos;
            return true;
        }
        return false;
    }

    // 伪代码:
    //   1. 当前标识符(大写化后)等于 kw 则消费并返回 true。
    bool acceptKeyword(const char* kw) {
        if (peek().kind == Token::Kind::Ident && upperOf(peek().text) == kw) {
            ++pos;
            return true;
        }
        return false;
    }

    // 伪代码:
    //   1. 生成位置化错误:「位置 N: 期望 expected,得到 got」。
    static std::string errorAt(const Token& tok, const std::string& expected) {
        std::string got = "(结尾)";
        switch (tok.kind) {
            case Token::Kind::Number: got = "数字"; break;
            case Token::Kind::String: got = "字符串 '" + tok.text + "'"; break;
            case Token::Kind::Ident: got = "标识符 " + tok.text; break;
            case Token::Kind::LParen: got = "'('"; break;
            case Token::Kind::RParen: got = "')'"; break;
            case Token::Kind::Comma: got = "','"; break;
            case Token::Kind::Tilde: got = "'~'"; break;
            case Token::Kind::End: break;
        }
        return "位置 " + std::to_string(tok.pos) + ": 期望 " + expected +
               ",得到 " + got;
    }

    // 伪代码:
    //   1. 消费一个数字 token,失败返回 false 并填 error。
    bool readNumber(double& out, std::string& error) {
        if (peek().kind != Token::Kind::Number) {
            error = errorAt(peek(), "数字");
            return false;
        }
        out = take().number;
        return true;
    }

    // 伪代码:
    //   1. 消费一个指定种类 token,失败填 error。
    bool expect(Token::Kind kind, const std::string& expected, std::string& error) {
        if (peek().kind != kind) {
            error = errorAt(peek(), expected);
            return false;
        }
        ++pos;
        return true;
    }

    // 伪代码:
    //   1. 消费一个指定关键字,失败填 error。
    bool expectKeyword(const char* kw, std::string& error) {
        if (!acceptKeyword(kw)) {
            error = errorAt(peek(), std::string("关键字 ") + kw);
            return false;
        }
        return true;
    }

    // 伪代码:
    //   1. 解析 '(' 数字 (',' 数字)* ')' 到 numbers;
    //   2. 参数个数不符 expectedCount 时给出语义错误。
    bool readNumberList(std::size_t expectedCount, std::vector<double>& numbers,
                        const std::string& what, std::string& error) {
        numbers.clear();
        if (!expect(Token::Kind::LParen, "'('", error)) return false;
        while (true) {
            double v = 0.0;
            if (!readNumber(v, error)) return false;
            numbers.push_back(v);
            if (accept(Token::Kind::Comma)) continue;
            if (accept(Token::Kind::RParen)) break;
            error = errorAt(peek(), "',' 或 ')'");
            return false;
        }
        if (numbers.size() != expectedCount) {
            error = "语义错误: " + what + " 需要 " + std::to_string(expectedCount) +
                    " 个数字,得到 " + std::to_string(numbers.size()) + " 个";
            return false;
        }
        return true;
    }

    // 伪代码:
    //   1. 解析 REGION(...)/DURING(...) 子句到 stmt;
    //   2. 任一失败返回 false。
    bool readRegionAndRange(Statement& stmt, std::string& error) {
        if (!expectKeyword("REGION", error)) return false;
        std::vector<double> nums;
        if (!readNumberList(6, nums, "REGION", error)) return false;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            stmt.region.min[axis] = nums[axis];
            stmt.region.max[axis] = nums[axis + 3];
        }
        if (!expectKeyword("DURING", error)) return false;
        if (!readNumberList(2, nums, "DURING", error)) return false;
        stmt.range.start = static_cast<TimeStamp>(nums[0]);
        stmt.range.end = static_cast<TimeStamp>(nums[1]);
        return true;
    }

    // 伪代码(语句分派):
    //   1. STATS 直接返回;
    //   2. FIND:第二关键字分派 BLOCKS/HISTORY/DYNAMIC/TRAJECTORY;
    //   3. BLOCKS:IN REGION(...) DURING(...) 后循环消费可选子句
    //      LEVEL/STATE/PAYLOAD/AT/SUMMARY/LIMIT,遇到 End 收尾;
    //   4. HISTORY/TRAJECTORY:OF + 数字 id;DYNAMIC:IN REGION + DURING;
    //   5. 末尾要求 End token。
    bool parse(Statement& stmt, std::string& error) {
        if (acceptKeyword("STATS")) {
            stmt.kind = Statement::Kind::Stats;
        } else if (acceptKeyword("FIND")) {
            if (acceptKeyword("BLOCKS")) {
                stmt.kind = Statement::Kind::FindBlocks;
                if (!expectKeyword("IN", error)) return false;
                if (!readRegionAndRange(stmt, error)) return false;
                while (peek().kind != Token::Kind::End) {
                    if (acceptKeyword("LEVEL")) {
                        double v = 0.0;
                        if (!readNumber(v, error)) return false;
                        stmt.level = static_cast<int>(v);
                    } else if (acceptKeyword("STATE")) {
                        if (peek().kind != Token::Kind::Ident) {
                            error = errorAt(peek(), "Pending|Stable|Changing");
                            return false;
                        }
                        const std::string value = upperOf(take().text);
                        if (value == "PENDING") stmt.state = BlockState::Pending;
                        else if (value == "STABLE") stmt.state = BlockState::Stable;
                        else if (value == "CHANGING") stmt.state = BlockState::Changing;
                        else {
                            error = "语义错误: 非法 STATE 值 '" + value +
                                    "',期望 Pending|Stable|Changing";
                            return false;
                        }
                    } else if (acceptKeyword("PAYLOAD")) {
                        if (!expect(Token::Kind::Tilde, "'~'", error)) return false;
                        if (peek().kind != Token::Kind::String) {
                            error = errorAt(peek(), "字符串");
                            return false;
                        }
                        stmt.payloadContains = take().text;
                    } else if (acceptKeyword("AT")) {
                        double v = 0.0;
                        if (!readNumber(v, error)) return false;
                        stmt.at = static_cast<TimeStamp>(v);
                        stmt.hasAt = true;
                    } else if (acceptKeyword("SUMMARY")) {
                        stmt.summary = true;
                    } else if (acceptKeyword("LIMIT")) {
                        double v = 0.0;
                        if (!readNumber(v, error)) return false;
                        stmt.limit = static_cast<std::size_t>(v);
                    } else {
                        error = errorAt(peek(),
                                        "LEVEL|STATE|PAYLOAD|AT|SUMMARY|LIMIT 或语句结束");
                        return false;
                    }
                }
            } else if (acceptKeyword("HISTORY")) {
                stmt.kind = Statement::Kind::FindHistory;
                if (!expectKeyword("OF", error)) return false;
                double v = 0.0;
                if (!readNumber(v, error)) return false;
                stmt.blockId = static_cast<BlockId>(v);
            } else if (acceptKeyword("DYNAMIC")) {
                stmt.kind = Statement::Kind::FindDynamic;
                if (!expectKeyword("IN", error)) return false;
                if (!readRegionAndRange(stmt, error)) return false;
            } else if (acceptKeyword("TRAJECTORY")) {
                stmt.kind = Statement::Kind::FindTrajectory;
                if (!expectKeyword("OF", error)) return false;
                double v = 0.0;
                if (!readNumber(v, error)) return false;
                stmt.instanceId = static_cast<InstanceId>(v);
            } else {
                error = errorAt(peek(), "BLOCKS|HISTORY|DYNAMIC|TRAJECTORY");
                return false;
            }
        } else {
            error = errorAt(peek(), "关键字 FIND 或 STATS");
            return false;
        }
        if (peek().kind != Token::Kind::End) {
            error = errorAt(peek(), "语句结束");
            return false;
        }
        return true;
    }
};

}  // namespace

// 伪代码:
//   1. 词法 + 语法分析,失败返回 {ok:false, 位置化错误};
//   2. 按语句类型执行:
//      - Stats:组装 stats 对象;
//      - FindBlocks:SUMMARY 走 queryCoarseToFine,否则按 level 选择
//        query/queryLevel;再应用 STATE/PAYLOAD~/AT/LIMIT 后过滤,
//        AT 时改用 getAt 的版本 payload/confidence 填充结果;
//      - FindHistory:historyOf 序列化为版本数组;
//      - FindDynamic:queryDynamic 序列化为实例数组;
//      - FindTrajectory:trajectoryOf 序列化为轨迹点数组;
//   3. 返回 {ok:true, data}。
VqlResult VqlEngine::execute(const std::string& text) {
    VqlParser parser(tokenize(text));
    Statement stmt;
    std::string error;
    if (!parser.parse(stmt, error)) {
        return VqlResult{false, error, JsonValue::makeNull()};
    }
    VqlResult result;
    result.ok = true;

    switch (stmt.kind) {
        case Statement::Kind::Stats: {
            const ServiceStats s = service_.stats();
            const DynamicStats ds = service_.dynamicStats();
            JsonValue data = JsonValue::makeObject();
            data.set("blocks", JsonValue::makeNumber(static_cast<double>(s.blockCount)));
            data.set("capacity", JsonValue::makeNumber(static_cast<double>(s.capacity)));
            data.set("evictions",
                     JsonValue::makeNumber(static_cast<double>(s.evictCount)));
            data.set("loaded_shards",
                     JsonValue::makeNumber(static_cast<double>(s.loadedShards)));
            data.set("dynamic_active",
                     JsonValue::makeNumber(static_cast<double>(ds.activeCount)));
            data.set("dynamic_stationary",
                     JsonValue::makeNumber(static_cast<double>(ds.stationaryCount)));
            data.set("dynamic_archived",
                     JsonValue::makeNumber(static_cast<double>(ds.archivedCount)));
            result.data = data;
            return result;
        }
        case Statement::Kind::FindBlocks: {
            std::vector<MemoryBlock> hits;
            if (stmt.summary) {
                hits = service_.queryCoarseToFine(stmt.region, stmt.range);
            } else if (stmt.level >= 0) {
                hits = service_.queryLevel(stmt.region, stmt.range, stmt.level);
            } else {
                hits = service_.query(stmt.region, stmt.range);
            }
            JsonValue blocks = JsonValue::makeArray();
            for (const MemoryBlock& b : hits) {
                if (stmt.state.has_value() && b.state != *stmt.state) {
                    continue;
                }
                if (!stmt.payloadContains.empty() &&
                    b.payload.find(stmt.payloadContains) == std::string::npos) {
                    continue;
                }
                if (stmt.hasAt) {
                    // 时间回溯:改用 t 时刻生效的版本内容
                    const std::optional<BlockVersion> v = service_.getAt(b.id, stmt.at);
                    if (!v.has_value()) {
                        continue;  // 该时刻尚无此块的版本记录
                    }
                    MemoryBlock past = b;
                    past.payload = v->payload;
                    past.confidence = v->confidence;
                    past.version = v->version;
                    past.state = v->state;
                    blocks.push(blockToJson(past));
                } else {
                    blocks.push(blockToJson(b));
                }
                if (stmt.limit > 0 && blocks.asArray().size() >= stmt.limit) {
                    break;
                }
            }
            result.data = blocks;
            return result;
        }
        case Statement::Kind::FindHistory: {
            JsonValue versions = JsonValue::makeArray();
            for (const BlockVersion& v : service_.historyOf(stmt.blockId)) {
                versions.push(versionToJson(v));
            }
            result.data = versions;
            return result;
        }
        case Statement::Kind::FindDynamic: {
            JsonValue instances = JsonValue::makeArray();
            for (const DynamicInstance& inst :
                 service_.queryDynamic(stmt.region, stmt.range)) {
                instances.push(instanceToJson(inst));
            }
            result.data = instances;
            return result;
        }
        case Statement::Kind::FindTrajectory: {
            JsonValue points = JsonValue::makeArray();
            for (const TrackPoint& p : service_.trajectoryOf(stmt.instanceId)) {
                JsonValue point = JsonValue::makeObject();
                point.set("t", JsonValue::makeNumber(static_cast<double>(p.t)));
                JsonValue pos = JsonValue::makeArray();
                JsonValue vel = JsonValue::makeArray();
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    pos.push(JsonValue::makeNumber(p.position[axis]));
                    vel.push(JsonValue::makeNumber(p.velocity[axis]));
                }
                point.set("position", pos);
                point.set("velocity", vel);
                points.push(point);
            }
            result.data = points;
            return result;
        }
    }
    return VqlResult{false, "未实现的语句类型", JsonValue::makeNull()};
}

}  // namespace stmb
