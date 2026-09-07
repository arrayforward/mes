#pragma once

// ============================================================================
// mse/wasm_sandbox.h —— WASM 规则沙盒(wasm3)+ 烘焙管线
//
// 依据实现方案 §3.3 与 API 文档 §六(烘焙管线):
//   规则是代码,代码运行在无环境沙盒里。ambient authority 是确定性与
//   可审计性的死敌——沙盒不注入任何时钟/随机/网络/文件/数据库能力。
//   唯一能力:读声明过的字典属性。deps 声明即最小权限——宿主在 ABI 层
//   强制:attr_*/write_* 导入函数对 deps 之外的键一律返回"不存在"。
//
// WASM ABI(宿主导入模块 "mse",全部确定性纯函数):
//   (import "mse" "attr_len"  (func (param i32 i32) (result i32)))
//       key_ptr,key_len → 该键当前属性值(JSON 文本)长度;不存在/未声明 → -1
//   (import "mse" "attr_get"  (func (param i32 i32 i32 i32) (result i32)))
//       key_ptr,key_len,out_ptr,out_cap → 写入 JSON 文本,返回实际长度(截断返回 -2)
//   (import "mse" "write_len" / "write_get"):同上,读候选对当前目标本体写入的新值
//   (import "mse" "result"    (func (param i32 i32 i32 i32)))
//       kind_ptr,kind_len ∈ {"pass","reject","value","noop"} + payload_ptr,payload_len
//       (reject:理由文本;value:派生值 JSON;其余忽略 payload)——规则通过它给出结论
//   触发(emit)结论:kind="value",payload 为 {"emit":[候选扁平载荷,...]} JSON。
// 客体导出:memory + export_name(默认 "mse_eval",无参无返回)。
//
// 烘焙管线(定义结算的固定环节,WasmSandbox::bake):
//   产物(base64)→ 解码 → 静态检查(deps 已登记、导出存在、无环境导入)
//   → 沙盒试跑(样例输入)→ 记录 artifact_hash(FNV-1a 64 hex)+
//   engine_version("wasm3 " M3_VERSION)——编译失败 = 定义候选被拒。
// ============================================================================

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mse/model.h"
#include "mse/rules.h"

namespace mse {

class WasmSandbox {
public:
    /// 沙盒引擎版本标识(如 "wasm3 0.5.0"),写进规则的 engine_version。
    static std::string engine_version();

    struct BakeResult {
        bool                     ok = false;
        std::vector<std::string> errors;         // 失败原因(= 定义候选被拒的清单)
        std::string              artifact_hash;  // ok:产物内容哈希(16 位 hex)
        std::string              engine_version; // ok:沙盒引擎版本
    };

    /// 烘焙:校验 + 试跑 + 钉版本。artifact_b64 为 base64 产物;
    /// is_registered 用于静态检查 deps 键已登记。不要求成功执行出特定结论,
    /// 但模块必须可加载、导出存在、导入不超过宿主 ABI 白名单。
    static BakeResult bake(const std::string& artifact_b64,
                           const std::vector<std::string>& deps,
                           const std::string& export_name,
                           const std::function<bool(const std::string&)>& is_registered);

    /// 求值。attrs 为按 deps 注入的属性集;host 在 ABI 层再次按 deps 白名单
    /// 过滤 attr_*/write_* 访问。沙盒运行错误/超时 → 抛 RuleError(定义层 bug)。
    RuleOutcome eval(const Rule& r, const json& attrs, const Candidate& candidate,
                     const std::string& target_id) const;
};

/// base64 编解码(产物进定义事件负载用)。
std::string base64_encode(const std::string& bytes);
std::string base64_decode(const std::string& b64, bool& ok);

} // namespace mse
