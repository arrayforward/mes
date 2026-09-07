// ============================================================================
// mse/seeds_wasm.cpp —— WASM 种子规则:规则即 WASM 的全链路打样
//
// 读 assets/wasm/plan_adjust.wasm(预烘焙二进制,随仓库提交,构建期不依赖
// wabt),base64 后经 DefinitionLayer 结算注册 R-PLAN-ADJUST-WASM——烘焙管线
// (解码 → 静态检查 → 沙盒试跑 → 钉 artifact_hash/engine_version)在定义结算
// 内完成,与 jsonlogic 种子走同一条 settle_definition 路径。
// 产物文件缺失不致命(stderr 提示后跳过),方便无 assets 的最小部署。
// ============================================================================

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "mse/dictionary.h"
#include "mse/model.h"
#include "mse/seeds.h"
#include "mse/wasm_sandbox.h"

namespace mse {

void load_wasm_seeds(DefinitionLayer& defs, const std::string& assets_dir) {
    const std::string path = assets_dir + "/wasm/plan_adjust.wasm";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[seeds_wasm] 警告:读不到 %s,跳过 WASM 种子规则\n",
                     path.c_str());
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string bytes = ss.str();
    if (bytes.empty()) {
        std::fprintf(stderr, "[seeds_wasm] 警告:%s 为空,跳过 WASM 种子规则\n",
                     path.c_str());
        return;
    }

    // A3 五状态机调序过滤的 WASM 版(与 jsonlogic 种子 R-PLAN-ADJUST 同一立法)
    const json payload = {
        {"rule_id",  "R-PLAN-ADJUST-WASM"},
        {"deps",     {"计划状态"}},
        {"effect",   "filter"},
        {"runtime",  "wasm"},
        {"artifact", base64_encode(bytes)},
        {"consumers", "both"},
    };
    const Receipt r = defs.settle_definition("RuleRegistered", payload);
    if (r.status != Receipt::Status::kSettled) {
        std::fprintf(stderr, "[seeds_wasm] 警告:R-PLAN-ADJUST-WASM 烘焙/结算被拒:\n");
        for (const auto& v : r.violations) std::fprintf(stderr, "  - %s\n", v.c_str());
        return;
    }
    // 回执 settled:打印烘焙钉下的产物哈希与引擎版本(版本钉死,供审计)
    const Rule* rule = defs.find_rule("R-PLAN-ADJUST-WASM");
    std::fprintf(stderr, "[seeds_wasm] R-PLAN-ADJUST-WASM 已结算(event=%lld)\n",
                 static_cast<long long>(r.event_id.value_or(0)));
    if (rule != nullptr) {
        std::fprintf(stderr, "[seeds_wasm]   artifact_hash=%s engine_version=%s\n",
                     rule->artifact_hash.c_str(), rule->engine_version.c_str());
    }
}

} // namespace mse
