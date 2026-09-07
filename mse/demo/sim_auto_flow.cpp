// ============================================================================
// mse/demo/sim_auto_flow.cpp —— 整车厂总装车间的一天:端到端综合流程(交付演示)
//
// 一条"开班 → 物料 → 生产 → 质量 → 设备 → 收尾"的完整班次日,把 46 视图
// 背后的业务动作全部经真实 HTTP 落到事件树:
//   · 两个动词:全程 POST /events、GET /views/{id}(打印每个 POST 的回执,
//     被拦截的打印 violations);
//   · 规则即法:冻结禁调序、A3 五状态机(jsonlogic + WASM 双拦截)、防错比对、
//     锁定禁下线、逐级报警、KPI 派生——全是定义数据,不写死代码;
//   · 机制全景:拦截视图、遍历视图(VIN→批次→供应商)、规则决定行、时空切片、
//     AS OF t、定距快照、幂等键、崩溃恢复、重放逐比特一致;
//   · AI 不碰状态:knowledge 建议照样过四层校验;RFID 观测流经 entity 浮现本体。
//
// 退出码:0 = 全部关键环节符合预期;非 0 = 有红线被打破。
// ============================================================================

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "entity/resolver.h"
#include "entitytree/backends/memory_store.h"
#include "entitytree/model.h"
#include "mse/http_server.h"
#include "mse/plc_filter.h"
#include "mse/projection.h"
#include "mse/seeds.h"
#include "mse/system.h"
#include "storage/backends/sqlite_backend.h"
#include "voxelstore/record_voxel_store.h"

using nlohmann::json;

// ---- 演示自验:关键环节不符合预期即计失败,退出码非零 ----
static int g_failures = 0;
#define DEMO_CHECK(cond, what)                                                   \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  [红线被打破] %s:%d: %s\n", __FILE__, __LINE__, what); \
        }                                                                        \
    } while (0)

static void section(const char* title) {
    std::printf("\n============================================================\n");
    std::printf("  %s\n", title);
    std::printf("============================================================\n");
}

// ---- HTTP 驱动辅助 ----
static uint16_t g_port = 0;

// POST /events 并打印类型与回执(拦截的打印 violations);返回回执 JSON。
// token 非空时携带 X-MSE-Token 凭证头(信任分级:凭证 → 信任级)。
static json post_event(const json& payload, const std::string& note = "",
                       const std::string& token = "") {
    const std::map<std::string, std::string> headers =
        token.empty() ? std::map<std::string, std::string>{}
                      : std::map<std::string, std::string>{{"X-MSE-Token", token}};
    const auto [status, body] =
        mse::http_request("127.0.0.1", g_port, "POST", "/events", payload.dump(), headers);
    const json r = json::parse(body, nullptr, false);
    const std::string type = payload.value("type", "?");
    if (status == 200 && r.is_object() && r.value("status", "") == "settled") {
        std::printf("  POST %-22s → 结算 event_id=%lld %s\n", type.c_str(),
                    (long long)r["event_id"].get<int64_t>(), note.c_str());
    } else if (status == 200 && r.is_object() && r.value("status", "") == "accepted") {
        std::printf("  POST %-22s → 已受理 queue_seq=%lld(异步:drain 后落账)%s\n",
                    type.c_str(), (long long)r["queue_seq"].get<int64_t>(), note.c_str());
    } else {
        std::string viol;
        if (r.is_object() && r.contains("violations"))
            for (const json& v : r["violations"]) viol += " " + v.get<std::string>();
        std::printf("  POST %-22s → 拦截 layer=%d |%s %s\n", type.c_str(),
                    r.is_object() ? r.value("layer", -1) : -1, viol.c_str(), note.c_str());
    }
    return r;
}

static bool accepted(const json& receipt) {
    return receipt.is_object() && receipt.value("status", "") == "accepted";
}

static json get_view(const std::string& path_with_query) {
    const auto [status, body] =
        mse::http_request("127.0.0.1", g_port, "GET", path_with_query);
    if (status != 200) std::printf("  GET %s → HTTP %d\n", path_with_query.c_str(), status);
    return json::parse(body, nullptr, false);
}

static bool settled(const json& receipt) {
    return receipt.is_object() && receipt.value("status", "") == "settled";
}

static bool rejected_at(const json& receipt, int layer) {
    return receipt.is_object() && receipt.value("status", "") == "rejected" &&
           receipt.value("layer", -2) == layer;
}

static bool violations_contain(const json& receipt, const std::string& needle) {
    if (!receipt.is_object() || !receipt.contains("violations")) return false;
    for (const json& v : receipt["violations"])
        if (v.get<std::string>().find(needle) != std::string::npos) return true;
    return false;
}

static bool double_eq(const json& v, double expect) {
    return v.is_number() && std::fabs(v.get<double>() - expect) < 1e-9;
}

// 确定性伪随机(LCG;演示不读物理随机数,噪声流逐比特可复现)
struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed) {}
    uint32_t next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(s >> 33);  // 取高 31 位
    }
    double uniform(double lo, double hi) {
        return lo + (hi - lo) * (static_cast<double>(next()) / 2147483647.0);
    }
};

// ---- 视图格式化打印 ----
static void print_terminal_view(const char* title, const json& view) {
    std::printf("【%s】render=%s observer=%s\n", title,
                view.value("render_mode", "?").c_str(),
                view.value("observer", "").c_str());
    for (const json& row : view["rows"]) {
        std::string line = "  " + row.value("id", std::string("?")) + "  ";
        for (const json& col : view["columns"]) {
            const std::string c = col.get<std::string>();
            line += c + "=" + (row.contains(c) && !row[c].is_null()
                                   ? row[c].dump()
                                   : std::string("-")) +
                    "  ";
        }
        std::printf("%s\n", line.c_str());
        if (row.contains("buttons")) {
            std::string btns = "    按钮:";
            for (const json& b : row["buttons"]) {
                btns += std::string(" ") + (b["enabled"].get<bool>() ? "[●" : "[○") +
                        b["type"].get<std::string>() + "]";
                if (!b["enabled"].get<bool>() && !b["reasons"].empty())
                    btns += "(" + b["reasons"][0].get<std::string>() + ")";
            }
            std::printf("%s\n", btns.c_str());
        }
    }
}

static void print_flow_view(const char* title, const json& view) {
    std::printf("【%s】render=%s(流水,L1 事实 + L2 修正同列)\n", title,
                view.value("render_mode", "?").c_str());
    for (const json& row : view["rows"]) {
        std::string corr = row["is_correction"].get<bool>()
                               ? " 修正→#" + row["corrects"].dump()
                               : "";
        std::printf("  #%-3lld %-22s actor=%-24s writes=%s%s\n",
                    (long long)row["event_id"].get<int64_t>(),
                    row["type"].get<std::string>().c_str(),
                    row["actor"].get<std::string>().c_str(),
                    row["writes"].dump().c_str(), corr.c_str());
    }
}

static void print_intercept_view(const char* title, const json& view) {
    std::printf("【%s】render=%s(拦截模式:主角是 L0-L3 拦截反馈)\n", title,
                view.value("render_mode", "?").c_str());
    std::printf("  已结算事实行:%zu 条\n", view["rows"].size());
    for (const json& rj : view["rejections"]) {
        std::string viol;
        for (const json& v : rj["violations"]) viol += " " + v.get<std::string>();
        std::printf("  拦截 layer=%d type=%s writes=%s |%s\n", rj["layer"].get<int>(),
                    rj["candidate"]["type"].get<std::string>().c_str(),
                    rj["candidate"]["writes"].dump().c_str(), viol.c_str());
    }
}

static void print_traversal_view(const char* title, const json& view) {
    std::printf("【%s】render=%s root=%s\n", title,
                view.value("render_mode", "?").c_str(),
                view.value("root", "?").c_str());
    for (const json& n : view["nodes"]) {
        std::string line = "  节点 " + n.value("id", std::string("?")) + "  ";
        for (auto it = n.begin(); it != n.end(); ++it) {
            if (it.key() == "id" || it.value().is_null()) continue;
            line += it.key() + "=" + it.value().dump() + "  ";
        }
        std::printf("%s\n", line.c_str());
    }
    for (const json& e : view["edges"])
        std::printf("  边 %s -[%s]-> %s (源事件 #%lld)\n", e["from"].get<std::string>().c_str(),
                    e["key"].get<std::string>().c_str(), e["to"].get<std::string>().c_str(),
                    (long long)e["source_event"].get<int64_t>());
}

// 一行摘要(清单遍历用)
static void print_view_digest(const json& v) {
    if (v.contains("error")) {
        std::printf("  %-16s %-4s %s\n", v.value("view_id", "?").c_str(), "ERR",
                    v["error"].get<std::string>().c_str());
        return;
    }
    const std::string mode = v.value("render_mode", "?");
    if (mode == "遍历") {
        std::printf("  %-16s %-4s 节点=%zu 边=%zu\n", v["view_id"].get<std::string>().c_str(),
                    mode.c_str(), v["nodes"].size(), v["edges"].size());
    } else if (mode == "拦截") {
        std::printf("  %-16s %-4s 行=%zu 拦截=%zu\n", v["view_id"].get<std::string>().c_str(),
                    mode.c_str(), v["rows"].size(), v["rejections"].size());
    } else {
        std::printf("  %-16s %-4s 行=%zu\n", v["view_id"].get<std::string>().c_str(),
                    mode.c_str(), v["rows"].size());
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // ========================================================================
    section("0. 开班前:sqlite + voxel 持久化 + 种子定义 + WASM 规则 + HTTP 服务");
    // ========================================================================
    std::filesystem::remove("mse_demo.db");         // 每次演示从空白世界开始
    std::filesystem::remove("mse_demo_voxels.db");

    storage::SqliteBackend backend("mse_demo.db");  // storage 是唯一持久化接口
    auto voxel_backend = std::make_unique<storage::SqliteBackend>("mse_demo_voxels.db");
    voxelstore::RecordVoxelStore voxel_store(std::move(voxel_backend));
    mse::System sys(backend, &voxel_store);

    mse::load_system_keys(sys.defs());        // 一切皆属性:系统键先登记
    mse::load_auto_plant_seeds(sys.defs());   // 编词典、立行为、立法、开窗口(46 视图)
    mse::load_wasm_seeds(sys.defs(), MSE_ASSETS_DIR);  // WASM 规则沙盒(烘焙管线)
    sys.knowledge().load(std::string(MSE_ASSETS_DIR) + "/ontology_seed.json",
                         std::string(MSE_ASSETS_DIR) + "/lexicon.json");

    // WASM 规则接入类型(定义热更新):SequenceAdjusted 由 jsonlogic 与 WASM 双重立法
    const json wasm_wire = sys.api().post_definition(
        "EventTypeRegistered",
        {{"type", "SequenceAdjusted"},
         {"required_keys", {"id", "actor"}},
         {"optional_keys", {"优先级", "序列号"}},
         {"rules", {"R-PLAN-ADJUST", "R-SEQ-FROZEN", "R-PLAN-ADJUST-WASM"}},
         {"correction", "SequenceAdjustReversed"},
         {"multi_target", false},
         {"settlement", "sync"}});
    DEMO_CHECK(settled(wasm_wire), "WASM 规则接入 SequenceAdjusted 失败");
    std::printf("  SequenceAdjusted 规则族:R-PLAN-ADJUST + R-SEQ-FROZEN + R-PLAN-ADJUST-WASM\n");

    // 定距快照:每 20 条已结算事件落一份投影快照(AS OF 加速 + 崩溃恢复基座)
    mse::SnapshotStore snapshots(backend);
    sys.pipeline().set_snapshots(&snapshots, 20);

    std::printf("  定义层就绪:属性 %zu 键,事件类型 %zu 个,规则 %zu 条,锚点 %zu 个\n",
                sys.defs().attr_keys().size(), sys.defs().event_type_names().size(),
                sys.defs().rules().size(), sys.defs().anchors().size());
    std::printf("  knowledge 协助:%s\n", sys.knowledge().loaded() ? "已加载" : "未加载");

    // 信任分级:凭证 → 信任级(HTTP 头 X-MSE-Token 查表;未携带/未登记 → 0)
    sys.api().set_trust_tokens({{"plc-gw-01", 1}, {"mes-admin", 3}});

    mse::HttpServer server;
    const bool listening = server.listen_on("127.0.0.1", 0, [&sys](const mse::HttpRequest& req) {
        if (req.method == "POST" && req.path == "/events") {
            const json payload = json::parse(req.body, nullptr, false);
            if (payload.is_discarded())
                return mse::HttpResponse{400, "application/json; charset=utf-8",
                                         "{\"error\":\"bad json\"}"};
            int trust = 0;  // 凭证头 → 信任级
            if (auto it = req.headers.find("x-mse-token"); it != req.headers.end())
                trust = sys.api().trust_of(it->second);
            return mse::HttpResponse{200, "application/json; charset=utf-8",
                                     sys.api().post_events(payload, trust).dump()};
        }
        if (req.method == "GET" && req.path.rfind("/views/", 0) == 0) {
            return mse::HttpResponse{
                200, "application/json; charset=utf-8",
                sys.api().get_view(req.path.substr(7), req.query).dump()};
        }
        return mse::HttpResponse{404, "application/json; charset=utf-8",
                                 "{\"error\":\"not found\"}"};
    });
    DEMO_CHECK(listening, "HTTP 服务监听失败");
    g_port = server.port();
    std::printf("  HTTP 服务:127.0.0.1:%d(POST /events,GET /views/{id},其余 404)\n",
                g_port);

    const char* kWS = "整车厂/总装车间";  // 时空切片锚点(车间)
    (void)kWS;

    // ========================================================================
    section("1. 开班:ERP 报文 → 订单接收 → 排序/冻结 → 五连发 → 双拦截");
    // ========================================================================
    std::printf("-- H3:ERP 订单经集成接口进系统(报文即事实,证据留痕)--\n");
    DEMO_CHECK(settled(post_event({{"type", "InboundMessageRecorded"}, {"id", "MSG-ERP-0001"},
                                   {"actor", "erp-adapter"}, {"来源系统", "SAP-ERP"},
                                   {"报文类型", "IDOC-ORDERS"}, {"外部单号", "PO-20260902-001"},
                                   {"evidence", "idoc:0000004711"}})),
               "ERP 报文未结算");

    std::printf("-- A1:三个整车订单 + 产线计划基线(计划产量下到产线本体)--\n");
    const json r_o1 = post_event({{"type", "OrderReceived"}, {"id", "VIN-LBV0001"},
                                  {"actor", "erp"}, {"车型", "SUV-A"}, {"订单状态", "接收"},
                                  {"交付期", "2026-09-10"},
                                  {"idempotency_key", "erp-order-0001"}});
    DEMO_CHECK(settled(r_o1), "订单 1 未结算");
    DEMO_CHECK(settled(post_event({{"type", "OrderReceived"}, {"id", "VIN-LBV0002"},
                                   {"actor", "erp"}, {"车型", "Sedan-B"}, {"订单状态", "接收"},
                                   {"交付期", "2026-09-12"}})),
               "订单 2 未结算");
    DEMO_CHECK(settled(post_event({{"type", "OrderReceived"}, {"id", "VIN-LBV0003"},
                                   {"actor", "erp"}, {"车型", "SUV-A"}, {"订单状态", "接收"},
                                   {"交付期", "2026-09-15"}})),
               "订单 3 未结算");
    DEMO_CHECK(settled(post_event({{"type", "OrderReceived"}, {"id", "总装线"},
                                   {"actor", "erp"}, {"车型", "混流产线"},
                                   {"计划产量", 200}})),
               "产线计划基线未结算");

    std::printf("-- A2:排序定序列号;换单(未冻结,合法);冻结一单 --\n");
    DEMO_CHECK(settled(post_event({{"type", "SequenceAdjusted"}, {"id", "VIN-LBV0001"},
                                   {"actor", "planner-li"}, {"序列号", 1}, {"优先级", 10}})),
               "VIN1 定序未结算");
    DEMO_CHECK(settled(post_event({{"type", "SequenceAdjusted"}, {"id", "VIN-LBV0002"},
                                   {"actor", "planner-li"}, {"序列号", 2}, {"优先级", 20}})),
               "VIN2 定序未结算");
    DEMO_CHECK(settled(post_event({{"type", "SequenceAdjusted"}, {"id", "VIN-LBV0003"},
                                   {"actor", "planner-li"}, {"序列号", 3}, {"优先级", 30}})),
               "VIN3 定序未结算");
    DEMO_CHECK(settled(post_event({{"type", "OrderSwapped"},
                                   {"actor", "planner-li"},
                                   {"writes", {{"VIN-LBV0002", {{"序列号", 3}}},
                                               {"VIN-LBV0003", {{"序列号", 2}}}}}},
                                  "← 一次发生、两单换位")),
               "换单未结算");
    DEMO_CHECK(settled(post_event({{"type", "OrderFrozen"}, {"id", "VIN-LBV0002"},
                                   {"actor", "planner-li"}, {"冻结状态", "已冻结"}})),
               "冻结未结算");

    std::printf("-- 冻结后再调序:应被 R-SEQ-FROZEN 拦截(L3)--\n");
    const json r_frz = post_event({{"type", "SequenceAdjusted"}, {"id", "VIN-LBV0002"},
                                   {"actor", "planner-li"}, {"序列号", 9}},
                                  "← 已冻结订单禁止调序");
    DEMO_CHECK(rejected_at(r_frz, 3) && violations_contain(r_frz, "R-SEQ-FROZEN"),
               "冻结禁调序未生效");

    std::printf("-- A3:VIN1/VIN2 五连发 01→05 --\n");
    for (const char* vin : {"VIN-LBV0001", "VIN-LBV0002"})
        for (const char* st : {"01", "02", "03", "04", "05"})
            DEMO_CHECK(settled(post_event({{"type", "PlanReleased"}, {"id", vin},
                                           {"actor", "planner-li"}, {"计划状态", st}})),
                       "五连发未结算");

    std::printf("-- 05 调序:应被 R-PLAN-ADJUST 与 R-PLAN-ADJUST-WASM 双重拦截(L3)--\n");
    const json r_adj5 = post_event({{"type", "SequenceAdjusted"}, {"id", "VIN-LBV0001"},
                                    {"actor", "planner-li"}, {"优先级", 1}},
                                   "← 05 绝对禁止调整(jsonlogic+WASM 同一立法)");
    DEMO_CHECK(rejected_at(r_adj5, 3), "05 调序未被 L3 拦截");
    DEMO_CHECK(violations_contain(r_adj5, "R-PLAN-ADJUST"), "缺 jsonlogic 拦截理由");
    DEMO_CHECK(violations_contain(r_adj5, "R-PLAN-ADJUST-WASM"), "缺 WASM 拦截理由");

    // ========================================================================
    section("2. 物料:BOM 下达 → 防错比对 → 批次绑定 → 库存/拉动 → 缺料呼叫");
    // ========================================================================
    std::printf("-- B2/A4:TCM 的 BOM 与工艺数据下到工位01 --\n");
    DEMO_CHECK(settled(post_event(
        {{"type", "BomReceived"}, {"id", "工位01"}, {"actor", "tcm-adapter"},
         {"BOM清单", {"MAT-1001", "MAT-1002"}},
         {"物料需求", {"MAT-1001×2", "MAT-1002×1"}},
         {"工艺信息", "拧紧扭矩 25N·m±2,转角 90°"},
         {"space", {{"anchor", "整车厂/总装车间/总装线/工位01"}}}})),
        "BOM 下达未结算");

    std::printf("-- B4 防错:扫错料应被 R-MAT-PKE 拦截(L3,进 RejectionLog)--\n");
    const json r_pke = post_event({{"type", "MaterialVerified"}, {"id", "工位01"},
                                   {"actor", "op-zhang"}, {"物料编号", "MAT-9999"}},
                                  "← 扫描物料不在 BOM 内");
    DEMO_CHECK(rejected_at(r_pke, 3) && violations_contain(r_pke, "R-MAT-PKE"),
               "错料未被拦截");
    DEMO_CHECK(settled(post_event({{"type", "MaterialVerified"}, {"id", "工位01"},
                                   {"actor", "op-zhang"}, {"物料编号", "MAT-1001"}},
                                  "← 扫对通过")),
               "对料未通过");

    std::printf("-- B1:批次登记(带供应商 ref)→ B5:两车各绑两批(双向 ref 边)--\n");
    for (const json& m : std::vector<json>{
             {{"id", "BATCH-A1"}, {"物料编号", "MAT-1001"}, {"批次号", "A1"},
              {"供应商", "SUP-BOSCH"}},
             {{"id", "BATCH-A2"}, {"物料编号", "MAT-1002"}, {"批次号", "A2"},
              {"供应商", "SUP-BOSCH"}},
             {{"id", "BATCH-B1"}, {"物料编号", "MAT-1003"}, {"批次号", "B1"},
              {"供应商", "SUP-CONTINENTAL"}},
             {{"id", "BATCH-B2"}, {"物料编号", "MAT-1004"}, {"批次号", "B2"},
              {"供应商", "SUP-CONTINENTAL"}}}) {
        json p = {{"type", "MaterialRegistered"}, {"actor", "wms"},
                  {"库存数量", 500}, {"库存阈值", 100}, {"库位", "W-01"}};
        p.update(m);
        DEMO_CHECK(settled(post_event(p)), "批次登记未结算");
    }
    for (const auto& [vin, batch] : std::vector<std::pair<std::string, std::string>>{
             {"VIN-LBV0001", "BATCH-A1"}, {"VIN-LBV0001", "BATCH-A2"},
             {"VIN-LBV0002", "BATCH-B1"}, {"VIN-LBV0002", "BATCH-B2"}}) {
        DEMO_CHECK(settled(post_event({{"type", "BatchBoundToVIN"},
                                       {"actor", "logistics"},
                                       {"writes", {{vin, {{"批次绑定", batch}}},
                                                   {batch, {{"VIN绑定", vin}}}}}})),
                   "批次绑定未结算");
    }

    std::printf("-- B3/B11:库存更新(线边库切片 vs 中央库)--\n");
    DEMO_CHECK(settled(post_event(
        {{"type", "StockUpdated"}, {"id", "线边-MAT-1001"}, {"actor", "wms"},
         {"物料编号", "MAT-1001"}, {"库存数量", 48}, {"库位", "LS-01-02"},
         {"space", {{"anchor", "整车厂/总装车间/线边库"}}}},
        "← 线边库切片内")),
        "线边库存未结算");
    DEMO_CHECK(settled(post_event(
        {{"type", "StockUpdated"}, {"id", "中央-MAT-1001"}, {"actor", "wms"},
         {"物料编号", "MAT-1001"}, {"库存数量", 5000}, {"库位", "CW-01"},
         {"space", {{"anchor", "整车厂/总装车间"}}}},
        "← 切片外,只进 B3")),
        "中央库存未结算");

    std::printf("-- B7-B10:JIS 拉动全生命周期 + 其余三类各建一单(规则决定行用)--\n");
    DEMO_CHECK(settled(post_event({{"type", "PullOrderCreated"}, {"id", "PO-JIS-0001"},
                                   {"actor", "logistics"}, {"拉动类型", "JIS"},
                                   {"拉动状态", "已创建"}, {"物料编号", "MAT-1001"}})),
               "JIS 拉动单未创建");
    DEMO_CHECK(settled(post_event({{"type", "PullOrderShipped"}, {"id", "PO-JIS-0001"},
                                   {"actor", "supplier"}, {"拉动状态", "已发货"}})),
               "JIS 发货未结算");
    DEMO_CHECK(settled(post_event({{"type", "PullOrderReceived"}, {"id", "PO-JIS-0001"},
                                   {"actor", "wms"}, {"拉动状态", "已收货"}})),
               "JIS 收货未结算");
    for (const auto& [po, kind] : std::vector<std::pair<std::string, std::string>>{
             {"PO-KAN-0001", "Kanban"}, {"PO-URG-0001", "紧急"}, {"PO-JIT-0001", "JIT"}}) {
        DEMO_CHECK(settled(post_event({{"type", "PullOrderCreated"}, {"id", po},
                                       {"actor", "logistics"}, {"拉动类型", kind},
                                       {"拉动状态", "已创建"}, {"物料编号", "MAT-1001"}})),
                   "拉动单未创建");
    }

    std::printf("-- B6:缺料呼叫 → 响应;再呼一单留在看板上 --\n");
    DEMO_CHECK(settled(post_event({{"type", "MaterialCallRaised"}, {"id", "CALL-0001"},
                                   {"actor", "op-zhang"}, {"呼叫状态", "呼叫中"},
                                   {"缺料工位", "工位02"}})),
               "缺料呼叫未结算");
    DEMO_CHECK(settled(post_event({{"type", "MaterialCallAnswered"}, {"id", "CALL-0001"},
                                   {"actor", "logistics"}, {"呼叫状态", "已响应"}})),
               "呼叫响应未结算");
    DEMO_CHECK(settled(post_event({{"type", "MaterialCallRaised"}, {"id", "CALL-0002"},
                                   {"actor", "op-wang"}, {"呼叫状态", "呼叫中"},
                                   {"缺料工位", "工位03"}})),
               "第二呼叫未结算");

    // ========================================================================
    section("3. 生产:AVI 过点 → ANDON 停/复线 → PMC 逐级报警");
    // ========================================================================
    std::printf("-- C:AVI 两车过工位01-03;VIN2 在工位02 后离区再入工位03 --\n");
    int64_t t_mid = 0;  // AS OF 演示用:首个过点事件的序号
    for (const char* st : {"工位01", "工位02"}) {
        for (const char* vin : {"VIN-LBV0001", "VIN-LBV0002"}) {
            const json r = post_event(
                {{"type", "VehicleEnteredZone"}, {"id", vin}, {"actor", "avi-gate"},
                 {"过点区域", st},
                 {"space", {{"anchor", std::string("整车厂/总装车间/总装线/") + st}}}});
            DEMO_CHECK(settled(r), "过点未结算");
            if (t_mid == 0) t_mid = r["event_id"].get<int64_t>();
        }
    }
    DEMO_CHECK(settled(post_event(
        {{"type", "VehicleExitedZone"}, {"id", "VIN-LBV0002"}, {"actor", "avi-gate"},
         {"过点区域", "工位02"},
         {"space", {{"anchor", "整车厂/总装车间/总装线/工位02"}}}},
        "← VIN2 离开工位02(转返修区)")),
        "离区未结算");
    for (const char* vin : {"VIN-LBV0001", "VIN-LBV0002"}) {
        DEMO_CHECK(settled(post_event(
            {{"type", "VehicleEnteredZone"}, {"id", vin}, {"actor", "avi-gate"},
             {"过点区域", "工位03"},
             {"space", {{"anchor", "整车厂/总装车间/总装线/工位03"}}}})),
            "工位03 过点未结算");
    }

    std::printf("-- D ANDON:设备呼叫 → 响应 → 拉环停线 → 复线 --\n");
    DEMO_CHECK(settled(post_event({{"type", "CallRaised"}, {"id", "总装线"},
                                   {"actor", "op-wang"}, {"呼叫类型", "设备"},
                                   {"呼叫状态", "呼叫中"},
                                   {"space", {{"anchor", "整车厂/总装车间/总装线"}}}})),
               "ANDON 呼叫未结算");
    DEMO_CHECK(settled(post_event({{"type", "CallAcknowledged"}, {"id", "总装线"},
                                   {"actor", "maintenance"}, {"呼叫状态", "已响应"}})),
               "ANDON 响应未结算");
    DEMO_CHECK(settled(post_event({{"type", "LineStopped"}, {"id", "总装线"},
                                   {"actor", "op-wang"}, {"线状态", "停线"},
                                   {"停线原因", "拧紧枪故障(E-427)"}})),
               "停线未结算");
    DEMO_CHECK(settled(post_event({{"type", "LineResumed"}, {"id", "总装线"},
                                   {"actor", "maintenance"}, {"线状态", "运行"}})),
               "复线未结算");

    std::printf("-- E PMC:同检点连发故障,观察 derive 计数与逐级报警 --\n");
    for (int i = 1; i <= 10; ++i) {
        const json r = post_event({{"type", "FaultAlarmed"}, {"id", "EQ-NG01"},
                                   {"actor", "plc-gateway"}, {"检点", "工位02-拧紧点P1"},
                                   {"故障码", "E-427"}, {"设备状态", "故障"},
                                   {"故障级别", 2}},
                                  i == 3  ? "← 第 3 次:触发科长预警"
                                  : i == 10 ? "← 第 10 次:触发部长预警"
                                            : "");
        DEMO_CHECK(settled(r), "故障报警未结算");
        if (i == 3) {
            const json attrs = sys.projection().attrs_of("EQ-NG01");
            DEMO_CHECK(attrs.value("同检点故障计数", 0) == 3, "同检点计数 derive 未到 3");
            const auto alarms = sys.log().events_of_type("AlarmEscalated");
            DEMO_CHECK(alarms.size() == 1, "第 3 次应触发 1 条科长预警");
            if (!alarms.empty())
                DEMO_CHECK(alarms[0].writes.at("EQ-NG01").at("报警级别") == "科长",
                           "第 3 次预警级别应为科长");
        }
    }
    {
        const json attrs = sys.projection().attrs_of("EQ-NG01");
        std::printf("  EQ-NG01 同检点故障计数 = %s(derive R-ALM-COUNT,null→0 起步)\n",
                    attrs["同检点故障计数"].dump().c_str());
        DEMO_CHECK(attrs.value("同检点故障计数", 0) == 10, "同检点计数 derive 未到 10");
        bool saw_minister = false;
        for (const mse::Event& e : sys.log().events_of_type("AlarmEscalated"))
            if (e.writes.at("EQ-NG01").at("报警级别") == "部长") saw_minister = true;
        DEMO_CHECK(saw_minister, "第 10 次未触发部长预警");
    }
    DEMO_CHECK(settled(post_event({{"type", "FaultCleared"}, {"id", "EQ-NG01"},
                                   {"actor", "maintenance"}, {"设备状态", "运行"},
                                   {"检点", "工位02-拧紧点P1"}, {"故障码", "E-427"}})),
               "故障消除未结算");
    DEMO_CHECK(settled(post_event({{"type", "CountUpdated"}, {"id", "总装线"},
                                   {"actor", "pmc-collector"}, {"产量计数", 2},
                                   {"停线计数", 1}, {"首次合格数", 2}, {"缓冲区计数", 5}})),
               "计数更新未结算");

    // ========================================================================
    section("3.5 PLC 噪声洪峰:适配层三层过滤 + 异步结算 + 信任分级");
    // ========================================================================
    // 工位01 占用光电:100000 个含噪采样(LCG 确定性)灌入适配层;
    // 真实信号只翻转 2 次(空闲→占用→空闲)——验证噪声洪峰下事件率有界。
    {
        mse::PlcFilter filter("工位01-占用光电", mse::PlcFilter::Config{1.0, 0.0, 3, 0});
        mse::AdapterGateway gw;
        mse::AdapterGateway::EdgeMapping m;
        m.type = "PlcEdgeReported";  // async + min_trust=1(种子定义)
        m.target_id = "工位01";
        m.key = "工位占用";
        m.high_value = "占用";
        m.low_value = "空闲";
        m.anchor = "整车厂/总装车间/总装线/工位01";
        gw.map_edge("工位01-占用光电", m);

        constexpr int64_t kSamples = 100000;
        Lcg lcg(20260907);
        int edges = 0, accepted_n = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int64_t t = 0; t < kSamples; ++t) {
            // 真实信号:0..29999 空闲,30000..69999 占用,70000.. 空闲(真翻转 2 次)
            const bool high = t >= 30000 && t < 70000;
            double v = (high ? 1.0 : 0.0) + lcg.uniform(-0.1, 0.1);  // 阈值附近抖动
            if (!high && t % 2000 < 2) v = 1.05;  // 噪声突刺:2 连拍越阈,不足驻留
            const auto edge = filter.sample(v, t);
            if (!edge.has_value()) continue;  // 噪声/驻留中:到不了端点
            ++edges;
            auto cand = gw.translate("工位01-占用光电", *edge, t);
            cand->trust = 3;  // 进程内适配器入口:显式信任级
            if (sys.pipeline().submit(*cand).status == mse::Receipt::Status::kAccepted)
                ++accepted_n;
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        std::printf("  采样 %lld 个 → 迁移沿 %d 个 → 异步受理 %d 条(过滤+提交 %.1f ms)\n",
                    (long long)kSamples, edges, accepted_n, ms);
        DEMO_CHECK(edges == 2, "迁移沿数应等于真实翻转次数 2");
        DEMO_CHECK(accepted_n == edges, "迁移沿应全部被异步受理");
        // 异步悬态口径:已验未结,drain 前视图看不到
        std::printf("  悬态 %zu 条在异步队列(已验未结)→ drain_async 串行落账\n",
                    sys.pipeline().async_pending());
        const size_t drained = sys.pipeline().drain_async();
        const size_t plc_events = sys.log().events_of_type("PlcEdgeReported").size();
        std::printf("  drain 结算 %zu 条;事件日志 PlcEdgeReported = %zu(零污染,有界)\n",
                    drained, plc_events);
        DEMO_CHECK(plc_events == 2, "噪声洪峰下结算事件数应有界(==2)");
        DEMO_CHECK(
            sys.projection().attrs_of("工位01").value("工位占用", "") == "空闲",
            "工位01 终态应为空闲");
    }

    // 信任分级(真实 HTTP 凭证头):无凭证/错凭证被 L2 拒,PLC 网关凭证受理
    std::printf("-- 信任分级:X-MSE-Token 凭证 → 信任级 → min_trust L2 校验 --\n");
    {
        const json plc_payload = {{"type", "PlcEdgeReported"}, {"id", "工位03"},
                                  {"actor", "plc-gateway"}, {"工位占用", "占用"}};
        const json r_no = post_event(plc_payload, "← 无凭证:应被 L2 拒");
        DEMO_CHECK(rejected_at(r_no, 2) && violations_contain(r_no, "信任级不足"),
                   "无凭证提交 PLC 类型未被 L2 拒");
        const json r_bad = post_event(plc_payload, "← 错凭证:同样被拒", "forged-token");
        DEMO_CHECK(rejected_at(r_bad, 2), "错凭证未被拒");
        const json r_ok = post_event(plc_payload, "← PLC 网关凭证", "plc-gw-01");
        DEMO_CHECK(accepted(r_ok), "带 PLC 网关凭证未被受理");
        const size_t drained_http = sys.pipeline().drain_async();
        std::printf("  drain_async 结算 %zu 条;工位03 工位占用=%s\n", drained_http,
                    sys.projection().attrs_of("工位03").value("工位占用", "?").c_str());
        DEMO_CHECK(
            sys.projection().attrs_of("工位03").value("工位占用", "") == "占用",
            "工位03 终态应为占用");
    }

    // ========================================================================
    section("4. 质量:检验 → 缺陷锁车 → 误扫拦截 → 返修 → 改判 → 解锁");
    // ========================================================================
    std::printf("-- F2/F4:检验计划下达与任务指派 --\n");
    DEMO_CHECK(settled(post_event({{"type", "InspectionPlanIssued"}, {"id", "PLAN-Q-0001"},
                                   {"actor", "qc-planner"}, {"检验计划号", "QP-2026-ASM-01"}})),
               "检验计划未结算");
    DEMO_CHECK(settled(post_event({{"type", "InspectionTaskAssigned"}, {"id", "TASK-Q-0001"},
                                   {"actor", "qc-planner"}, {"检验工位", "终检工位"},
                                   {"检验计划号", "QP-2026-ASM-01"}})),
               "检验任务未结算");

    std::printf("-- F12:检测线设备数据写入车辆质量记录 --\n");
    DEMO_CHECK(settled(post_event({{"type", "InspectionDataRecorded"}, {"id", "VIN-LBV0001"},
                                   {"actor", "testline-plc"}, {"尾气检测值", 0.8},
                                   {"大灯检测值", 92.5}, {"淋雨结论", "合格"}})),
               "检验数据未结算");

    std::printf("-- F10:划痕(级别2)→ R-QUAL-TRIGGER 自动锁车 --\n");
    const json r_def = post_event({{"type", "DefectRegistered"}, {"id", "VIN-LBV0001"},
                                   {"actor", "qc-liuyang"}, {"车漆", "划痕"}, {"缺陷级别", 2},
                                   {"space", {{"raw", "左前门"}}},
                                   {"evidence", "img:defect-8821.jpg"}});
    DEMO_CHECK(settled(r_def), "缺陷登记未结算");
    DEMO_CHECK(sys.log().events_of_type("VehicleLocked").size() == 1, "未自动锁车");

    std::printf("-- 锁定期间报工:应被 R-QUAL-LOCK 拦截(L3)--\n");
    const json r_rep0 = post_event({{"type", "ProductionReported"}, {"id", "VIN-LBV0001"},
                                    {"actor", "op-zhang"}, {"报工数量", 1}},
                                   "← 锁定车辆不得下线");
    DEMO_CHECK(rejected_at(r_rep0, 3) && violations_contain(r_rep0, "R-QUAL-LOCK"),
               "锁定报工未被拦截");

    std::printf("-- F10:误扫缺陷(级别越界):应被 L2 值域校验拦截(拦截视图可见)--\n");
    const json r_mis9 = post_event({{"type", "DefectRegistered"}, {"id", "VIN-LBV0001"},
                                    {"actor", "qc-newbie"}, {"车漆", "划痕"},
                                    {"缺陷级别", 9}},
                                   "← 缺陷级别越出值域 {1,2,3}");
    DEMO_CHECK(rejected_at(r_mis9, 2), "误扫缺陷未被 L2 拦截");

    std::printf("-- F11 返修全流程:NOK 自动再开 → OK 关闭 --\n");
    DEMO_CHECK(settled(post_event({{"type", "ReworkRequested"}, {"id", "VIN-LBV0001"},
                                   {"actor", "qc-liuyang"}})),
               "返修请求未结算");
    DEMO_CHECK(settled(post_event({{"type", "ReworkRecorded"}, {"id", "VIN-LBV0001"},
                                   {"actor", "reworker-wang"},
                                   {"返修内容", "左前门漆面打磨后重喷"}})),
               "返修记录未结算");
    DEMO_CHECK(settled(post_event({{"type", "ReworkSubmitted"}, {"id", "VIN-LBV0001"},
                                   {"actor", "reworker-wang"}})),
               "返修提交未结算");
    const json r_nok = post_event({{"type", "RecheckJudged"}, {"id", "VIN-LBV0001"},
                                   {"actor", "rechecker-zhao"}, {"复检结论", "NOK"}},
                                  "← 复检不合格");
    DEMO_CHECK(settled(r_nok), "复检 NOK 未结算");
    DEMO_CHECK(sys.log().events_of_type("ReworkRequested").size() == 2,
               "NOK 未自动再开返修");
    DEMO_CHECK(settled(post_event({{"type", "ReworkRecorded"}, {"id", "VIN-LBV0001"},
                                   {"actor", "reworker-wang"},
                                   {"返修内容", "二次返修:局部补漆并抛光"}})),
               "二次返修未结算");
    DEMO_CHECK(settled(post_event({{"type", "ReworkSubmitted"}, {"id", "VIN-LBV0001"},
                                   {"actor", "reworker-wang"}})),
               "二次提交未结算");
    DEMO_CHECK(settled(post_event({{"type", "RecheckJudged"}, {"id", "VIN-LBV0001"},
                                   {"actor", "rechecker-zhao"}, {"复检结论", "OK"}})),
               "复检 OK 未结算");
    DEMO_CHECK(settled(post_event({{"type", "ReworkClosed"}, {"id", "VIN-LBV0001"},
                                   {"actor", "rechecker-zhao"}})),
               "返修关闭未结算");

    std::printf("-- 改判:质量主管复核,首次 NOK 系误判 → JudgementOverruled(corrects 链)--\n");
    const json r_ovr = post_event({{"type", "JudgementOverruled"}, {"id", "VIN-LBV0001"},
                                   {"actor", "qc-chief"}, {"复检结论", "OK"},
                                   {"corrects", r_nok["event_id"].get<int64_t>()}},
                                  "← 误判改判,原判永存");
    DEMO_CHECK(settled(r_ovr), "改判未结算");
    DEMO_CHECK(sys.log().corrections_of(r_nok["event_id"].get<int64_t>()).size() == 1,
               "改判修正链断裂");

    DEMO_CHECK(settled(post_event({{"type", "VehicleUnlocked"}, {"id", "VIN-LBV0001"},
                                   {"actor", "qc-liuyang"}, {"锁定状态", "未锁"}})),
               "解锁未结算");

    std::printf("-- VIN2 误录缺陷(色差,级别1)→ DefectCancelled 冲正 --\n");
    const json r_mis = post_event({{"type", "DefectRegistered"}, {"id", "VIN-LBV0002"},
                                   {"actor", "qc-newbie"}, {"车漆", "色差"}, {"缺陷级别", 1}},
                                  "← 事后发现是误录");
    DEMO_CHECK(settled(r_mis), "误录缺陷未结算");
    const json r_cancel = post_event({{"type", "DefectCancelled"}, {"id", "VIN-LBV0002"},
                                      {"actor", "qc-liuyang"}, {"车漆", "完好"},
                                      {"corrects", r_mis["event_id"].get<int64_t>()}},
                                     "← 携带 corrects 因果引用");
    DEMO_CHECK(settled(r_cancel), "缺陷撤销未结算");
    DEMO_CHECK(sys.projection().attrs_of("VIN-LBV0002").value("车漆", "") == "完好",
               "修正后终态错误");

    // ========================================================================
    section("5. 设备:台账状态上报(一台故障→运行)");
    // ========================================================================
    DEMO_CHECK(settled(post_event({{"type", "EquipmentStatusReported"}, {"id", "EQ-NG01"},
                                   {"actor", "plc-gateway"}, {"设备编号", "NG-01"},
                                   {"设备状态", "运行"}, {"刀具寿命", 68}})),
               "设备 1 上报未结算");
    DEMO_CHECK(settled(post_event({{"type", "EquipmentStatusReported"}, {"id", "EQ-AGV02"},
                                   {"actor", "plc-gateway"}, {"设备编号", "AGV-02"},
                                   {"设备状态", "故障"}, {"刀具寿命", 90}})),
               "设备 2 上报未结算");
    DEMO_CHECK(settled(post_event({{"type", "EquipmentStatusReported"}, {"id", "EQ-AGV02"},
                                   {"actor", "maintenance"}, {"设备编号", "AGV-02"},
                                   {"设备状态", "运行"}, {"刀具寿命", 90}},
                                  "← 修复后复机")),
               "设备 2 复机未结算");

    // ========================================================================
    section("6. 收尾:报工上卷 + 组成声明 + KPI 派生 + entity 演化");
    // ========================================================================
    std::printf("-- A7:解锁后报工(一次发生:VIN 与产线同时就位,属性多属)--\n");
    DEMO_CHECK(settled(post_event({{"type", "ProductionReported"},
                                   {"actor", "op-zhang"},
                                   {"writes", {{"VIN-LBV0001", {{"报工数量", 1}}},
                                               {"总装线", {{"报工数量", 1},
                                                           {"实际产量", 2}}}}},
                                   {"space", {{"anchor", "整车厂/总装车间/总装线"}}}})),
               "报工未结算");
    std::printf("-- 报工错误 → ReportReversed 冲正(A7 流水 L1+L2 成对可见)--\n");
    const json r_wrong = post_event({{"type", "ProductionReported"}, {"id", "总装线"},
                                     {"actor", "op-zhang"}, {"报工数量", 1}, {"实际产量", 3}},
                                    "← 实际产量误写为 3");
    DEMO_CHECK(settled(r_wrong), "误报工未结算");
    DEMO_CHECK(settled(post_event({{"type", "ReportReversed"}, {"id", "总装线"},
                                   {"actor", "statistician"}, {"实际产量", 2},
                                   {"corrects", r_wrong["event_id"].get<int64_t>()}},
                                  "← 冲正回 2,derive 随动")),
               "冲正未结算");
    {
        const json line = sys.projection().attrs_of("总装线");
        std::printf("  总装线:计划产量=%s 实际产量=%s 达成率=%s(R-KPI-002) FTT=%s(R-KPI-003)\n",
                    line["计划产量"].dump().c_str(), line["实际产量"].dump().c_str(),
                    line["达成率"].dump().c_str(), line["FTT"].dump().c_str());
        DEMO_CHECK(line.value("实际产量", 0) == 2, "冲正后实际产量错误");
        DEMO_CHECK(double_eq(line["达成率"], 0.01), "达成率派生错误");
        DEMO_CHECK(double_eq(line["FTT"], 1.0), "FTT 派生错误");
        const json vin1 = sys.projection().attrs_of("VIN-LBV0001");
        DEMO_CHECK(vin1.value("缺陷总数", 0) == 1, "缺陷总数派生错误(误扫被拦不计)");
    }

    std::printf("-- 组成声明:车间聚合产线的产量属性(多属)+ OEE 三率 --\n");
    DEMO_CHECK(settled(post_event(
        {{"type", "CompositionDeclared"}, {"id", "总装车间"}, {"actor", "mes-collector"},
         {"aggregates", {{{"key", "实际产量"}, {"from", "总装线"}},
                         {{"key", "计划产量"}, {"from", "总装线"}}}},
         {"可用率", 0.92}, {"性能率", 0.95}, {"良品率", 0.98}})),
        "组成声明未结算");
    {
        const json ws = sys.projection().attrs_of("总装车间");
        std::printf("  总装车间:实际产量=%s(聚合自总装线) OEE=%s(= 0.92×0.95×0.98)\n",
                    ws["实际产量"].dump().c_str(), ws["OEE"].dump().c_str());
        DEMO_CHECK(ws.value("实际产量", 0) == 2, "聚合集回填错误");
        DEMO_CHECK(ws.contains("OEE"), "OEE 未派生");
    }

    std::printf("-- entity 演化:RFID 观测流浮现新本体(非人力创建)--\n");
    entitytree::MemoryEntityStore et_store;
    entity::Resolver            resolver(et_store);
    sys.attach_entity_bridge(resolver, et_store);
    const char* kAnchor = "整车厂/总装车间/总装线/工位02";
    const char* keys[]  = {"观测标识", "观测来源", "载具类型"};
    const char* srcs[]  = {"rfid-gate-01", "rfid-gate-02"};
    std::vector<mse::EmergedEntity> emerged;
    int64_t ts = 2000;
    for (const char* key : keys) {
        for (const char* src : srcs) {
            entitytree::Observation obs;
            obs.attribute_key = key;
            obs.value = std::string(key) == "观测标识"    ? "CARRIER-777"
                        : std::string(key) == "载具类型" ? "滑撬载具"
                                                         : src;
            obs.confidence = 1.0;
            obs.source_id  = src;
            obs.anchor_ref = kAnchor;
            obs.timestamp  = ts++;
            auto newly = sys.entity_bridge()->ingest_observation(obs);
            emerged.insert(emerged.end(), newly.begin(), newly.end());
        }
    }
    DEMO_CHECK(emerged.size() == 1, "新实体未浮现");
    if (!emerged.empty()) {
        std::printf("  浮现新实体:%s → EntityObserved 候选经四层校验结算(本体诞生)\n",
                    emerged[0].entity_id.c_str());
        DEMO_CHECK(sys.log().events_of_type("EntityObserved").size() == 1,
                   "EntityObserved 未结算");
        DEMO_CHECK(sys.projection().find(emerged[0].entity_id) != nullptr, "新本体未诞生");
    }

    // ========================================================================
    section("7. 读侧:44 个视图全清单渲染 + 重点视图详打 + AS OF 对比");
    // ========================================================================
    std::printf("-- 全清单(46 视图的 44 个机制实例;F1/F3/F5/F6/H1/H2/E6-E8 见 README 机制说明)--\n");
    const std::vector<std::string> all_views = {
        "V-ORDER-A1", "V-SEQ-A2", "V-PLAN-A3", "V-STATION-A4", "V-MONITOR-A5",
        "V-EFF-A6", "V-REPORT-A7",
        "V-MAT-B1", "V-BOM-B2?entity=工位01", "V-STOCK-B3", "V-PKE-B4",
        "V-TRACE-B5?entity=VIN-LBV0001", "V-CALL-B6", "V-KANBAN-B7", "V-PULL-B8",
        "V-JIS-B9", "V-JIT-B10", "V-LINESIDE-B11",
        "V-AVI-C1", "V-TRACK-C2?entity=VIN-LBV0002", "V-ZONE-C3",
        "V-ANDON-D1", "V-REPORT-D2", "V-CALLLOG-D3",
        "V-PMC-E1", "V-COUNT-E2", "V-CHART-E3", "V-FAULT-E4", "V-ALARM-E5", "V-ALARM-F15",
        "V-PLAN-F2", "V-TASK-F4", "V-CAR-F7?entity=VIN-LBV0001",
        "V-CARD-F8?entity=VIN-LBV0001", "V-HIST-F9?entity=VIN-LBV0002", "V-DEFECT-F10",
        "V-REWORK-001", "V-QUALREC-F12?entity=VIN-LBV0001", "V-LOCK-F13",
        "V-TRACE-F14?entity=VIN-LBV0001", "V-QUALITY-F16",
        "V-EQ-G1", "V-EQ-G2?entity=EQ-NG01", "V-INTEG-H3",
    };
    for (const std::string& v : all_views) print_view_digest(get_view("/views/" + v));

    std::printf("\n-- 重点视图详打 --\n");
    print_terminal_view("V-ORDER-A1 生产订单接收列表", get_view("/views/V-ORDER-A1"));
    std::printf("\n");
    print_terminal_view("V-SEQ-A2 生产排序视图", get_view("/views/V-SEQ-A2"));
    std::printf("\n");
    print_terminal_view("V-PLAN-A3 作业计划下发队列(observer=计划员)",
                        get_view("/views/V-PLAN-A3?observer=计划员"));
    std::printf("\n");
    const json vpke = get_view("/views/V-PKE-B4");
    print_intercept_view("V-PKE-B4 防错防漏校验", vpke);
    DEMO_CHECK(!vpke["rejections"].empty(), "B4 拦截视图为空");
    std::printf("\n");
    const json vb5 = get_view("/views/V-TRACE-B5?entity=VIN-LBV0001");
    print_traversal_view("V-TRACE-B5 物料追溯(VIN→批次→供应商)", vb5);
    {
        bool s_a1 = false, s_a2 = false, s_sup = false;
        for (const json& n : vb5["nodes"]) {
            if (n["id"] == "BATCH-A1") s_a1 = true;
            if (n["id"] == "BATCH-A2") s_a2 = true;
            if (n["id"] == "SUP-BOSCH") s_sup = true;
        }
        DEMO_CHECK(s_a1 && s_a2 && s_sup, "B5 遍历图缺节点(VIN→2 批次→供应商)");
    }
    std::printf("\n");
    print_terminal_view("V-CALL-B6 缺料呼叫看板(规则决定行:只出呼叫中)",
                        get_view("/views/V-CALL-B6"));
    DEMO_CHECK(get_view("/views/V-CALL-B6")["rows"].size() == 1, "B6 应只剩 1 行呼叫中");
    std::printf("\n");
    print_terminal_view("V-ANDON-D1 安灯状态大屏", get_view("/views/V-ANDON-D1"));
    std::printf("\n");
    print_flow_view("V-ALARM-F15 逐级报警列表", get_view("/views/V-ALARM-F15"));
    std::printf("\n");
    print_flow_view("V-REWORK-001 返修流程(observer=复检员)",
                    get_view("/views/V-REWORK-001?observer=复检员"));
    std::printf("\n");
    print_flow_view("V-HIST-F9 车辆位置历史(VIN-LBV0002)",
                    get_view("/views/V-HIST-F9?entity=VIN-LBV0002"));
    std::printf("\n");
    print_traversal_view("V-TRACE-F14 车辆追溯(VIN-LBV0001)",
                         get_view("/views/V-TRACE-F14?entity=VIN-LBV0001"));
    std::printf("\n");
    print_terminal_view("V-EFF-A6 生产效率分析", get_view("/views/V-EFF-A6"));
    std::printf("\n");
    print_terminal_view("V-QUALITY-F16 质量综合报表", get_view("/views/V-QUALITY-F16"));
    std::printf("\n");
    print_intercept_view("V-DEFECT-F10 缺陷采集", get_view("/views/V-DEFECT-F10"));

    std::printf("\n-- AS OF t:回到首个过点时刻(t=#%lld)对比当前 --\n", (long long)t_mid);
    const json as_of = get_view("/views/V-AVI-C1?t=" + std::to_string(t_mid));
    std::printf("  AS OF #%lld: V-AVI-C1 行数=%zu(当时只有首车刚进工位01)\n",
                (long long)t_mid, as_of["rows"].size());
    const json now = get_view("/views/V-AVI-C1");
    std::printf("  当前      : V-AVI-C1 行数=%zu\n", now["rows"].size());
    DEMO_CHECK(as_of["rows"].size() < now["rows"].size(), "AS OF t 未生效");

    // ========================================================================
    section("8. 收尾:事件/拦截/快照 + 崩溃恢复 + 重放逐比特一致 + 幂等重放");
    // ========================================================================
    std::printf("  事件总数:%lld\n", (long long)sys.log().size());
    std::printf("  RejectionLog 拦截条数:%zu\n", sys.rejections().recent(256).size());
    const auto snap_latest = snapshots.latest();
    DEMO_CHECK(snap_latest.has_value(), "定距快照缺失");
    if (snap_latest)
        std::printf("  最近快照:截至 event #%lld(间隔 20)\n", (long long)snap_latest->first);

    const std::string h0 = sys.projection().hash();
    bool replay_ok = true;
    for (int i = 0; i < 3; ++i) {  // 同输入重放 3 次,逐比特一致(最硬指标,含派生)
        if (sys.pipeline().replay_projection(sys.log()).hash() != h0) replay_ok = false;
    }
    std::printf("  投影哈希 ×3 重放:%s(%s)\n", replay_ok ? "逐比特一致" : "不一致!",
                h0.c_str());
    DEMO_CHECK(replay_ok, "重放逐比特一致被打破");

    std::printf("  轨迹 trajectory_of(VIN-LBV0002):\n");
    for (const auto& [seq, anchor] : sys.spacetime().trajectory_of("VIN-LBV0002"))
        std::printf("    #%lld  %s\n", (long long)seq, anchor.c_str());

    server.stop();  // 模拟崩溃:进程内系统丢弃,只用同一个 db 重建
    std::printf("-- 崩溃恢复:同一 db 重建 System(最近快照 + 增量重放)--\n");
    std::string h1;
    int64_t recovered_events = 0;
    {
        storage::SqliteBackend backend2("mse_demo.db");
        mse::System            sys2(backend2);  // 定义/事件/快照全部从 backend 恢复
        recovered_events = sys2.log().size();
        h1 = sys2.projection().hash();
        std::printf("  恢复后事件总数:%lld,投影哈希:%s\n", (long long)recovered_events,
                    h1.c_str());
        std::printf("  与崩溃前哈希一致:%s\n", h1 == h0 ? "是" : "否");
        DEMO_CHECK(h1 == h0, "崩溃恢复后哈希不一致");

        std::printf("-- 幂等键重放:重复提交 ERP 订单(跨进程,持久化命中)--\n");
        const json dup = {{"type", "OrderReceived"}, {"id", "VIN-LBV0001"},
                          {"actor", "erp"},           {"车型", "SUV-A"},
                          {"订单状态", "接收"},       {"交付期", "2026-09-10"},
                          {"idempotency_key", "erp-order-0001"}};
        const json rd = sys2.api().post_events(dup);
        DEMO_CHECK(settled(rd), "幂等重放未返回原回执");
        DEMO_CHECK(rd["event_id"] == r_o1["event_id"], "幂等重放 event_id 不一致");
        DEMO_CHECK(sys2.log().size() == recovered_events, "幂等重放日志不应增长");
        std::printf("  重复提交返回原 event_id=%lld,日志未增长\n",
                    (long long)rd["event_id"].get<int64_t>());
    }

    // ========================================================================
    section(g_failures == 0 ? "演示完成:世界模型在车间有效" : "演示完成:存在红线被打破");
    // ========================================================================
    std::printf("  数据库:mse_demo.db(事件/定义/快照/幂等)、mse_demo_voxels.db(时空)\n");
    std::printf("  退出码:%d\n", g_failures == 0 ? 0 : 1);
    return g_failures == 0 ? 0 : 1;
}
