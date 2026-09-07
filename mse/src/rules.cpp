// ============================================================================
// mse/rules.cpp —— 数据集合 ③ 规则集 + 无环境沙盒求值器(实现)
//
// 规则 = 属性谓词(deps)+ 确定性纯函数(JSON-logic 方言)。
// 硬约束:无环境——本文件不含时钟/随机/IO;全部输入由参数注入,天然确定性。
// ============================================================================

#include <functional>
#include <stdexcept>

#include "mse/rules.h"
#include "mse/wasm_sandbox.h"

#include <cmath>
#include <set>
#include <utility>

namespace mse {

// ---- Rule JSON 序列化(定义事件负载形态,snake_case) ----
void to_json(json& j, const Rule& r) {
    j = json{
        {"rule_id",       r.rule_id},
        {"deps",          r.deps},
        {"effect",        r.effect},
        {"target_key",    r.target_key},
        {"logic",         r.logic},
        {"consumers",     r.consumers},
        {"on_types",      r.on_types},
        {"runtime",       r.runtime},
        {"artifact",      r.artifact},
        {"export_name",   r.export_name},
        {"artifact_hash", r.artifact_hash},
        {"engine_version", r.engine_version},
        {"status",        r.status},
        {"version",       r.version},
        {"registered_by", r.registered_by},
    };
}

void from_json(const json& j, Rule& r) {
    j.at("rule_id").get_to(r.rule_id);
    j.at("deps").get_to(r.deps);
    j.at("effect").get_to(r.effect);
    r.target_key     = j.value("target_key", std::string{});
    r.logic          = j.value("logic", json(nullptr));
    r.consumers      = j.value("consumers", std::string{"both"});
    r.on_types       = j.value("on_types", std::vector<std::string>{});
    r.runtime        = j.value("runtime", std::string{"jsonlogic"});
    r.artifact       = j.value("artifact", std::string{});
    r.export_name    = j.value("export_name", std::string{"mse_eval"});
    r.artifact_hash  = j.value("artifact_hash", std::string{});
    r.engine_version = j.value("engine_version", std::string{});
    r.status         = j.value("status", std::string{"active"});
    r.version        = j.value("version", int64_t{1});
    r.registered_by  = j.value("registered_by", int64_t{0});
}

// ---- RuleOutcome 静态工厂 ----
RuleOutcome RuleOutcome::pass() {
    RuleOutcome o;
    o.kind = Kind::kPass;
    return o;
}
RuleOutcome RuleOutcome::reject(std::string reason) {
    RuleOutcome o;
    o.kind   = Kind::kReject;
    o.reason = std::move(reason);
    return o;
}
RuleOutcome RuleOutcome::value_of(json v) {
    RuleOutcome o;
    o.kind  = Kind::kValue;
    o.value = std::move(v);
    return o;
}
RuleOutcome RuleOutcome::emit(std::vector<Candidate> cs) {
    RuleOutcome o;
    o.kind    = Kind::kEmit;
    o.emitted = std::move(cs);
    return o;
}
RuleOutcome RuleOutcome::noop() {
    RuleOutcome o;
    o.kind = Kind::kNoop;
    return o;
}

namespace {

// 方言全部算子名(单键 object 的键命中即视为表达式,否则为裸值)
const std::set<std::string>& operator_names() {
    static const std::set<std::string> ops = {
        "var", "cand", "write",
        "==", "!=", "<", "<=", ">", ">=",
        "+", "-", "*", "/", "%",
        "and", "or", "!", "in", "cat", "count", "if",
        "pass", "reject", "return", "emit", "noop",
    };
    return ops;
}

bool is_operator_object(const json& v) {
    return v.is_object() && v.size() == 1 && operator_names().count(v.begin().key()) != 0;
}

bool is_terminal_op(const std::string& op) {
    return op == "pass" || op == "reject" || op == "return" || op == "emit" || op == "noop";
}

// 真值判断:false / null / 0 / "" 为假,其余(含空数组空对象)为真
bool is_truthy(const json& v) {
    if (v.is_null())    return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number())  return v.get<double>() != 0.0;
    if (v.is_string())  return !v.get<std::string>().empty();
    return true;
}

// json 相等:数字统一按 double 比较(1 与 1.0 相等)
bool json_equal(const json& a, const json& b) {
    if (a.is_number() && b.is_number()) return a.get<double>() == b.get<double>();
    return a == b;
}

// 拼接语义:字符串原样,其余 json 序列化
std::string str_of(const json& v) {
    if (v.is_string())  return v.get<std::string>();
    if (v.is_null())    return "null";
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return v.dump();
}

} // namespace

// ----------------------------------------------------------------------------
// static_check:烘焙管线的"编译期"环节
// ----------------------------------------------------------------------------
std::vector<std::string> RuleEngine::static_check(
    const Rule& r, const std::function<bool(const std::string&)>& is_registered) {
    std::vector<std::string> problems;

    // ① deps 每个键已登记
    for (const auto& d : r.deps) {
        if (!is_registered(d)) problems.push_back("deps 引用未登记键: " + d);
    }
    const std::set<std::string> dep_set(r.deps.begin(), r.deps.end());

    // ④ effect 合法;derive 须带已登记 target_key
    if (r.effect != "filter" && r.effect != "derive" && r.effect != "trigger") {
        problems.push_back("effect 非法(须为 filter/derive/trigger): " + r.effect);
    }
    if (r.effect == "derive") {
        if (r.target_key.empty()) {
            problems.push_back("derive 规则缺少 target_key");
        } else if (!is_registered(r.target_key)) {
            problems.push_back("target_key 未登记: " + r.target_key);
        }
    }

    // ②③ 递归遍历 logic 树:var 只引用 deps 内键;算子参数个数/类型合法。
    // wasm 规则无 logic(求值委托给沙盒产物),跳过 logic 形状检查。
    if (r.runtime == "wasm") return problems;
    std::function<void(const json&)> walk = [&](const json& expr) {
        if (expr.is_array()) {
            for (const auto& e : expr) walk(e);
            return;
        }
        if (!expr.is_object()) return;  // 裸标量
        if (!is_operator_object(expr)) {
            for (const auto& [k, v] : expr.items()) walk(v);  // 裸 object:递归值
            return;
        }
        const std::string& op  = expr.begin().key();
        const json&        arg = expr.begin().value();

        auto need_string = [&]() -> bool {
            if (!arg.is_string()) {
                problems.push_back("算子 " + op + " 的参数须为字符串键");
                return false;
            }
            return true;
        };
        auto walk_args = [&](size_t lo, size_t hi, bool odd) {
            if (!arg.is_array()) {
                problems.push_back("算子 " + op + " 的参数须为数组");
                return;
            }
            if (arg.size() < lo || arg.size() > hi || (odd && arg.size() % 2 == 0)) {
                problems.push_back("算子 " + op + " 的参数个数非法(" +
                                   std::to_string(arg.size()) + " 个)");
                return;
            }
            for (const auto& e : arg) walk(e);
        };

        if (op == "var") {
            if (need_string() && dep_set.count(arg.get<std::string>()) == 0) {
                problems.push_back("var 引用 deps 未声明的键: " + arg.get<std::string>());
            }
        } else if (op == "cand" || op == "write") {
            need_string();
        } else if (op == "==" || op == "!=" || op == "<" || op == "<=" ||
                   op == ">" || op == ">=" || op == "/" || op == "%" || op == "in") {
            walk_args(2, 2, false);
        } else if (op == "+" || op == "-" || op == "*" ||
                   op == "and" || op == "or" || op == "cat") {
            walk_args(1, static_cast<size_t>(-1), false);
        } else if (op == "if") {
            walk_args(3, static_cast<size_t>(-1), true);
        } else if (op == "!" || op == "count" || op == "reject" || op == "return") {
            walk(arg);
        } else if (op == "pass" || op == "noop") {
            // 参数任意,不递归
        } else if (op == "emit") {
            if (!arg.is_object()) {
                problems.push_back("emit 载荷须为 object");
            } else {
                for (const auto& [k, v] : arg.items()) walk(v);
            }
        }
    };
    walk(r.logic);

    return problems;
}

// ----------------------------------------------------------------------------
// eval_expr:私有递归求值器(无环境沙盒)
// ----------------------------------------------------------------------------
json RuleEngine::eval_expr(const json& expr, const json& attrs, const Candidate& cand,
                           const std::string& target_id) const {
    if (!is_operator_object(expr)) return expr;  // 裸值原样返回

    const std::string& op  = expr.begin().key();
    const json&        arg = expr.begin().value();

    auto need_args = [&](size_t n) {
        if (!arg.is_array() || arg.size() != n) {
            throw RuleError("算子 " + op + " 需要 " + std::to_string(n) + " 个参数");
        }
    };

    // ---- 取值 ----
    if (op == "var" || op == "cand" || op == "write") {
        if (!arg.is_string()) throw RuleError("算子 " + op + " 的参数须为字符串键");
        const std::string k = arg.get<std::string>();
        if (op == "var") {
            if (attrs.is_object() && attrs.contains(k)) return attrs.at(k);
            return nullptr;  // 缺失 → null
        }
        if (op == "cand") {
            if (k == "__target")    return target_id;
            if (k == "type")        return cand.type;
            if (k == "actor")       return cand.actor;
            if (k == "occur_time")  return cand.occur_time;
            if (k == "evidence")    return cand.evidence ? *cand.evidence : json(nullptr);
            if (k == "corrects")    return cand.corrects ? json(*cand.corrects) : json(nullptr);
            throw RuleError("未知 candidate 顶层键: " + k);
        }
        // write:当前求值目标本体的变化集
        auto wit = cand.writes.find(target_id);
        if (wit == cand.writes.end() || !wit->second.is_object() || !wit->second.contains(k)) {
            return nullptr;
        }
        return wit->second.at(k);
    }

    // ---- 相等/不等:json 相等(数字按 double) ----
    if (op == "==" || op == "!=") {
        need_args(2);
        const json a = eval_expr(arg[0], attrs, cand, target_id);
        const json b = eval_expr(arg[1], attrs, cand, target_id);
        const bool eq = json_equal(a, b);
        return op == "==" ? eq : !eq;
    }

    // ---- 大小比较:number 或 string,类型不符抛 RuleError ----
    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
        need_args(2);
        const json a = eval_expr(arg[0], attrs, cand, target_id);
        const json b = eval_expr(arg[1], attrs, cand, target_id);
        int cmp = 0;
        if (a.is_number() && b.is_number()) {
            const double x = a.get<double>(), y = b.get<double>();
            cmp = (x < y) ? -1 : (x > y ? 1 : 0);
        } else if (a.is_string() && b.is_string()) {
            const std::string x = a.get<std::string>(), y = b.get<std::string>();
            cmp = (x < y) ? -1 : (x > y ? 1 : 0);
        } else {
            throw RuleError("比较算子 " + op + " 的操作数须同为 number 或同为 string");
        }
        if (op == "<")  return cmp < 0;
        if (op == "<=") return cmp <= 0;
        if (op == ">")  return cmp > 0;
        return cmp >= 0;
    }

    // ---- 算术 + - *(number;+ 允许字符串拼接) ----
    if (op == "+" || op == "-" || op == "*") {
        if (!arg.is_array() || arg.empty()) {
            throw RuleError("算子 " + op + " 需要至少 1 个参数");
        }
        std::vector<json> vals;
        vals.reserve(arg.size());
        for (const auto& e : arg) vals.push_back(eval_expr(e, attrs, cand, target_id));

        if (op == "+") {
            bool any_str = false;
            for (const auto& v : vals) any_str = any_str || v.is_string();
            if (any_str) {
                std::string s;
                for (const auto& v : vals) s += str_of(v);
                return s;
            }
        }
        bool   all_int = true;
        double acc = 0.0;
        for (size_t i = 0; i < vals.size(); ++i) {
            // null 操作数按 0 处理:计数器类派生规则的初值语义——首个事件结算时
            // 派生键尚不存在,var 取到 null,"旧值 + 1" 应从 0 起步而非报错。
            // 其余类型错误仍抛 RuleError。
            if (vals[i].is_null()) vals[i] = 0;
            if (!vals[i].is_number()) {
                throw RuleError("算子 " + op + " 的操作数须为 number");
            }
            if (!vals[i].is_number_integer() && !vals[i].is_number_unsigned()) all_int = false;
            const double d = vals[i].get<double>();
            if (i == 0) acc = d;
            else if (op == "+") acc += d;
            else if (op == "-") acc -= d;
            else acc *= d;
        }
        if (op == "-" && vals.size() == 1) acc = -acc;  // 单参 = 取负
        if (all_int) return static_cast<int64_t>(acc);
        return acc;
    }

    // ---- 算术 / %(除零/模零抛 RuleError;null 操作数按 0 处理,同上初值语义) ----
    if (op == "/" || op == "%") {
        need_args(2);
        json a = eval_expr(arg[0], attrs, cand, target_id);
        json b = eval_expr(arg[1], attrs, cand, target_id);
        if (a.is_null()) a = 0;
        if (b.is_null()) b = 0;
        if (!a.is_number() || !b.is_number()) {
            throw RuleError("算子 " + op + " 的操作数须为 number");
        }
        const double dv = b.get<double>();
        if (dv == 0.0) throw RuleError(op == "/" ? "除零" : "模零");
        if (op == "/") return a.get<double>() / dv;
        return static_cast<int64_t>(a.get<double>()) % static_cast<int64_t>(dv);
    }

    // ---- and/or 短路(返回操作数值,与 JSON-logic 一致) ----
    if (op == "and" || op == "or") {
        if (!arg.is_array() || arg.empty()) {
            throw RuleError("算子 " + op + " 需要至少 1 个参数");
        }
        json last;
        for (const auto& e : arg) {
            last = eval_expr(e, attrs, cand, target_id);
            const bool t = is_truthy(last);
            if (op == "and" && !t) return last;
            if (op == "or"  &&  t) return last;
        }
        return last;
    }

    // ---- ! 取反 ----
    if (op == "!") {
        return !is_truthy(eval_expr(arg, attrs, cand, target_id));
    }

    // ---- in:v ∈ 数组(json 相等) ----
    if (op == "in") {
        need_args(2);
        const json v   = eval_expr(arg[0], attrs, cand, target_id);
        const json arr = eval_expr(arg[1], attrs, cand, target_id);
        if (!arr.is_array()) throw RuleError("in 的第二参数须为数组");
        for (const auto& e : arr) {
            if (json_equal(v, e)) return true;
        }
        return false;
    }

    // ---- cat:拼接为字符串 ----
    if (op == "cat") {
        if (!arg.is_array()) throw RuleError("cat 的参数须为数组");
        std::string s;
        for (const auto& e : arg) s += str_of(eval_expr(e, attrs, cand, target_id));
        return s;
    }

    // ---- count:数组/对象长度 ----
    if (op == "count") {
        const json v = eval_expr(arg, attrs, cand, target_id);
        if (v.is_array() || v.is_object()) return static_cast<int64_t>(v.size());
        throw RuleError("count 的操作数须为数组或对象");
    }

    // ---- if:[c,t,e] 或链式 [c1,t1,...,else](奇数个参数) ----
    if (op == "if") {
        if (!arg.is_array() || arg.size() < 3 || arg.size() % 2 == 0) {
            throw RuleError("if 需要奇数个参数且不少于 3 个");
        }
        size_t i = 0;
        while (i + 1 < arg.size()) {
            if (is_truthy(eval_expr(arg[i], attrs, cand, target_id))) {
                return eval_expr(arg[i + 1], attrs, cand, target_id);
            }
            i += 2;
        }
        return eval_expr(arg[i], attrs, cand, target_id);  // else 分支
    }

    // ---- 终态算子不能作为子表达式 ----
    if (is_terminal_op(op)) {
        throw RuleError("终态算子 " + op + " 不能作为子表达式");
    }

    throw RuleError("未知算子: " + op);
}

// ----------------------------------------------------------------------------
// eval:终态处理 + effect 种类校验
// ----------------------------------------------------------------------------
RuleOutcome RuleEngine::eval(const Rule& r, const json& attrs, const Candidate& candidate,
                             const std::string& target_id) const {
    // wasm 运行时:整个求值委托给 WASM 沙盒(见 wasm_sandbox.h 的 ABI)
    if (r.runtime == "wasm") {
        return WasmSandbox().eval(r, attrs, candidate, target_id);
    }
    // 顶层 if 链先归约:求值条件落到选中分支(分支可再嵌套 if),最终须落在
    // 终态算子(pass/reject/return/emit/noop)或裸表达式(仅 derive)上。
    const json* logic_p = &r.logic;
    while (is_operator_object(*logic_p) && logic_p->begin().key() == "if") {
        const json& arg = logic_p->begin().value();
        if (!arg.is_array() || arg.size() < 3 || arg.size() % 2 == 0) {
            throw RuleError("if 需要奇数个参数且不少于 3 个");
        }
        size_t i = 0;
        bool   chosen = false;
        while (i + 1 < arg.size()) {
            if (is_truthy(eval_expr(arg[i], attrs, candidate, target_id))) {
                logic_p = &arg[i + 1];
                chosen  = true;
                break;
            }
            i += 2;
        }
        if (!chosen) logic_p = &arg[arg.size() - 1];  // else 分支
    }
    const json& logic = *logic_p;
    RuleOutcome out;

    bool handled = false;
    if (is_operator_object(logic)) {
        const std::string& op  = logic.begin().key();
        const json&        arg = logic.begin().value();

        if (op == "pass") {
            out = RuleOutcome::pass();
            handled = true;
        } else if (op == "reject") {
            const json reason = eval_expr(arg, attrs, candidate, target_id);
            out = RuleOutcome::reject(reason.is_string() ? reason.get<std::string>()
                                                         : reason.dump());
            handled = true;
        } else if (op == "return") {
            out = RuleOutcome::value_of(eval_expr(arg, attrs, candidate, target_id));
            handled = true;
        } else if (op == "noop") {
            out = RuleOutcome::noop();
            handled = true;
        } else if (op == "emit") {
            if (!arg.is_object()) throw RuleError("emit 载荷须为 object");
            // 载荷中的嵌套表达式(单键算子 object)先递归求值,得到扁平载荷
            std::function<json(const json&)> deep = [&](const json& v) -> json {
                if (is_operator_object(v)) {
                    const std::string& k = v.begin().key();
                    if (is_terminal_op(k)) {
                        throw RuleError("emit 载荷中不允许嵌套终态算子: " + k);
                    }
                    return eval_expr(v, attrs, candidate, target_id);
                }
                if (v.is_object()) {
                    json o = json::object();
                    for (const auto& [key, val] : v.items()) o[key] = deep(val);
                    return o;
                }
                if (v.is_array()) {
                    json a = json::array();
                    for (const auto& e : v) a.push_back(deep(e));
                    return a;
                }
                return v;
            };
            const json flat = deep(arg);

            // 组装候选:type 必填
            if (!flat.contains("type") || !flat["type"].is_string() ||
                flat["type"].get<std::string>().empty()) {
                throw RuleError("emit 载荷缺少 type(非空字符串)");
            }
            static const std::set<std::string> kSysKeys = {
                "type", "id", "actor", "writes", "occur_time", "space",
                "evidence", "corrects", "idempotency_key",
            };
            Candidate c;
            c.type = flat["type"].get<std::string>();
            // "writes"(object)与 "id"(字符串)二选一或并存:有 writes 直接用
            if (flat.contains("writes")) {
                if (!flat["writes"].is_object()) throw RuleError("emit 载荷 writes 须为 object");
                for (const auto& [wid, wobj] : flat["writes"].items()) c.writes[wid] = wobj;
            }
            // 有 id:其余非系统键组成(或并入)writes[id]
            if (flat.contains("id")) {
                if (!flat["id"].is_string()) throw RuleError("emit 载荷 id 须为字符串");
                const std::string wid = flat["id"].get<std::string>();
                json w = json::object();
                auto   it = c.writes.find(wid);
                if (it != c.writes.end() && it->second.is_object()) w = it->second;
                for (const auto& [k, v] : flat.items()) {
                    if (kSysKeys.count(k) == 0) w[k] = v;
                }
                c.writes[wid] = std::move(w);
            }
            if (c.writes.empty()) throw RuleError("emit 载荷须含 id 或 writes 之一");
            // actor 缺省 "rule:<rule_id>";space/occur_time 留默认,由管线继承父候选
            c.actor = (flat.contains("actor") && flat["actor"].is_string() &&
                       !flat["actor"].get<std::string>().empty())
                          ? flat["actor"].get<std::string>()
                          : ("rule:" + r.rule_id);
            out = RuleOutcome::emit({std::move(c)});
            handled = true;
        }
    }

    if (!handled) {
        // 裸表达式:仅 derive 允许,求值结果即派生值
        if (r.effect != "derive") {
            throw RuleError("effect=" + r.effect + " 的规则须以终态算子收尾");
        }
        out = RuleOutcome::value_of(eval_expr(logic, attrs, candidate, target_id));
    }

    // effect 终态种类校验
    using K = RuleOutcome::Kind;
    if (r.effect == "filter" && out.kind != K::kPass && out.kind != K::kReject) {
        throw RuleError("filter 规则结果须为 pass/reject");
    }
    if (r.effect == "derive" && out.kind != K::kValue) {
        throw RuleError("derive 规则结果须为 return/裸表达式");
    }
    if (r.effect == "trigger" && out.kind != K::kEmit && out.kind != K::kNoop) {
        throw RuleError("trigger 规则结果须为 emit/noop");
    }
    return out;
}

// ----------------------------------------------------------------------------
// match:按属性谓词匹配(std::map 迭代序即 rule_id 字典序,确定性)
// ----------------------------------------------------------------------------
std::vector<const Rule*> RuleEngine::match(const std::set<std::string>& owned_keys,
                                           const std::map<std::string, Rule>& rules,
                                           const std::string& effect_filter) const {
    std::vector<const Rule*> out;
    for (const auto& [id, rule] : rules) {
        if (rule.status != "active") continue;
        if (!effect_filter.empty() && rule.effect != effect_filter) continue;
        bool applicable = true;
        for (const auto& d : rule.deps) {
            if (owned_keys.count(d) == 0) { applicable = false; break; }
        }
        if (applicable) out.push_back(&rule);
    }
    return out;
}

// ----------------------------------------------------------------------------
// eval_filters:过滤规则族求值(写侧 L3 与读侧按钮可用性共用)
// ----------------------------------------------------------------------------
std::vector<std::string> RuleEngine::eval_filters(
    const std::vector<std::string>& rule_refs,
    const std::map<std::string, Rule>& rules,
    const std::map<std::string, json>& attrs_per_target,
    const Candidate& candidate, const std::string& side) const {
    std::vector<std::string> violations;

    for (const auto& ref : rule_refs) {
        auto rit = rules.find(ref);
        if (rit == rules.end()) continue;  // 定义结算时已强校验,缺失则跳过
        const Rule& rule = rit->second;
        if (rule.effect != "filter") continue;
        // 消费侧匹配:写侧(L3/按钮)评 write/both;读侧(行过滤)评 read/both
        if (rule.consumers != side && rule.consumers != "both") continue;

        for (const auto& [target, attrs] : attrs_per_target) {
            // 适用性:
            //  - 读侧行过滤(side=="read"):本体须聚合 deps 全部键才适用
            //    (缺键的行不受该规则约束,保持可见);
            //  - 写侧(L3/按钮):类型显式引用的规则一律求值,缺失的 deps 键
            //    注入 null——"初始状态"分支(如状态机的"无状态只能进 01")
            //    就是写在 null 上的;读写两侧同一口径,可点 ⇔ 可结算。
            bool applicable = attrs.is_object();
            for (const auto& d : rule.deps) {
                if (!applicable || !attrs.contains(d)) { applicable = false; break; }
            }
            if (!applicable && side == "read") continue;

            // 最小权限:只注入 deps 声明的键(缺失注入 null)
            json injected = json::object();
            for (const auto& d : rule.deps)
                injected[d] =
                    (attrs.is_object() && attrs.contains(d)) ? attrs[d] : json(nullptr);

            // fail-closed:规则执行错误(定义层 bug)按拒绝处理,绝不上炸成 500
            RuleOutcome out;
            try {
                out = eval(rule, injected, candidate, target);
            } catch (const RuleError& e) {
                violations.push_back(rule.rule_id + ":规则执行错误(fail-closed): " + e.what());
                continue;
            }
            if (out.kind == RuleOutcome::Kind::kReject) {
                violations.push_back(rule.rule_id + ":" + out.reason);
            }
        }
    }
    return violations;
}

} // namespace mse
