#pragma once

// ============================================================================
// mse/rules.h —— 数据集合 ③ 规则集 + 无环境沙盒求值器
//
// 依据《车间世界模型_系统实现方案》§3.3 与《开发说明》§四:
//   规则 = 属性谓词(deps)+ 确定性纯函数(logic)。三种效果:
//     filter(过滤)  合法性校验——写侧决策环与读侧按钮可用性消费同一份
//     derive(推导)  派生属性(target_key 须在字典登记为派生键)
//     trigger(触发) 产生新事件候选(回到写侧管线,走完整四层校验)
//   硬约束:无环境——沙盒里无时钟、无随机数、无网络/文件/数据库;
//   deps 声明之外的键对规则不可见(最小权限,引擎只注入声明的键);
//   deps 引用的键必须已在字典登记(静态检查,未登记 = 编译不过);
//   按属性存在性匹配,不按类型归属——没有类。
//
// logic 采用 JSON-logic 方言(定义是数据,可直接进定义事件树,天然确定性):
//   取值:  {"var":"键"}        attrs 注入值(只含 deps 声明的键;缺失为 null)
//          {"cand":"键"}       candidate 顶层键("type"|"actor"|"occur_time"|"evidence"|"corrects")
//          {"write":"键"}      candidate 对当前求值目标本体写入的新值
//   运算:  {"==":[a,b]} {"!=":[a,b]} {"<":[a,b]} {"<=":[a,b]} {">":[a,b]} {">=":[a,b]}
//          {"+":[a,...]} {"-":[a,...]} {"*":[a,...]} {"/":[a,b]} {"/": 除零抛 RuleError}
//          {"and":[a,...]} {"or":[a,...]} {"!":a}
//          {"in":[v, [list...]]}  {"cat":[a,...]}  {"count":list}
//          {"if":[c,t,e]} 或 {"if":[c1,t1,c2,t2,...,else]}
//   顶层 if 链先归约:求值条件落到选中分支(分支可再嵌套 if),最终须落在终态
//   算子或裸表达式(仅 derive)上——状态机等"按条件通过/拒绝"的规则由此书写。
//   终态:  {"pass":true}             过滤通过
//          {"reject":"理由"}         过滤拒绝(理由进 violations)
//          {"return": expr}          推导:派生值
//          {"emit": {候选对象}}      触发:新候选({type, id?, actor?, 属性键...} 扁平载荷)
//          {"noop":true}             无动作
//   emit 载荷中的嵌套表达式先求值再组装候选;特殊引用 {"cand":"__target"}
//   取当前求值目标本体 id(如"复检 NOK → 对同一辆车发起新返修候选")。
//   emit 载荷中缺省的 actor 由引擎填 "rule:<rule_id>",space/occur_time 由
//   写侧管线从父候选继承。
//   filter 规则求值结果须为 pass/reject;derive 为 return 或裸表达式;
//   trigger 为 emit/noop。沙盒无任何环境能力函数——伸手拿环境在结构上不可能。
// ============================================================================

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "mse/model.h"

namespace mse {

// ---- 规则条目(API 文档 §6.3) ----
// 双运行时:
//   runtime="jsonlogic"(默认):logic 为 JSON-logic 表达式,解释执行。
//   runtime="wasm":artifact 为 base64 编码的 WASM 产物,在 wasm3 无环境沙盒
//     中执行(见 wasm_sandbox.h 的 ABI)。烘焙管线在定义结算时完成:解码 →
//     静态检查 → 沙盒试跑 → 记录 artifact_hash 与 engine_version(版本钉死:
//     同产物+同引擎才有逐比特重放)。产物库内容寻址、append-only。
// on_types:仅 derive/trigger 有效——只在这几类事件结算后求值(变化驱动);
//   空 = 对所有写及本本体的事件求值。filter 规则经事件类型的 rules 引用触发,
//   不使用 on_types。
struct Rule {
    std::string rule_id;
    std::vector<std::string> deps;       // 属性谓词:声明读取的字典键(决定适用性)
    std::string effect;                  // "filter" | "derive" | "trigger"
    std::string target_key;              // derive:派生键(字典中登记,rule_ref 指回本规则)
    json        logic;                   // jsonlogic:JSON-logic 表达式(确定性纯函数)
    std::string consumers = "both";      // 消费侧:"write" | "read" | "both"
    std::vector<std::string> on_types;   // derive/trigger:只在这些事件类型结算后求值
    std::string runtime = "jsonlogic";   // "jsonlogic" | "wasm"
    std::string artifact;                // wasm:base64 编码的 WASM 产物
    std::string export_name = "mse_eval";// wasm:入口导出函数名
    std::string artifact_hash;           // wasm:产物内容哈希(烘焙管线结算时填)
    std::string engine_version;          // wasm:沙盒引擎版本(烘焙管线结算时填)
    std::string status = "active";
    int64_t     version = 1;
    int64_t     registered_by = 0;       // 登记定义事件 id
};
void to_json(json& j, const Rule& r);
void from_json(const json& j, Rule& r);

// ---- 求值结果 ----
struct RuleOutcome {
    enum class Kind { kPass, kReject, kValue, kEmit, kNoop };

    Kind        kind = Kind::kNoop;
    std::string reason;                  // kReject:拒绝理由
    json        value;                   // kValue:派生值
    std::vector<Candidate> emitted;      // kEmit:触发的新候选(回到写侧管线)

    static RuleOutcome pass();
    static RuleOutcome reject(std::string reason);
    static RuleOutcome value_of(json v);
    static RuleOutcome emit(std::vector<Candidate> cs);
    static RuleOutcome noop();
};

// 求值时规则违规(DSL 形状错误/未声明键访问等)——定义层 bug,非业务拒绝
class RuleError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ----------------------------------------------------------------------------
// RuleEngine:无环境沙盒求值器。无状态——全部输入由参数注入,天然确定性。
// ----------------------------------------------------------------------------
class RuleEngine {
public:
    /// 静态检查(烘焙管线的编译期环节):deps 引用的键已登记;
    /// logic 中 {"var":...} 只引用 deps 内的键;形状合法。返回问题清单(空 = 通过)。
    /// is_registered 回调:键是否在字典登记(由 DefinitionLayer 提供,避免环依赖)。
    static std::vector<std::string> static_check(
        const Rule& r, const std::function<bool(const std::string&)>& is_registered);

    /// 求值。attrs 只含 deps 声明的键(调用方按依赖注入);
    /// candidate 为提交的变化描述;target_id 为当前求值的目标本体
    /// ({"write":...} 取其变化集)。抛出 RuleError 表示定义层 bug。
    RuleOutcome eval(const Rule& r, const json& attrs, const Candidate& candidate,
                     const std::string& target_id) const;

    /// 按属性谓词匹配:规则适用于某本体 ⟺ 该本体(变化后)聚合了规则 deps 的全部键。
    /// owned_keys = 目标本体当前属性键 ∪ candidate 写入键。
    /// effect_filter 非空时只返回该效果的规则;只返回 status == "active" 的规则。
    std::vector<const Rule*> match(const std::set<std::string>& owned_keys,
                                   const std::map<std::string, Rule>& rules,
                                   const std::string& effect_filter = "") const;

    /// 过滤规则族求值(写侧 L3 与读侧按钮可用性/行过滤共用同一份——读写一致性的实现):
    /// 对事件类型引用的每条 filter 规则,凡适用的(本体聚合了 deps 全部键)逐条求值;
    /// 任一 reject 即收集理由。返回拒绝理由清单(空 = 全部通过)。
    /// attrs_per_target:每个目标本体变化后的预览属性集(只含各自键,引擎内部按 deps 裁剪注入)。
    /// side:消费侧——"write" 只评 consumers∈{write,both} 的规则(写侧 L3/按钮可用性),
    /// "read" 只评 consumers∈{read,both} 的规则(读侧行过滤,§4.5 规则一份定义两处消费)。
    std::vector<std::string> eval_filters(
        const std::vector<std::string>& rule_refs,
        const std::map<std::string, Rule>& rules,
        const std::map<std::string, json>& attrs_per_target,
        const Candidate& candidate, const std::string& side = "write") const;

private:
    json eval_expr(const json& expr, const json& attrs, const Candidate& cand,
                   const std::string& target_id) const;
};

} // namespace mse
