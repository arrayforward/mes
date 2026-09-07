// ============================================================================
// mse/tests/test_main.cpp —— 红线测试(极简自带框架,memory 后端)
//
// 覆盖 docs/mes 的硬性红线:
//   1. 重放逐比特一致(投影哈希 ×3 + 与原投影一致)
//   2. 四层校验各自拦截(L0 未登记键 / L1 空 writes / L2 缺必填·未授权·值域外
//      / L3 规则拒绝:05 调序、锁定车报工)
//   3. 修正链(原事件永在日志、corrections_of、流水 L1+L2 同列、终态正确)
//   4. 读写一致性(视图按钮 enabled ⇔ 同类型同本体 submit 能结算)
//   5. 定义热更新(新属性+新事件类型不停机生效、def_versions 递增、引用未登记
//      键的定义候选被拒)
//   6. 时空(anchor_matches 正反、轨迹/当前锚点、anchor_as_of 历史版本)
//   7. 规则 DSL 单测(纯函数:算术/in/if/emit(var/cand/write/__target))
//   8. HTTP 端到端(真实 socket POST /events、GET /views/{id}、幂等键)
//   9. knowledge 协助(属性提取出 {车漆:划痕}、suggest_followups 映射返修)
//  10. entity 演化桥(同一锚点多源观测 → 新实体浮现 → EntityObserved 结算)
//  11. WASM 规则沙盒(base64、白名单外导入烘焙被拒、种子全链路:01 可结算/
//      05 被 WASM 规则拒、artifact_hash 内容寻址)
//  18. B5 物料追溯遍历(VIN→2 批次→供应商,节点边齐全)
//  19. B4 防错拦截视图(扫错料进 rejections,扫对通过)
//  20. F15 逐级报警链(同检点 3 次→科长、10 次→部长,derive null→0 起步)
//  21. B7-B10 规则决定行(consumers=read 行过滤,各拉动视图只出本类型)
//  22. 冻结禁调序/换单(R-SEQ-FROZEN,解冻后恢复)
//  23. 达成率 derive(冲正回摆 + 计划产量=0 → null 除零保护)
//  24. WASM 规则接入类型后 05 调序双拦截(理由含 R-PLAN-ADJUST-WASM)
// ============================================================================

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "entity/resolver.h"
#include "entitytree/backends/memory_store.h"
#include "entitytree/model.h"
#include "mse/http_server.h"
#include "mse/seeds.h"
#include "mse/system.h"
#include "mse/wasm_sandbox.h"
#include "storage/backends/memory_backend.h"

using nlohmann::json;

// ---- 极简测试框架:失败计数,main 返回非零 ----
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  [FAIL] %s:%d: CHECK(%s)\n", __FILE__, __LINE__,       \
                        #cond);                                                  \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++g_checks;                                                              \
        const auto va = (a);                                                     \
        const auto vb = (b);                                                     \
        if (!(va == vb)) {                                                       \
            ++g_failures;                                                        \
            std::printf("  [FAIL] %s:%d: CHECK_EQ(%s, %s)\n", __FILE__,          \
                        __LINE__, #a, #b);                                       \
        }                                                                        \
    } while (0)

static void banner(const char* name) { std::printf("== %s ==\n", name); }

// ---- 装配:memory 后端 + 系统键 + 整车厂种子 ----
struct Fixture {
    storage::MemoryBackend backend;
    mse::System            sys;

    Fixture() : sys(backend) {
        mse::load_system_keys(sys.defs());
        mse::load_auto_plant_seeds(sys.defs());
    }
};

// 便捷提交:单目标、可选锚点
static mse::Receipt post(mse::System& s, const std::string& type,
                         const std::string& id, json kvs,
                         const std::string& actor = "tester",
                         const std::string& anchor = "") {
    mse::Candidate c;
    c.type  = type;
    c.actor = actor;
    c.writes[id] = std::move(kvs);
    c.space = mse::SpaceRef::anchor_ref(anchor);
    return s.pipeline().submit(c);
}

static bool settled(const mse::Receipt& r) {
    return r.status == mse::Receipt::Status::kSettled;
}
static bool rejected_at(const mse::Receipt& r, int layer) {
    return r.status == mse::Receipt::Status::kRejected && r.layer == layer;
}

// 把车从"无计划状态"逐格开到 target(1..5),沿途每步都应结算
static void drive_plan_to(mse::System& s, const std::string& vin, int target) {
    static const char* kStates[] = {"01", "02", "03", "04", "05"};
    for (int i = 0; i < target; ++i) {
        const mse::Receipt r =
            post(s, "PlanReleased", vin, {{"计划状态", kStates[i]}}, "planner");
        CHECK(settled(r));
    }
}

// ============================================================================
// 1. 重放逐比特一致
// ============================================================================
static void test_replay_bit_identical() {
    banner("1. 重放逐比特一致");
    Fixture f;
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-R1", {{"车型", "SUV-A"}})));
    drive_plan_to(f.sys, "VIN-R1", 3);
    CHECK(settled(post(f.sys, "VehicleEnteredZone", "VIN-R1",
                       {{"过点区域", "工位01"}}, "avi",
                       "整车厂/总装车间/总装线/工位01")));
    CHECK(settled(post(f.sys, "DefectRegistered", "VIN-R1",
                       {{"车漆", "划痕"}, {"缺陷级别", 1}}, "qc")));

    const std::string h0 = f.sys.projection().hash();
    CHECK_EQ(h0.size(), 16u);
    for (int i = 0; i < 3; ++i) {  // 同输入重放 3 次,投影哈希一致(最硬指标)
        // 经管线统一重放入口(含派生属性):与在线结算同一条 fold 路径
        mse::Projection fresh = f.sys.pipeline().replay_projection(f.sys.log());
        CHECK_EQ(fresh.hash(), h0);
    }
}

// ============================================================================
// 2. 四层校验各自拦截
// ============================================================================
static void test_four_layers() {
    banner("2. 四层校验各自拦截");
    Fixture f;

    // [0] 字典登记:未登记键 → 系统不认识,拒绝
    CHECK(rejected_at(post(f.sys, "OrderReceived", "VIN-L0",
                           {{"车型", "SUV-A"}, {"未登记键", "x"}}),
                      0));

    // [1] 可聚合性:空 writes → 无法落地
    {
        mse::Candidate c;
        c.type  = "OrderReceived";
        c.actor = "tester";  // writes 空:不含 id 键的变化描述
        CHECK(rejected_at(f.sys.pipeline().submit(c), 1));
    }

    // [2] 类型 schema:缺必填键 / 未授权写键 / 值域外值
    CHECK(rejected_at(post(f.sys, "OrderReceived", "VIN-L2a", json::object()), 2));
    CHECK(rejected_at(post(f.sys, "OrderReceived", "VIN-L2b",
                           {{"车型", "SUV-A"}, {"锁定状态", "已锁"}}),
                      2));  // 锁定状态 writers 不含 OrderReceived
    CHECK(rejected_at(post(f.sys, "PlanReleased", "VIN-L2c", {{"计划状态", "99"}}),
                      2));  // 值越出值域

    // [3] 规则过滤:计划状态 05 时 SequenceAdjusted 被 R-PLAN-ADJUST 拒绝
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-L3a", {{"车型", "SUV-A"}})));
    drive_plan_to(f.sys, "VIN-L3a", 5);
    const mse::Receipt r3a =
        post(f.sys, "SequenceAdjusted", "VIN-L3a", {{"优先级", 1}}, "planner");
    CHECK(rejected_at(r3a, 3));

    // [3] 规则过滤:缺陷级别 ≥2 触发锁定后,ProductionReported 被 R-QUAL-LOCK 拒绝
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-L3b", {{"车型", "Sedan-B"}})));
    CHECK(settled(post(f.sys, "DefectRegistered", "VIN-L3b",
                       {{"车漆", "划痕"}, {"缺陷级别", 2}}, "qc")));
    // 触发链:VehicleLocked 由 R-QUAL-TRIGGER 自动结算
    CHECK_EQ(f.sys.log().events_of_type("VehicleLocked").size(), 1u);
    const mse::Receipt r3b =
        post(f.sys, "ProductionReported", "VIN-L3b", {{"报工数量", 1}}, "op");
    CHECK(rejected_at(r3b, 3));

    // 对照:合法提交都能结算(01 调序、未锁车报工)
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-OK", {{"车型", "SUV-A"}})));
    CHECK(settled(post(f.sys, "PlanReleased", "VIN-OK", {{"计划状态", "01"}}, "planner")));
    CHECK(settled(post(f.sys, "SequenceAdjusted", "VIN-OK", {{"优先级", 1}}, "planner")));
    CHECK(settled(post(f.sys, "ProductionReported", "VIN-OK", {{"报工数量", 1}}, "op")));
}

// ============================================================================
// 3. 修正链
// ============================================================================
static void test_correction_chain() {
    banner("3. 修正链:DefectRegistered → DefectCancelled");
    Fixture f;
    const mse::Receipt r1 = post(f.sys, "DefectRegistered", "VIN-C1",
                                 {{"车漆", "划痕"}, {"缺陷级别", 1}}, "qc");
    CHECK(settled(r1));
    const int64_t e1 = *r1.event_id;

    mse::Candidate fix;
    fix.type  = "DefectCancelled";
    fix.actor = "qc";
    fix.writes["VIN-C1"] = {{"车漆", "完好"}};
    fix.corrects = e1;  // 因果引用:被修正的原事件 id
    const mse::Receipt r2 = f.sys.pipeline().submit(fix);
    CHECK(settled(r2));
    const int64_t e2 = *r2.event_id;

    // 原事件永在日志(append-only,已结算事件永不修改)
    const auto orig = f.sys.log().get(e1);
    CHECK(orig.has_value());
    CHECK_EQ(orig->type, "DefectRegistered");
    CHECK_EQ(orig->writes.at("VIN-C1").at("车漆"), json("划痕"));

    // corrections_of 找得到修正事件
    const auto corr = f.sys.log().corrections_of(e1);
    CHECK_EQ(corr.size(), 1u);
    CHECK_EQ(corr[0].event_id, e2);

    // 流水视图 L1+L2:原事件与修正同列可见
    mse::ViewParams p;
    p.entity = "VIN-C1";
    const json view = f.sys.views().render("V-REWORK-001", p);
    bool saw_original = false, saw_correction = false;
    for (const json& row : view["rows"]) {
        if (row["event_id"] == e1) {
            saw_original = true;
            CHECK_EQ(row["is_correction"], false);
        }
        if (row["event_id"] == e2) {
            saw_correction = true;
            CHECK_EQ(row["is_correction"], true);
            CHECK_EQ(row["corrects"], json(e1));
        }
    }
    CHECK(saw_original);
    CHECK(saw_correction);

    // 终态正确:修正后车漆 = 完好
    CHECK_EQ(f.sys.projection().attrs_of("VIN-C1").at("车漆"), json("完好"));
}

// ============================================================================
// 4. 读写一致性:按钮 enabled ⇔ 同类型同本体 submit 能结算
// ============================================================================
static void test_read_write_consistency() {
    banner("4. 读写一致性(V-PLAN-A3 × SequenceAdjusted)");
    Fixture f;
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-W1", {{"车型", "SUV-A"}})));
    CHECK(settled(post(f.sys, "PlanReleased", "VIN-W1", {{"计划状态", "01"}}, "planner")));
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-W5", {{"车型", "Sedan-B"}})));
    drive_plan_to(f.sys, "VIN-W5", 5);

    mse::ViewParams p;
    p.observer = "计划员";
    const json view = f.sys.views().render("V-PLAN-A3", p);

    std::map<std::string, bool> adjust_enabled;  // 行 id → SequenceAdjusted 按钮可用性
    for (const json& row : view["rows"]) {
        for (const json& btn : row["buttons"]) {
            if (btn["type"] == "SequenceAdjusted")
                adjust_enabled[row["id"]] = btn["enabled"].get<bool>();
        }
    }
    CHECK_EQ(adjust_enabled.count("VIN-W1"), 1u);  // 01 行可见
    CHECK_EQ(adjust_enabled.count("VIN-W5"), 1u);  // 05 行可见(不隐藏,按钮置灰)
    CHECK(adjust_enabled["VIN-W1"]);               // 01:enabled
    CHECK(!adjust_enabled["VIN-W5"]);              // 05:disabled

    // 同一类型同一本体:enabled=true 的可结算;enabled=false 的被拒(第 3 层)
    CHECK(settled(post(f.sys, "SequenceAdjusted", "VIN-W1", {{"优先级", 1}}, "planner")));
    CHECK(rejected_at(post(f.sys, "SequenceAdjusted", "VIN-W5", {{"优先级", 1}}, "planner"),
                      3));
}

// ============================================================================
// 5. 定义热更新
// ============================================================================
static void test_definition_hot_update() {
    banner("5. 定义热更新");
    Fixture f;
    const mse::DefVersions v0 = f.sys.defs().versions();

    // 运行中注册新属性(不停机)
    const mse::Receipt ra = f.sys.defs().settle_definition(
        "AttributeRegistered",
        {{"key", "班次"},       {"semantic", "生产班次(早/中/晚)"},
         {"datatype", "string"}, {"unit", ""},
         {"range", json::array()},{"writers", {"ShiftChanged"}},
         {"kind", "原生"},      {"rule_ref", ""}});
    CHECK(settled(ra));
    CHECK_EQ(f.sys.defs().versions().dict, v0.dict + 1);

    // 运行中注册新事件类型(引用已登记键,编译通过)
    const mse::Receipt rt = f.sys.defs().settle_definition(
        "EventTypeRegistered",
        {{"type", "ShiftChanged"},      {"required_keys", {"id", "actor", "班次"}},
         {"optional_keys", json::array()},{"rules", json::array()},
         {"correction", ""},            {"multi_target", false},
         {"settlement", "sync"}});
    CHECK(settled(rt));
    CHECK_EQ(f.sys.defs().versions().types, v0.types + 1);

    // 立即可用:新类型事件经四层校验结算
    CHECK(settled(post(f.sys, "ShiftChanged", "VIN-H1", {{"班次", "早班"}})));

    // 引用未登记键的定义候选被拒(未登记 = 编译不过)
    const mse::Receipt bad = f.sys.defs().settle_definition(
        "EventTypeRegistered",
        {{"type", "BadType"},              {"required_keys", {"id", "不存在的键"}},
         {"optional_keys", json::array()},{"rules", json::array()}});
    CHECK(!settled(bad));

    // deps 引用未登记键的规则候选同样被拒(静态检查)
    const mse::Receipt bad_rule = f.sys.defs().settle_definition(
        "RuleRegistered",
        {{"rule_id", "R-BAD"},  {"deps", {"不存在的键"}},
         {"effect", "filter"},  {"target_key", ""},
         {"logic", {{"pass", true}}},{"consumers", "both"}});
    CHECK(!settled(bad_rule));
}

// ============================================================================
// 6. 时空
// ============================================================================
static void test_spacetime() {
    banner("6. 时空树");
    using ST = mse::SpacetimeTree;

    // 精度匹配:查"总装线"命中"总装线中段"(末段前缀);正反用例
    CHECK(ST::anchor_matches("总装线", "总装线中段"));
    CHECK(!ST::anchor_matches("总装线", "焊装线中段"));
    CHECK(ST::anchor_matches("整车厂/总装车间", "整车厂/总装车间/总装线/工位01"));
    CHECK(!ST::anchor_matches("工位01", "工位02"));
    CHECK(ST::anchor_matches("", "任意锚点"));  // 空 query 命中一切
    CHECK(!ST::anchor_matches("整车厂/总装车间/总装线", "整车厂/总装车间"));  // 段数超出

    // 切片:锚点命中;模糊原文永不匹配锚点切片(原文只存档)
    CHECK(ST::in_slice("整车厂/总装车间",
                       mse::SpaceRef::anchor_ref("整车厂/总装车间/总装线/工位01")));
    CHECK(!ST::in_slice("整车厂/总装车间", mse::SpaceRef::fuzzy("左前门")));

    Fixture f;
    const char* kW1 = "整车厂/总装车间/总装线/工位01";
    const char* kW2 = "整车厂/总装车间/总装线/工位02";
    const mse::Receipt e1 = post(f.sys, "VehicleEnteredZone", "VIN-S1",
                                 {{"过点区域", "工位01"}}, "avi", kW1);
    const mse::Receipt e2 = post(f.sys, "VehicleEnteredZone", "VIN-S1",
                                 {{"过点区域", "工位02"}}, "avi", kW2);
    const mse::Receipt e3 = post(f.sys, "VehicleEnteredZone", "VIN-S2",
                                 {{"过点区域", "工位01"}}, "avi", kW1);
    const mse::Receipt e4 = post(f.sys, "VehicleEnteredZone", "VIN-S3",
                                 {{"过点区域", "工位01"}}, "avi", kW1);
    CHECK(settled(e1) && settled(e2) && settled(e3) && settled(e4));

    // 轨迹与当前锚点(AVI 位置历史)
    const auto traj = f.sys.spacetime().trajectory_of("VIN-S1");
    CHECK_EQ(traj.size(), 2u);
    CHECK_EQ(traj[0].first, *e1.event_id);
    CHECK_EQ(traj[0].second, std::string(kW1));
    CHECK_EQ(traj[1].second, std::string(kW2));
    const auto cur = f.sys.spacetime().current_anchor_of("VIN-S1");
    CHECK(cur.has_value());
    CHECK_EQ(*cur, std::string(kW2));

    // AS OF t:锚点块的历史版本逐版本可取回
    const auto v1 = f.sys.spacetime().anchor_as_of(kW1, *e1.event_id);
    const auto v3 = f.sys.spacetime().anchor_as_of(kW1, *e3.event_id);
    const auto v4 = f.sys.spacetime().anchor_as_of(kW1, *e4.event_id);
    CHECK(v1.has_value());
    CHECK(v3.has_value());
    CHECK(v4.has_value());
    if (v1 && v3 && v4) {
        CHECK_EQ((*v1)["events"].size(), 1u);
        CHECK_EQ((*v3)["events"].size(), 2u);
        CHECK_EQ((*v4)["events"].size(), 3u);
    }
}

// ============================================================================
// 7. 规则 DSL 单测(纯函数:构造输入,断言输出)
// ============================================================================
static void test_rule_dsl() {
    banner("7. 规则 DSL 单测");
    mse::RuleEngine eng;

    // 算术(derive 允许裸表达式):1 + 2*3 = 7
    {
        mse::Rule r;
        r.rule_id = "T-ARITH";
        r.effect  = "derive";
        r.target_key = "x";
        r.logic = json::parse(R"({"+":[1,{"*":[2,3]}]})");
        const mse::RuleOutcome o = eng.eval(r, json::object(), mse::Candidate{}, "T");
        CHECK(o.kind == mse::RuleOutcome::Kind::kValue);
        CHECK_EQ(o.value, json(7));
    }

    // in + if(var 取值)
    {
        mse::Rule r;
        r.rule_id = "T-IN";
        r.deps    = {"x"};
        r.effect  = "filter";
        r.logic   = json::parse(
            R"({"if":[{"in":[{"var":"x"},["a","b"]]},{"pass":true},{"reject":"不在值域"}]})");
        mse::Candidate c;
        c.type = "Any";
        const mse::RuleOutcome ok = eng.eval(r, {{"x", "a"}}, c, "T");
        CHECK(ok.kind == mse::RuleOutcome::Kind::kPass);
        const mse::RuleOutcome no = eng.eval(r, {{"x", "z"}}, c, "T");
        CHECK(no.kind == mse::RuleOutcome::Kind::kReject);
        CHECK_EQ(no.reason, "不在值域");
    }

    // if 链式 [c1,t1,c2,t2,...,else]
    {
        mse::Rule r;
        r.rule_id = "T-IF";
        r.deps    = {"n"};
        r.effect  = "derive";
        r.target_key = "y";
        r.logic = json::parse(
            R"({"if":[{">=":[{"var":"n"},3]},"大",{">=":[{"var":"n"},2]},"中","小"]})");
        const mse::RuleOutcome o = eng.eval(r, {{"n", 2}}, mse::Candidate{}, "T");
        CHECK_EQ(o.value, json("中"));
    }

    // cand / write / __target:emit 组装候选
    {
        mse::Rule r;
        r.rule_id = "T-EMIT";
        r.effect  = "trigger";
        r.logic   = json::parse(
            R"({"emit":{"type":{"cand":"type"},"id":{"cand":"__target"},
                          "k":{"write":"w"},"from":{"var":"src"}}})");
        mse::Candidate c;
        c.type = "DefectRegistered";
        c.writes["CAR-1"] = {{"w", 5}};
        const mse::RuleOutcome o = eng.eval(r, {{"src", "S"}}, c, "CAR-1");
        CHECK(o.kind == mse::RuleOutcome::Kind::kEmit);
        CHECK_EQ(o.emitted.size(), 1u);
        const mse::Candidate& em = o.emitted[0];
        CHECK_EQ(em.type, "DefectRegistered");            // {"cand":"type"}
        CHECK(em.writes.count("CAR-1") == 1);             // {"cand":"__target"}
        CHECK_EQ(em.writes.at("CAR-1").at("k"), json(5)); // {"write":"w"}
        CHECK_EQ(em.writes.at("CAR-1").at("from"), json("S"));
        CHECK_EQ(em.actor, "rule:T-EMIT");                // actor 缺省回填
    }

    // 静态检查:var 引用 deps 未声明的键 → 问题清单非空
    {
        mse::Rule r;
        r.rule_id = "T-BAD";
        r.deps    = {"x"};
        r.effect  = "filter";
        r.logic   = json::parse(R"({"if":[{"var":"y"},{"pass":true},{"reject":"no"}]})");
        const auto problems =
            mse::RuleEngine::static_check(r, [](const std::string&) { return true; });
        CHECK(!problems.empty());
    }
}

// ============================================================================
// 8. HTTP 端到端(真实 socket)
// ============================================================================
static void test_http_end_to_end() {
    banner("8. HTTP 端到端");
    Fixture f;
    mse::HttpServer srv;
    const bool ok = srv.listen_on("127.0.0.1", 0, [&f](const mse::HttpRequest& req) {
        if (req.method == "POST" && req.path == "/events") {
            const json payload = json::parse(req.body, nullptr, false);
            if (payload.is_discarded())
                return mse::HttpResponse{400, "application/json; charset=utf-8",
                                         "{\"error\":\"bad json\"}"};
            return mse::HttpResponse{200, "application/json; charset=utf-8",
                                     f.sys.api().post_events(payload).dump()};
        }
        if (req.method == "GET" && req.path.rfind("/views/", 0) == 0) {
            return mse::HttpResponse{
                200, "application/json; charset=utf-8",
                f.sys.api().get_view(req.path.substr(7), req.query).dump()};
        }
        return mse::HttpResponse{404, "application/json; charset=utf-8",
                                 "{\"error\":\"not found\"}"};
    });
    CHECK(ok);
    const uint16_t port = srv.port();
    CHECK(port != 0);

    // POST /events:结算回执
    const json payload = {{"type", "OrderReceived"},
                          {"id", "VIN-HTTP"},
                          {"actor", "erp"},
                          {"车型", "SUV-A"},
                          {"idempotency_key", "erp-order-0001"}};
    const auto [s1, b1] =
        mse::http_request("127.0.0.1", port, "POST", "/events", payload.dump());
    CHECK_EQ(s1, 200);
    const json r1 = json::parse(b1);
    CHECK_EQ(r1["status"], "settled");
    const int64_t eid = r1["event_id"].get<int64_t>();
    CHECK(eid >= 1);

    // 幂等键:重复 POST 返回同一 event_id,日志只长一条
    const int64_t size_before = f.sys.log().size();
    const auto [s2, b2] =
        mse::http_request("127.0.0.1", port, "POST", "/events", payload.dump());
    CHECK_EQ(s2, 200);
    const json r2 = json::parse(b2);
    CHECK_EQ(r2["event_id"], json(eid));
    CHECK_EQ(f.sys.log().size(), size_before);  // 第一次已长一条,重复提交不再长

    // GET /views/{id}:渲染 JSON 含该车行
    const auto [s3, b3] =
        mse::http_request("127.0.0.1", port, "GET", "/views/V-PLAN-A3?observer=计划员");
    CHECK_EQ(s3, 200);
    const json view = json::parse(b3);
    CHECK_EQ(view["view_id"], "V-PLAN-A3");
    bool found = false;
    for (const json& row : view["rows"])
        if (row["id"] == "VIN-HTTP") found = true;
    CHECK(found);

    // 未知路径 404
    const auto [s4, b4] = mse::http_request("127.0.0.1", port, "GET", "/nope");
    CHECK_EQ(s4, 404);
    (void)b4;

    srv.stop();
}

// ============================================================================
// 9. knowledge 协助
// ============================================================================
static void test_knowledge_assist() {
    banner("9. knowledge 协助");
    Fixture f;
    mse::KnowledgeAssist& ka = f.sys.knowledge();
    CHECK(ka.load(std::string(MSE_ASSETS_DIR) + "/ontology_seed.json",
                  std::string(MSE_ASSETS_DIR) + "/lexicon.json"));
    CHECK(ka.loaded());

    // 属性提取:自由文本 → 建议 {车漆: 划痕}(只产出已登记且值域内的键值)
    std::vector<mse::AttributeEntry> entries;
    for (const std::string& k : f.sys.defs().attr_keys())
        if (const mse::AttributeEntry* e = f.sys.defs().find_attr(k)) entries.push_back(*e);
    const auto sug = ka.extract_attributes("左前门漆面有明显划痕", entries);
    CHECK(sug.count("车漆") == 1);
    if (sug.count("车漆") == 1) CHECK_EQ(sug.at("车漆"), json("划痕"));

    // 规则协助推理:"划痕" 经前向链映射出已注册的 ReworkRequested(仅建议)
    const auto fol = ka.suggest_followups("划痕", f.sys.defs().event_type_names());
    bool has_rework = false;
    for (const std::string& t : fol)
        if (t == "ReworkRequested") has_rework = true;
    CHECK(has_rework);
}

// ============================================================================
// 10. entity 演化桥:非人力的实体创建
// ============================================================================
static void test_entity_bridge() {
    banner("10. entity 演化桥");
    Fixture f;
    entitytree::MemoryEntityStore store;
    entity::Resolver            resolver(store);  // 默认阈值:θ_form=3.0
    f.sys.attach_entity_bridge(resolver, store);
    mse::EntityEvolutionBridge* bridge = f.sys.entity_bridge();
    CHECK(bridge != nullptr);

    // 同一锚点、多源(2)×多属性(3)观测 → info_score = 3×log1p(2) ≈ 3.30 ≥ θ_form
    const char*      kAnchor = "整车厂/总装车间/总装线/工位02";
    const char*      keys[]  = {"观测标识", "观测来源", "载具类型"};
    const char*      srcs[]  = {"rfid-gate-01", "rfid-gate-02"};
    std::vector<mse::EmergedEntity> emerged;
    int64_t         ts = 1000;
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
            auto newly = bridge->ingest_observation(obs);
            emerged.insert(emerged.end(), newly.begin(), newly.end());
        }
    }

    // 新实体浮现(非人力创建),且只浮现一次
    CHECK_EQ(emerged.size(), 1u);
    // EntityObserved 候选经四层校验结算(首个携带该 id 键的事件 = 本体诞生)
    const auto observed = f.sys.log().events_of_type("EntityObserved");
    CHECK_EQ(observed.size(), 1u);
    if (!emerged.empty()) {
        const mse::Ontology* ont = f.sys.projection().find(emerged[0].entity_id);
        CHECK(ont != nullptr);  // 新本体诞生
        if (ont != nullptr) {
            CHECK(ont->attrs.count("观测标识") == 1);
            if (ont->attrs.count("观测标识") == 1)
                CHECK_EQ(ont->attrs.at("观测标识").value, json("CARRIER-777"));
        }
        if (!observed.empty()) CHECK(observed[0].writes.count(emerged[0].entity_id) == 1);
    }
}

// ============================================================================
// 11. WASM 规则沙盒(烘焙管线 + 无环境静态闸 + 全链路结算)
// ============================================================================
static std::string read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void test_wasm_sandbox() {
    banner("11. WASM 规则沙盒");
    const std::string assets = MSE_ASSETS_DIR;

    // base64 编解码:往返一致 + 非法字符置 ok=false
    {
        bool        ok = false;
        const std::string raw("\x00\x01\xff" "wasm", 7);
        const std::string rt = mse::base64_decode(mse::base64_encode(raw), ok);
        CHECK(ok);
        CHECK(rt == raw);
        ok = true;
        (void)mse::base64_decode("A!=/", ok);
        CHECK(!ok);
    }

    // ① 烘焙静态闸:声明白名单外导入(env.clock)= 环境能力伸手 → 定义候选被拒
    {
        const std::string evil = read_file_bytes(assets + "/wasm/evil_clock.wasm");
        CHECK(!evil.empty());
        const auto baked = mse::WasmSandbox::bake(
            mse::base64_encode(evil), {}, "mse_eval",
            [](const std::string&) { return true; });
        CHECK(!baked.ok);
        bool mentions_env = false;
        for (const auto& e : baked.errors) {
            if (e.find("env.clock") != std::string::npos) mentions_env = true;
        }
        CHECK(mentions_env);  // 拒绝理由点名伸手的环境导入
    }

    // 种子注册:烘焙管线在定义结算内完成
    Fixture f;
    mse::load_wasm_seeds(f.sys.defs(), assets);
    const mse::Rule* wr = f.sys.defs().find_rule("R-PLAN-ADJUST-WASM");
    CHECK(wr != nullptr);
    if (wr == nullptr) return;
    CHECK_EQ(wr->runtime, std::string("wasm"));
    CHECK_EQ(wr->artifact_hash.size(), 16u);
    CHECK(wr->engine_version.find("wasm3 ") == 0);  // 引擎版本钉死

    // ③ 内容寻址:同一产物烘焙两次,artifact_hash 相同,且与结算钉下的一致
    {
        const std::string bytes = read_file_bytes(assets + "/wasm/plan_adjust.wasm");
        CHECK(!bytes.empty());
        const auto b1 = mse::WasmSandbox::bake(
            mse::base64_encode(bytes), {"计划状态"}, "mse_eval",
            [](const std::string&) { return true; });
        const auto b2 = mse::WasmSandbox::bake(
            mse::base64_encode(bytes), {"计划状态"}, "mse_eval",
            [](const std::string&) { return true; });
        CHECK(b1.ok);
        CHECK(b2.ok);
        CHECK_EQ(b1.artifact_hash, b2.artifact_hash);
        CHECK_EQ(b1.artifact_hash, wr->artifact_hash);
    }

    // ② 全链路:SequenceAdjusted 换引 WASM 规则(定义热更新,同类型新版本),
    //    写侧 L3 由 WASM 沙盒给出结论
    const auto tr = f.sys.defs().settle_definition(
        "EventTypeRegistered",
        {{"type", "SequenceAdjusted"},
         {"required_keys", {"id", "actor"}},
         {"optional_keys", {"优先级"}},
         {"rules", {"R-PLAN-ADJUST-WASM"}},
         {"correction", "SequenceAdjustReversed"},
         {"multi_target", false},
         {"settlement", "sync"}});
    CHECK(settled(tr));

    // 01 可调:SequenceAdjusted 可结算(WASM 规则 pass)
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-W1", {{"车型", "SUV-A"}})));
    drive_plan_to(f.sys, "VIN-W1", 1);
    CHECK(settled(post(f.sys, "SequenceAdjusted", "VIN-W1", {{"优先级", 1}}, "planner")));

    // 05 绝对禁止:L3 被拒,理由来自 WASM 规则
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-W2", {{"车型", "SUV-A"}})));
    drive_plan_to(f.sys, "VIN-W2", 5);
    const mse::Receipt rj =
        post(f.sys, "SequenceAdjusted", "VIN-W2", {{"优先级", 1}}, "planner");
    CHECK(rejected_at(rj, 3));
    bool from_wasm = false;
    for (const auto& v : rj.violations) {
        if (v.find("R-PLAN-ADJUST-WASM") != std::string::npos) from_wasm = true;
    }
    CHECK(from_wasm);
}

// ============================================================================
// 12. 定距快照与崩溃恢复
// ============================================================================
static void test_snapshots() {
    banner("12. 定距快照与崩溃恢复");
    storage::MemoryBackend backend;
    mse::System           sys(backend);
    mse::load_system_keys(sys.defs());
    mse::load_auto_plant_seeds(sys.defs());

    mse::SnapshotStore store(backend);
    sys.pipeline().set_snapshots(&store, 5);  // 每 5 条已结算事件落一份快照

    // 10 条事件:快照应落在 event 5 与 10
    CHECK(settled(post(sys, "OrderReceived", "VIN-SN1", {{"车型", "SUV-A"}})));
    for (int i = 0; i < 9; ++i)
        CHECK(settled(post(sys, "VehicleEnteredZone", "VIN-SN1",
                           {{"过点区域", "工位01"}}, "avi")));

    const auto latest = store.latest();
    CHECK(latest.has_value());
    if (latest) {
        CHECK_EQ(latest->first, 10);  // 最近快照截至 event 10
        // load_snapshot 重建的投影与在线投影逐比特一致
        mse::Projection p2;
        p2.load_snapshot(latest->second);
        CHECK_EQ(p2.hash(), sys.projection().hash());
    }

    // 再补 2 条(共 12):latest 仍停在 10
    CHECK(settled(post(sys, "VehicleEnteredZone", "VIN-SN1",
                       {{"过点区域", "工位02"}}, "avi")));
    CHECK(settled(post(sys, "DefectRegistered", "VIN-SN1",
                       {{"车漆", "划痕"}, {"缺陷级别", 1}}, "qc")));
    const auto latest2 = store.latest();
    CHECK(latest2.has_value());
    if (latest2) CHECK_EQ(latest2->first, 10);

    // 崩溃恢复:同一 backend 新开 System(最近快照 + 增量重放),哈希一致
    const std::string h_online = sys.projection().hash();
    {
        mse::System sys2(backend);  // 定义/事件/快照全部从 backend 恢复
        CHECK_EQ(sys2.projection().hash(), h_online);
        CHECK_EQ(sys2.log().size(), sys.log().size());
    }

    // 形状非法的快照抛 std::runtime_error
    bool threw = false;
    try {
        mse::Projection p3;
        p3.load_snapshot(json{{"not_ontologies", 1}});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

// ============================================================================
// 13. AS OF 快照加速:有快照 vs 无快照渲染同一视图,JSON 逐字节相等
// ============================================================================
static void test_as_of_snapshot_consistency() {
    banner("13. AS OF:有快照 vs 无快照渲染一致");
    storage::MemoryBackend b1, b2;
    mse::System            s1(b1), s2(b2);
    mse::SnapshotStore     store(b1);
    for (mse::System* s : {&s1, &s2}) {
        mse::load_system_keys(s->defs());
        mse::load_auto_plant_seeds(s->defs());
    }
    s1.pipeline().set_snapshots(&store, 2);  // s1 有快照;s2 无快照

    // 两系统走完全相同的事件流
    for (mse::System* s : {&s1, &s2}) {
        CHECK(settled(post(*s, "OrderReceived", "VIN-A1", {{"车型", "SUV-A"}})));
        drive_plan_to(*s, "VIN-A1", 5);
        CHECK(settled(post(*s, "OrderReceived", "VIN-A2", {{"车型", "Sedan-B"}})));
    }

    mse::ViewParams p;
    p.observer  = "计划员";
    p.as_of_seq = 5;  // floor(5) = 4:s1 走 快照@4 + 增量重放;s2 全量重放
    const json v1 = s1.views().render("V-PLAN-A3", p);
    const json v2 = s2.views().render("V-PLAN-A3", p);
    CHECK(!v1.contains("error"));
    CHECK(!v2.contains("error"));
    CHECK_EQ(v1.dump(), v2.dump());  // 行为不变,只是更快
}

// ============================================================================
// 14. 幂等键持久化:跨进程重启重复提交仍返回原回执
// ============================================================================
static void test_idempotency_persistence() {
    banner("14. 幂等键持久化(跨进程重启)");
    storage::MemoryBackend backend;
    const json payload = {{"type", "OrderReceived"},
                          {"id", "VIN-I1"},
                          {"actor", "erp"},
                          {"车型", "SUV-A"},
                          {"idempotency_key", "erp-order-0042"}};
    int64_t e1  = 0;
    int64_t sz1 = 0;
    {
        mse::System s1(backend);
        mse::load_system_keys(s1.defs());
        mse::load_auto_plant_seeds(s1.defs());
        const json r1 = s1.api().post_events(payload);
        CHECK_EQ(r1["status"], "settled");
        e1 = r1["event_id"].get<int64_t>();
        const json r2 = s1.api().post_events(payload);  // 同进程重复:内存命中
        CHECK_EQ(r2["event_id"], json(e1));
        sz1 = s1.log().size();
    }  // s1 析构,模拟进程退出
    {
        mse::System s2(backend);  // 新开 System:内存幂等表为空,走持久化命中
        const json r3 = s2.api().post_events(payload);
        CHECK_EQ(r3["status"], "settled");
        CHECK_EQ(r3["event_id"], json(e1));      // 仍返回原 event_id
        CHECK_EQ(s2.log().size(), sz1);          // 日志不增
    }
}

// ============================================================================
// 15. on_types 变化驱动:derive 计数规则只对列出的事件类型求值
// ============================================================================
static void test_on_types_derive() {
    banner("15. on_types 变化驱动(derive 计数)");
    Fixture f;
    // 字典:故障码(原生)+ 故障计数(计数器,writers="*" 允许初始置 0)
    CHECK(settled(f.sys.defs().settle_definition(
        "AttributeRegistered",
        {{"key", "故障码"},       {"semantic", "故障代码"}, {"datatype", "string"},
         {"unit", ""},            {"range", json::array()},
         {"writers", {"FaultAlarmed", "NoiseEvent"}},
         {"kind", "原生"},        {"rule_ref", ""}})));
    CHECK(settled(f.sys.defs().settle_definition(
        "AttributeRegistered",
        {{"key", "故障计数"},     {"semantic", "故障累计次数"}, {"datatype", "integer"},
         {"unit", "次"},          {"range", json::array()},     {"writers", {"*"}},
         {"kind", "原生"},        {"rule_ref", ""}})));
    // 规则:只在 FaultAlarmed 结算后求值;var null → 0 起步,+1 计数
    CHECK(settled(f.sys.defs().settle_definition(
        "RuleRegistered",
        {{"rule_id", "R-FAULT-COUNT"},
         {"deps", {"故障码", "故障计数"}},
         {"effect", "derive"},
         {"target_key", "故障计数"},
         {"logic", json::parse(R"({"return":{"+":[{"var":"故障计数"},1]}})")},
         {"consumers", "both"},
         {"on_types", {"FaultAlarmed"}}})));
    // 行为:FaultAlarmed 与 NoiseEvent 都写 故障码(dep_touched 均命中,
    // 只有 on_types 过滤把 NoiseEvent 排除在计数之外)
    for (const char* t : {"FaultAlarmed", "NoiseEvent"})
        CHECK(settled(f.sys.defs().settle_definition(
            "EventTypeRegistered",
            {{"type", t},                   {"required_keys", {"id", "actor", "故障码"}},
             {"optional_keys", json::array()},{"rules", json::array()},
             {"correction", ""},            {"multi_target", false},
             {"settlement", "sync"}})));

    // 初始置 0(派生键首次存在性由本事件建立)
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-F1",
                       {{"车型", "SUV-A"}, {"故障计数", 0}})));
    for (int i = 0; i < 3; ++i)
        CHECK(settled(post(f.sys, "FaultAlarmed", "VIN-F1", {{"故障码", "E01"}}, "qc")));
    CHECK(settled(post(f.sys, "NoiseEvent", "VIN-F1", {{"故障码", "E02"}}, "qc")));

    // 计数 == 3:NoiseEvent 不触发(on_types 过滤)
    CHECK_EQ(f.sys.projection().attrs_of("VIN-F1").at("故障计数"), json(3));
    // 重放一致:derive 钩子的 on_types 过滤在重放路径同样生效
    mse::Projection replayed = f.sys.pipeline().replay_projection(f.sys.log());
    CHECK_EQ(replayed.attrs_of("VIN-F1").at("故障计数"), json(3));
    CHECK_EQ(replayed.hash(), f.sys.projection().hash());

    // null 操作数按 0 处理(计数器初值语义):var 取到 null 时 "旧值+1" 从 0 起步
    {
        mse::RuleEngine eng;
        mse::Rule       r;
        r.rule_id    = "T-NULL0";
        r.deps       = {"故障计数"};
        r.effect     = "derive";
        r.target_key = "故障计数";
        r.logic      = json::parse(R"({"+":[{"var":"故障计数"},1]})");
        const mse::RuleOutcome o =
            eng.eval(r, {{"故障计数", nullptr}}, mse::Candidate{}, "T");
        CHECK(o.kind == mse::RuleOutcome::Kind::kValue);
        CHECK_EQ(o.value, json(1));
    }
}

// ============================================================================
// 16. 遍历视图(ref 双向 BFS + corrects 因果边)+ 17. ref 键 L2 类型校验
// ============================================================================
static void test_traversal_view() {
    banner("16. 遍历视图(ref 双向 BFS)");
    Fixture f;
    // 字典:VIN↔批次 双向 ref + 批次→供应商 ref
    auto reg_ref = [&f](const char* key, const char* semantic, const char* writer) {
        CHECK(settled(f.sys.defs().settle_definition(
            "AttributeRegistered",
            {{"key", key},          {"semantic", semantic}, {"datatype", "ref"},
             {"unit", ""},          {"range", json::array()}, {"writers", {writer}},
             {"kind", "原生"},      {"rule_ref", ""}})));
    };
    reg_ref("所属批次", "车辆所属生产批次(本体 id)", "批次绑定");
    reg_ref("绑定车辆", "批次绑定的车辆(本体 id)", "VIN绑定");
    reg_ref("供应商",   "批次的供应商(本体 id)",   "供应商登记");
    auto reg_type = [&f](const char* type, const char* key) {
        CHECK(settled(f.sys.defs().settle_definition(
            "EventTypeRegistered",
            {{"type", type},                {"required_keys", {"id", "actor", key}},
             {"optional_keys", json::array()},{"rules", json::array()},
             {"correction", ""},            {"multi_target", false},
             {"settlement", "sync"}})));
    };
    reg_type("批次绑定", "所属批次");
    reg_type("VIN绑定", "绑定车辆");
    reg_type("供应商登记", "供应商");
    CHECK(settled(f.sys.defs().settle_definition(
        "ViewRegistered",
        {{"view_id", "V-TRACE"},
         {"queries",
          {{"events", {"批次绑定", "VIN绑定", "供应商登记"}}, {"fold", "L1"}}},
         {"selects", {"所属批次", "绑定车辆", "供应商"}},
         {"rules", json::array()},
         {"emits", json::array()},
         {"render_mode", "遍历"}})));

    // 事件:VIN↔批次双向绑定 + 批次→供应商(两级链)
    CHECK(settled(post(f.sys, "批次绑定", "VIN-T1", {{"所属批次", "BATCH-T1"}})));
    CHECK(settled(post(f.sys, "VIN绑定", "BATCH-T1", {{"绑定车辆", "VIN-T1"}})));
    CHECK(settled(post(f.sys, "供应商登记", "BATCH-T1", {{"供应商", "SUP-T9"}})));

    // 从 VIN 出发:深度内到 批次(1 级)→ 供应商(2 级)
    mse::ViewParams p;
    p.entity = "VIN-T1";
    const json v = f.sys.views().render("V-TRACE", p);
    CHECK(!v.contains("error"));
    CHECK_EQ(v["render_mode"], "遍历");
    CHECK_EQ(v["root"], "VIN-T1");
    CHECK_EQ(v["nodes"].size(), 3u);  // VIN-T1 / BATCH-T1 / SUP-T9
    CHECK_EQ(v["edges"].size(), 3u);
    // nodes/edges 字典序:edges[0] 起点字典序最小
    CHECK_EQ(v["edges"][0]["from"], "BATCH-T1");
    bool saw_batch = false, saw_sup = false;
    for (const json& n : v["nodes"]) {
        if (n["id"] == "BATCH-T1") {
            saw_batch = true;
            CHECK_EQ(n["绑定车辆"], "VIN-T1");
            CHECK_EQ(n["供应商"], "SUP-T9");
        }
        if (n["id"] == "SUP-T9") saw_sup = true;
    }
    CHECK(saw_batch);
    CHECK(saw_sup);

    // 反向游走:从供应商出发,沿反向 ref 边命中批次与车辆
    mse::ViewParams pr;
    pr.entity = "SUP-T9";
    const json vr = f.sys.views().render("V-TRACE", pr);
    bool back_hit_vin = false;
    for (const json& n : vr["nodes"])
        if (n["id"] == "VIN-T1") back_hit_vin = true;
    CHECK(back_hit_vin);

    // 缺少 entity 起点 → {"error"}
    const json ve = f.sys.views().render("V-TRACE", mse::ViewParams{});
    CHECK(ve.contains("error"));

    // 17. ref 键 L2 粗校验:写非字符串值被拒
    CHECK(rejected_at(post(f.sys, "批次绑定", "VIN-T2", {{"所属批次", 123}}), 2));
}

// ============================================================================
// 18. B5 物料追溯遍历:VIN → 2 批次 → 供应商,节点边齐全
// ============================================================================
static void test_b5_traceability() {
    banner("18. B5 遍历:VIN→2 批次→供应商");
    Fixture f;
    // 批次登记(带供应商 ref)
    for (const char* b : {"BATCH-B1", "BATCH-B2"}) {
        CHECK(settled(post(f.sys, "MaterialRegistered", b,
                           {{"物料编号", "MAT-1001"}, {"批次号", b},
                            {"供应商", "SUP-B1"}}, "wms")));
    }
    // 一车绑两批(multi_target 显式 writes:VIN.批次绑定 + batch.VIN绑定 一次就位)
    auto bind = [&f](const char* vin, const char* batch) {
        mse::Candidate c;
        c.type  = "BatchBoundToVIN";
        c.actor = "logistics";
        c.writes[vin]   = {{"批次绑定", batch}};
        c.writes[batch] = {{"VIN绑定", vin}};
        CHECK(settled(f.sys.pipeline().submit(c)));
    };
    bind("VIN-B1", "BATCH-B1");
    bind("VIN-B1", "BATCH-B2");

    mse::ViewParams p;
    p.entity = "VIN-B1";
    const json v = f.sys.views().render("V-TRACE-B5", p);
    CHECK(!v.contains("error"));
    CHECK_EQ(v["render_mode"], "遍历");
    // 节点:VIN + 2 批次 + 供应商(终态 批次绑定 只留最近一批,另一批经反向
    // VIN绑定 边仍可达——绑定史在事件树,遍历是双向的)
    CHECK_EQ(v["nodes"].size(), 4u);
    bool s_vin = false, s_b1 = false, s_b2 = false, s_sup = false;
    for (const json& n : v["nodes"]) {
        if (n["id"] == "VIN-B1")  s_vin = true;
        if (n["id"] == "BATCH-B1") {
            s_b1 = true;
            CHECK_EQ(n["VIN绑定"], "VIN-B1");
            CHECK_EQ(n["供应商"], "SUP-B1");
        }
        if (n["id"] == "BATCH-B2") s_b2 = true;
        if (n["id"] == "SUP-B1")  s_sup = true;
    }
    CHECK(s_vin && s_b1 && s_b2 && s_sup);
    // 边:VIN.批次绑定→B2、B1.VIN绑定→VIN、B2.VIN绑定→VIN、B1.供应商→SUP、B2.供应商→SUP
    CHECK_EQ(v["edges"].size(), 5u);
    bool e_bind = false, e_rev1 = false, e_rev2 = false, e_sup1 = false, e_sup2 = false;
    for (const json& e : v["edges"]) {
        const std::string from = e["from"], key = e["key"], to = e["to"];
        if (from == "VIN-B1" && key == "批次绑定" && to == "BATCH-B2") e_bind = true;
        if (from == "BATCH-B1" && key == "VIN绑定" && to == "VIN-B1") e_rev1 = true;
        if (from == "BATCH-B2" && key == "VIN绑定" && to == "VIN-B1") e_rev2 = true;
        if (from == "BATCH-B1" && key == "供应商" && to == "SUP-B1") e_sup1 = true;
        if (from == "BATCH-B2" && key == "供应商" && to == "SUP-B1") e_sup2 = true;
    }
    CHECK(e_bind && e_rev1 && e_rev2 && e_sup1 && e_sup2);
}

// ============================================================================
// 19. B4 防错拦截视图:扫错料被 R-MAT-PKE 拦截,rejections 非空且理由正确
// ============================================================================
static void test_b4_poke_yoke() {
    banner("19. B4 拦截视图(扫错料)");
    Fixture f;
    CHECK(settled(post(f.sys, "BomReceived", "工位01",
                       {{"BOM清单", {"MAT-1001", "MAT-1002"}}}, "tcm")));

    // 扫错料:L3 被 R-MAT-PKE 拦,零事件零污染
    const mse::Receipt bad = post(f.sys, "MaterialVerified", "工位01",
                                  {{"物料编号", "MAT-9999"}}, "op");
    CHECK(rejected_at(bad, 3));
    bool from_pke = false;
    for (const auto& v : bad.violations)
        if (v.find("R-MAT-PKE") != std::string::npos) from_pke = true;
    CHECK(from_pke);
    CHECK(f.sys.log().events_of_type("MaterialVerified").empty());  // 未入树

    // 扫对通过
    CHECK(settled(post(f.sys, "MaterialVerified", "工位01",
                       {{"物料编号", "MAT-1001"}}, "op")));

    // 拦截视图:rejections 非空,含刚才那条错料候选
    const json v = f.sys.views().render("V-PKE-B4", mse::ViewParams{});
    CHECK(!v.contains("error"));
    CHECK_EQ(v["render_mode"], "拦截");
    CHECK(!v["rejections"].empty());
    bool saw_bad = false;
    for (const json& rj : v["rejections"]) {
        if (rj["candidate"]["type"] == "MaterialVerified" &&
            rj["candidate"]["writes"].contains("工位01") &&
            rj["candidate"]["writes"]["工位01"].value("物料编号", "") == "MAT-9999")
            saw_bad = true;
    }
    CHECK(saw_bad);
    CHECK_EQ(v["rows"].size(), 1u);  // 已结算事实行 = 工位01(扫对那条)
}

// ============================================================================
// 20. F15 逐级报警链:同检点 3 次 → 科长候选结算;10 次 → 部长
// ============================================================================
static void test_f15_alarm_escalation() {
    banner("20. F15 逐级报警(3→科长,10→部长)");
    Fixture f;
    auto fault = [&f]() {
        CHECK(settled(post(f.sys, "FaultAlarmed", "EQ-T1",
                           {{"检点", "工位02-拧紧点P1"}, {"故障码", "E-427"},
                            {"设备状态", "故障"}},
                           "plc")));
    };
    fault();
    fault();
    CHECK(f.sys.log().events_of_type("AlarmEscalated").empty());  // 2 次未达阈值
    fault();                                                      // 第 3 次
    // derive 计数:null→0 起步,第 3 次后 = 3
    CHECK_EQ(f.sys.projection().attrs_of("EQ-T1").at("同检点故障计数"), json(3));
    // 科长预警候选经四层校验结算(trigger 产物仍是候选)
    const auto alarms3 = f.sys.log().events_of_type("AlarmEscalated");
    CHECK_EQ(alarms3.size(), 1u);
    if (!alarms3.empty()) {
        CHECK_EQ(alarms3[0].writes.at("EQ-T1").at("报警级别"), json("科长"));
        CHECK_EQ(alarms3[0].writes.at("EQ-T1").at("检点"), json("工位02-拧紧点P1"));
        CHECK_EQ(alarms3[0].actor, std::string("rule:R-ALM-003"));
    }
    for (int i = 0; i < 7; ++i) fault();  // 到第 10 次
    CHECK_EQ(f.sys.projection().attrs_of("EQ-T1").at("同检点故障计数"), json(10));
    bool saw_minister = false;
    for (const mse::Event& e : f.sys.log().events_of_type("AlarmEscalated"))
        if (e.writes.at("EQ-T1").at("报警级别") == "部长") saw_minister = true;
    CHECK(saw_minister);

    // F15 流水可见;重放后计数一致(derive 是 fold 的一部分)
    const json v = f.sys.views().render("V-ALARM-F15", mse::ViewParams{});
    CHECK(!v["rows"].empty());
    mse::Projection replayed = f.sys.pipeline().replay_projection(f.sys.log());
    CHECK_EQ(replayed.attrs_of("EQ-T1").at("同检点故障计数"), json(10));
    CHECK_EQ(replayed.hash(), f.sys.projection().hash());
}

// ============================================================================
// 21. B7-B10 规则决定行:各拉动视图只出本类型的行
// ============================================================================
static void test_pull_views_row_rules() {
    banner("21. B7-B10 读侧行过滤(规则决定行)");
    Fixture f;
    for (const auto& [po, kind] : std::vector<std::pair<std::string, std::string>>{
             {"PO-KAN", "Kanban"}, {"PO-URG", "紧急"}, {"PO-JIS", "JIS"}, {"PO-JIT", "JIT"}}) {
        CHECK(settled(post(f.sys, "PullOrderCreated", po,
                           {{"拉动类型", kind}, {"拉动状态", "已创建"},
                            {"物料编号", "MAT-1001"}},
                           "logistics")));
    }
    const std::vector<std::pair<std::string, std::string>> views = {
        {"V-KANBAN-B7", "Kanban"}, {"V-PULL-B8", "紧急"},
        {"V-JIS-B9", "JIS"},       {"V-JIT-B10", "JIT"},
    };
    for (const auto& [vid, kind] : views) {
        const json v = f.sys.views().render(vid, mse::ViewParams{});
        CHECK(!v.contains("error"));
        CHECK_EQ(v["rows"].size(), 1u);  // 同一事件集,规则决定只出本类型行
        if (!v["rows"].empty()) CHECK_EQ(v["rows"][0]["拉动类型"], json(kind));
    }
}

// ============================================================================
// 22. 冻结禁调序/换单:R-SEQ-FROZEN 拦截已冻结订单
// ============================================================================
static void test_frozen_sequence_lock() {
    banner("22. 冻结禁调序(R-SEQ-FROZEN)");
    Fixture f;
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-FZ1", {{"车型", "SUV-A"}})));
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-FZ2", {{"车型", "Sedan-B"}})));
    // 未冻结:调序/换单可结算
    CHECK(settled(post(f.sys, "SequenceAdjusted", "VIN-FZ1", {{"序列号", 1}}, "planner")));
    CHECK(settled(post(f.sys, "SequenceAdjusted", "VIN-FZ2", {{"序列号", 2}}, "planner")));
    // 冻结 VIN-FZ1
    CHECK(settled(post(f.sys, "OrderFrozen", "VIN-FZ1", {{"冻结状态", "已冻结"}}, "planner")));
    // 冻结后调序:L3 拦截,理由含 R-SEQ-FROZEN
    const mse::Receipt r1 =
        post(f.sys, "SequenceAdjusted", "VIN-FZ1", {{"序列号", 9}}, "planner");
    CHECK(rejected_at(r1, 3));
    bool from_frozen = false;
    for (const auto& v : r1.violations)
        if (v.find("R-SEQ-FROZEN") != std::string::npos) from_frozen = true;
    CHECK(from_frozen);
    // 冻结后换单:同样被拦(multi_target 逐目标求值)
    mse::Candidate sw;
    sw.type  = "OrderSwapped";
    sw.actor = "planner";
    sw.writes["VIN-FZ1"] = {{"序列号", 2}};
    sw.writes["VIN-FZ2"] = {{"序列号", 1}};
    const mse::Receipt r2 = f.sys.pipeline().submit(sw);
    CHECK(rejected_at(r2, 3));
    // 未冻结的 VIN-FZ2 不受影响,仍可调序
    CHECK(settled(post(f.sys, "SequenceAdjusted", "VIN-FZ2", {{"序列号", 5}}, "planner")));
    // 解冻(逆事件,新事实)后恢复可调
    CHECK(settled(post(f.sys, "OrderFreezeReversed", "VIN-FZ1", {{"冻结状态", "未冻结"}},
                       "planner")));
    CHECK(settled(post(f.sys, "SequenceAdjusted", "VIN-FZ1", {{"序列号", 6}}, "planner")));
}

// ============================================================================
// 23. 达成率 derive:R-KPI-002(含计划产量=0 → null 保护,不给无意义值)
// ============================================================================
static void test_efficiency_derive() {
    banner("23. 达成率 derive(除零保护)");
    Fixture f;
    // 产线基线:计划产量 200
    CHECK(settled(post(f.sys, "OrderReceived", "LINE-T1",
                       {{"车型", "混流产线"}, {"计划产量", 200}})));
    CHECK(settled(post(f.sys, "ProductionReported", "LINE-T1",
                       {{"报工数量", 1}, {"实际产量", 50}}, "op")));
    const json a1 = f.sys.projection().attrs_of("LINE-T1");
    CHECK(a1.contains("达成率"));
    CHECK_EQ(a1.at("达成率"), json(0.25));  // 50/200

    // 冲正驱动回摆:on_types 含 ReportReversed
    const mse::Receipt rw = post(f.sys, "ProductionReported", "LINE-T1",
                                 {{"报工数量", 1}, {"实际产量", 100}}, "op");
    CHECK(settled(rw));
    mse::Candidate rev;
    rev.type  = "ReportReversed";
    rev.actor = "statistician";
    rev.writes["LINE-T1"] = {{"实际产量", 50}};
    rev.corrects = *rw.event_id;
    CHECK(settled(f.sys.pipeline().submit(rev)));
    CHECK_EQ(f.sys.projection().attrs_of("LINE-T1").at("达成率"), json(0.25));

    // 计划产量=0:return null 保护(除零不给无意义值,也不抛 RuleError)
    CHECK(settled(post(f.sys, "OrderReceived", "LINE-T2",
                       {{"车型", "混流产线"}, {"计划产量", 0}})));
    CHECK(settled(post(f.sys, "ProductionReported", "LINE-T2",
                       {{"报工数量", 1}, {"实际产量", 3}}, "op")));
    const json a2 = f.sys.projection().attrs_of("LINE-T2");
    CHECK(a2.contains("达成率"));
    CHECK(a2.at("达成率").is_null());

    // 重放一致(derive 是 fold 的一部分)
    mse::Projection replayed = f.sys.pipeline().replay_projection(f.sys.log());
    CHECK_EQ(replayed.hash(), f.sys.projection().hash());
}

// ============================================================================
// 24. WASM 规则接入类型后:05 调序拦截理由含 R-PLAN-ADJUST-WASM(双拦截)
// ============================================================================
static void test_wasm_rule_wired_double_gate() {
    banner("24. WASM 接入后 05 双拦截");
    Fixture f;
    mse::load_wasm_seeds(f.sys.defs(), MSE_ASSETS_DIR);
    CHECK(f.sys.defs().find_rule("R-PLAN-ADJUST-WASM") != nullptr);
    // 接入类型(定义热更新):jsonlogic 与 WASM 同一立法、双重把关
    const mse::Receipt wire = f.sys.defs().settle_definition(
        "EventTypeRegistered",
        {{"type", "SequenceAdjusted"},
         {"required_keys", {"id", "actor"}},
         {"optional_keys", {"优先级", "序列号"}},
         {"rules", {"R-PLAN-ADJUST", "R-SEQ-FROZEN", "R-PLAN-ADJUST-WASM"}},
         {"correction", "SequenceAdjustReversed"},
         {"multi_target", false},
         {"settlement", "sync"}});
    CHECK(settled(wire));

    CHECK(settled(post(f.sys, "OrderReceived", "VIN-WA", {{"车型", "SUV-A"}})));
    drive_plan_to(f.sys, "VIN-WA", 5);
    const mse::Receipt rj =
        post(f.sys, "SequenceAdjusted", "VIN-WA", {{"优先级", 1}}, "planner");
    CHECK(rejected_at(rj, 3));
    bool from_jsonlogic = false, from_wasm = false;
    for (const auto& v : rj.violations) {
        if (v.find("R-PLAN-ADJUST-WASM") != std::string::npos) from_wasm = true;
        else if (v.find("R-PLAN-ADJUST") != std::string::npos) from_jsonlogic = true;
    }
    CHECK(from_jsonlogic);  // jsonlogic 规则同场把关
    CHECK(from_wasm);       // 拦截理由含 WASM 规则
}

// ============================================================================
// 25. PLC 三层过滤单元:迟滞 / 驻留 / 聚合去重 / 网关映射
// ============================================================================
#include "mse/plc_filter.h"

static void test_plc_filter_units() {
    banner("25. PLC 三层过滤单元");
    using mse::PlcFilter;

    {   // 迟滞:中间带抖动不动(committed 低态,带内值永不产生输出)
        PlcFilter f("sig-h", PlcFilter::Config{1.0, 0.0, 3, 0});
        for (int t = 0; t < 50; ++t)
            CHECK(!f.sample(0.2 + 0.05 * (t % 7), t).has_value());  // 0.2..0.5 带内
        CHECK(!f.state());
    }
    {   // 驻留:新态须连续稳定 dwell_ticks 拍;不满不采信,回落清零重计
        PlcFilter f("sig-d", PlcFilter::Config{1.0, 0.0, 3, 0});
        CHECK(!f.sample(1.2, 0).has_value());   // 驻留 1/3
        CHECK(!f.sample(1.1, 1).has_value());   // 驻留 2/3
        CHECK(!f.sample(-0.2, 2).has_value());  // 值回落:清零重计
        CHECK(!f.sample(1.2, 3).has_value());   // 重新驻留 1/3
        CHECK(!f.sample(1.2, 4).has_value());   // 2/3
        const auto e = f.sample(1.2, 5);        // 3/3:采信翻转
        CHECK(e.has_value() && *e == true);
        CHECK(f.state());
        // 回到低态同样要驻留 3 拍
        CHECK(!f.sample(-0.1, 6).has_value());
        CHECK(!f.sample(-0.1, 7).has_value());
        const auto e2 = f.sample(-0.1, 8);
        CHECK(e2.has_value() && *e2 == false);
        CHECK(!f.state());
    }
    {   // 聚合去重:窗口内的迁移沿被吞,窗口外放行
        PlcFilter f("sig-w", PlcFilter::Config{1.0, 0.0, 1, 10});
        const auto e1 = f.sample(1.2, 0);    // 第一个沿:输出
        CHECK(e1.has_value() && *e1 == true);
        CHECK(!f.sample(-0.1, 2).has_value());  // 距上次输出 2 < 10:吞掉(状态仍翻转)
        CHECK(!f.state());
        const auto e3 = f.sample(1.2, 15);   // 距上次输出 15 >= 10:放行
        CHECK(e3.has_value() && *e3 == true);
    }
    {   // 网关映射:未映射信号 nullopt;已映射 → 候选(边决定写入值,tick 文本)
        mse::AdapterGateway gw;
        CHECK(!gw.translate("unmapped", true, 1).has_value());
        mse::AdapterGateway::EdgeMapping m;
        m.type = "PlcEdgeReported";
        m.target_id = "工位01";
        m.key = "工位占用";
        m.high_value = "占用";
        m.low_value = "空闲";
        m.anchor = "整车厂/总装车间/总装线/工位01";
        gw.map_edge("sig-occupy", m);
        const auto hi = gw.translate("sig-occupy", true, 42);
        CHECK(hi.has_value());
        CHECK_EQ(hi->type, "PlcEdgeReported");
        CHECK_EQ(hi->writes.at("工位01").at("工位占用"), json("占用"));
        CHECK_EQ(hi->actor, "plc-gateway");
        CHECK_EQ(hi->occur_time, "tick:42");
        CHECK_EQ(hi->space.anchor, "整车厂/总装车间/总装线/工位01");
        const auto lo = gw.translate("sig-occupy", false, 43);
        CHECK(lo.has_value());
        CHECK_EQ(lo->writes.at("工位01").at("工位占用"), json("空闲"));
    }
}

// ============================================================================
// 26. PLC 噪声洪峰:100000 个含噪采样经三层过滤,结算事件数有界
// ============================================================================
// 确定性伪随机(LCG;核心链路不读物理时钟/随机数,测试同样守此纪律)
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

static void test_plc_noise_flood() {
    banner("26. PLC 噪声洪峰(100000 采样 → 2 事件)");
    Fixture f;

    mse::PlcFilter filter("工位01-占用光电", mse::PlcFilter::Config{1.0, 0.0, 3, 0});
    mse::AdapterGateway gw;
    mse::AdapterGateway::EdgeMapping m;
    m.type = "PlcEdgeReported";
    m.target_id = "工位01";
    m.key = "工位占用";
    m.high_value = "占用";
    m.low_value = "空闲";
    m.anchor = "整车厂/总装车间/总装线/工位01";
    gw.map_edge("工位01-占用光电", m);

    constexpr int64_t kSamples = 100000;
    Lcg lcg(20260907);
    int edges = 0;
    const int64_t log_before = f.sys.log().size();
    for (int64_t t = 0; t < kSamples; ++t) {
        // 真实信号:0..29999 低态,30000..69999 高态,70000.. 低态(真翻转 2 次)
        const bool high = t >= 30000 && t < 70000;
        // 噪声:在 off-0.1 与 on+0.1 附近抖动(-0.1..0.1 / 0.9..1.1)
        double v = (high ? 1.0 : 0.0) + lcg.uniform(-0.1, 0.1);
        // 噪声突刺:低态期偶发 2 连拍越上阈(不足驻留 3 拍,不得翻转)
        if (!high && t % 2000 < 2) v = 1.05;
        const auto edge = filter.sample(v, t);
        if (!edge.has_value()) continue;  // 噪声/驻留中/去重:不到端点
        ++edges;
        auto cand = gw.translate("工位01-占用光电", *edge, t);
        CHECK(cand.has_value());
        cand->trust = 3;  // 进程内适配器入口:显式信任级(PLC 网关凭证等价)
        const mse::Receipt r = f.sys.pipeline().submit(*cand);
        CHECK(r.status == mse::Receipt::Status::kAccepted);  // async:已验未结
    }
    // 过滤器输出沿数 << 采样数:恰好等于真实翻转次数
    CHECK_EQ(edges, 2);
    CHECK(edges < kSamples / 1000);
    // 悬态口径:drain 之前事件日志看不到它们
    CHECK_EQ(f.sys.pipeline().async_pending(), 2u);
    CHECK_EQ(f.sys.log().size(), log_before);
    // drain:单写者串行落账,事件数 == 真实翻转次数,日志零污染
    CHECK_EQ(f.sys.pipeline().drain_async(), 2u);
    CHECK_EQ(f.sys.log().size(), log_before + 2);
    CHECK_EQ(f.sys.log().events_of_type("PlcEdgeReported").size(), 2u);
    // 终态正确:最后一次沿是回到低态(空闲)
    CHECK_EQ(f.sys.projection().attrs_of("工位01").at("工位占用"), json("空闲"));
}

// ============================================================================
// 27. 异步结算边界:accepted + queue_seq、悬态、drain 全序、被拒不入队
// ============================================================================
static void test_async_settlement() {
    banner("27. 异步结算边界");
    Fixture f;

    // 注册测试属性(writers 授权给新类型;优先级等既有键未授权,会被 L2 拦)
    const mse::Receipt ra = f.sys.defs().settle_definition(
        "AttributeRegistered",
        {{"key", "异步优先级"}, {"semantic", "异步结算测试用优先级"},
         {"datatype", "integer"}, {"unit", ""},
         {"range", json::array()},{"writers", {"AsyncSeqAdjusted"}},
         {"kind", "原生"},       {"rule_ref", ""}});
    CHECK(settled(ra));

    // 注册一个带 L3 规则的 async 类型(借用 R-PLAN-ADJUST:仅 01/02 可结算)
    const mse::Receipt rt = f.sys.defs().settle_definition(
        "EventTypeRegistered",
        {{"type", "AsyncSeqAdjusted"},
         {"required_keys", {"id", "actor"}},
         {"optional_keys", {"异步优先级"}},
         {"rules", {"R-PLAN-ADJUST"}},
         {"correction", ""},
         {"multi_target", false},
         {"settlement", "async"}});
    CHECK(settled(rt));

    auto async_candidate = [](const std::string& id, int prio) {
        mse::Candidate c;
        c.type = "AsyncSeqAdjusted";
        c.actor = "planner";
        c.writes[id] = {{"异步优先级", prio}};
        return c;
    };

    // L3 拒:计划状态 05 的车不可调序——被拒的异步候选立即 rejected,不入队
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-AS5", {{"车型", "SUV-A"}})));
    drive_plan_to(f.sys, "VIN-AS5", 5);
    const mse::Receipt rej = f.sys.pipeline().submit(async_candidate("VIN-AS5", 9));
    CHECK(rejected_at(rej, 3));
    CHECK_EQ(f.sys.pipeline().async_pending(), 0u);

    // 合法候选:accepted + queue_seq 单调;日志未增长(悬态:已验未结)
    CHECK(settled(post(f.sys, "OrderReceived", "VIN-AS1", {{"车型", "SUV-A"}})));
    drive_plan_to(f.sys, "VIN-AS1", 1);
    const int64_t log_before = f.sys.log().size();
    const mse::Receipt a1 = f.sys.pipeline().submit(async_candidate("VIN-AS1", 1));
    CHECK(a1.status == mse::Receipt::Status::kAccepted);
    CHECK(a1.queue_seq.has_value());
    const mse::Receipt a2 = f.sys.pipeline().submit(async_candidate("VIN-AS1", 2));
    CHECK(a2.status == mse::Receipt::Status::kAccepted);
    CHECK_EQ(*a2.queue_seq, *a1.queue_seq + 1);  // 队列序号单调
    CHECK_EQ(f.sys.log().size(), log_before);    // 未落日志
    CHECK_EQ(f.sys.pipeline().async_pending(), 2u);

    // 异步回执也幂等:重复提交返回原回执,队列不再增长
    mse::Candidate idem = async_candidate("VIN-AS1", 3);
    idem.idempotency_key = "async-idem-1";
    const mse::Receipt i1 = f.sys.pipeline().submit(idem);
    const mse::Receipt i2 = f.sys.pipeline().submit(idem);
    CHECK(i1.status == mse::Receipt::Status::kAccepted);
    CHECK(i2.status == mse::Receipt::Status::kAccepted);
    CHECK_EQ(*i2.queue_seq, *i1.queue_seq);
    CHECK_EQ(f.sys.pipeline().async_pending(), 3u);

    // FIFO + 全序:先 drain 1 条,再插一条同步事件,再 drain 余下——
    // 异步事件的事件 id 严格按 drain 次序分配(单写者全序保持)
    CHECK_EQ(f.sys.pipeline().drain_async(1), 1u);
    CHECK_EQ(f.sys.pipeline().async_pending(), 2u);
    const auto drained1 = f.sys.log().events_of_type("AsyncSeqAdjusted");
    CHECK_EQ(drained1.size(), 1u);
    CHECK_EQ(drained1[0].writes.at("VIN-AS1").at("异步优先级"), json(1));  // FIFO:先 1
    const mse::Receipt sync_r =
        post(f.sys, "OrderReceived", "VIN-AS2", {{"车型", "Sedan-B"}});
    CHECK(settled(sync_r));
    CHECK_EQ(f.sys.pipeline().drain_async(), 2u);
    const auto drained = f.sys.log().events_of_type("AsyncSeqAdjusted");
    CHECK_EQ(drained.size(), 3u);
    CHECK(drained[1].event_id > *sync_r.event_id);  // drain 晚于同步事件:序号在后
    CHECK(drained[2].event_id > drained[1].event_id);
    // 视图可见(投影终态 = 最后一次写入)
    CHECK_EQ(f.sys.projection().attrs_of("VIN-AS1").at("异步优先级"), json(3));
}

// ============================================================================
// 28. 信任分级:min_trust L2 校验、HTTP X-MSE-Token、accepted 序列化往返
// ============================================================================
static void test_trust_tiers() {
    banner("28. 信任分级");
    Fixture f;

    auto plc_candidate = [](const std::string& id) {
        mse::Candidate c;
        c.type = "PlcEdgeReported";
        c.actor = "plc-gateway";
        c.writes[id] = {{"工位占用", "占用"}};
        return c;
    };

    // 无凭证(trust=0)提交 min_trust=1 类型:L2 拒,理由含"信任级不足"
    mse::Candidate c0 = plc_candidate("工位01");
    const mse::Receipt r0 = f.sys.pipeline().submit(c0);
    CHECK(rejected_at(r0, 2));
    bool trust_msg = false;
    for (const auto& v : r0.violations)
        if (v.find("信任级不足") != std::string::npos) trust_msg = true;
    CHECK(trust_msg);

    // 进程内 SDK 调用显式 trust=3(最高):通过,async → accepted
    mse::Candidate c3 = plc_candidate("工位01");
    c3.trust = 3;
    CHECK(f.sys.pipeline().submit(c3).status == mse::Receipt::Status::kAccepted);
    f.sys.pipeline().drain_async();

    // min_trust 定义校验:负数被拒
    const mse::Receipt bad_def = f.sys.defs().settle_definition(
        "EventTypeRegistered",
        {{"type", "BadTrust"}, {"required_keys", {"id", "actor"}},
         {"optional_keys", json::array()}, {"rules", json::array()},
         {"correction", ""}, {"multi_target", false}, {"min_trust", -1}});
    CHECK(!settled(bad_def));

    // API 门面:post_events(payload, trust) 注入信任级;accepted 回执形态
    const json aj = f.sys.api().post_events(
        {{"type", "PlcEdgeReported"}, {"id", "工位02"}, {"actor", "plc-gateway"},
         {"工位占用", "占用"}},
        1);
    CHECK_EQ(aj["status"], "accepted");
    CHECK(aj["queue_seq"].is_number());
    const json aj0 = f.sys.api().post_events(
        {{"type", "PlcEdgeReported"}, {"id", "工位02"}, {"actor", "plc-gateway"},
         {"工位占用", "空闲"}},
        0);
    CHECK_EQ(aj0["status"], "rejected");
    CHECK_EQ(aj0["layer"], 2);
    f.sys.pipeline().drain_async();

    // Receipt accepted 序列化往返
    {
        const json j = json(mse::Receipt::accepted(7));
        CHECK_EQ(j["status"], "accepted");
        CHECK_EQ(j["queue_seq"], 7);
        const mse::Receipt back = j.get<mse::Receipt>();
        CHECK(back.status == mse::Receipt::Status::kAccepted);
        CHECK(back.queue_seq.has_value() && *back.queue_seq == 7);
        // 既有形态不回归
        CHECK(json(mse::Receipt::settled(5)).get<mse::Receipt>().status ==
              mse::Receipt::Status::kSettled);
        CHECK(json(mse::Receipt::rejected(3, {"x"})).get<mse::Receipt>().status ==
              mse::Receipt::Status::kRejected);
    }

    // 真实 HTTP:X-MSE-Token 头 → trust_of → post_events(body, trust)
    f.sys.api().set_trust_tokens({{"plc-secret", 1}, {"admin-secret", 3}});
    mse::HttpServer srv;
    const bool ok = srv.listen_on("127.0.0.1", 0, [&f](const mse::HttpRequest& req) {
        if (req.method == "POST" && req.path == "/events") {
            const json payload = json::parse(req.body, nullptr, false);
            if (payload.is_discarded())
                return mse::HttpResponse{400, "application/json; charset=utf-8",
                                         "{\"error\":\"bad json\"}"};
            // 凭证头 → 信任级(未携带/未登记 → 0)
            int trust = 0;
            if (auto it = req.headers.find("x-mse-token"); it != req.headers.end())
                trust = f.sys.api().trust_of(it->second);
            return mse::HttpResponse{200, "application/json; charset=utf-8",
                                     f.sys.api().post_events(payload, trust).dump()};
        }
        return mse::HttpResponse{404, "application/json; charset=utf-8",
                                 "{\"error\":\"not found\"}"};
    });
    CHECK(ok);
    const uint16_t port = srv.port();
    const json plc_payload = {{"type", "PlcEdgeReported"}, {"id", "工位03"},
                              {"actor", "plc-gateway"}, {"工位占用", "占用"}};
    // 无凭证:L2 拒
    {
        const auto [s, b] =
            mse::http_request("127.0.0.1", port, "POST", "/events", plc_payload.dump());
        CHECK_EQ(s, 200);
        const json r = json::parse(b);
        CHECK_EQ(r["status"], "rejected");
        CHECK_EQ(r["layer"], 2);
    }
    // 错凭证:同样被拒(未登记 → trust=0)
    {
        const auto [s, b] = mse::http_request("127.0.0.1", port, "POST", "/events",
                                              plc_payload.dump(),
                                              {{"X-MSE-Token", "wrong-token"}});
        CHECK_EQ(s, 200);
        CHECK_EQ(json::parse(b)["status"], "rejected");
    }
    // 带对凭证(PLC 网关级):accepted
    {
        const auto [s, b] = mse::http_request("127.0.0.1", port, "POST", "/events",
                                              plc_payload.dump(),
                                              {{"X-MSE-Token", "plc-secret"}});
        CHECK_EQ(s, 200);
        const json r = json::parse(b);
        CHECK_EQ(r["status"], "accepted");
        CHECK(r["queue_seq"].is_number());
    }
    f.sys.pipeline().drain_async();
    CHECK_EQ(f.sys.projection().attrs_of("工位03").at("工位占用"), json("占用"));
    srv.stop();
}

// ============================================================================
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // 崩溃时也能看到已完成的段落
    std::puts("mse 红线测试(memory 后端)");
    test_replay_bit_identical();
    test_four_layers();
    test_correction_chain();
    test_read_write_consistency();
    test_definition_hot_update();
    test_spacetime();
    test_rule_dsl();
    test_http_end_to_end();
    test_knowledge_assist();
    test_entity_bridge();
    test_wasm_sandbox();
    test_snapshots();
    test_as_of_snapshot_consistency();
    test_idempotency_persistence();
    test_on_types_derive();
    test_traversal_view();
    test_b5_traceability();
    test_b4_poke_yoke();
    test_f15_alarm_escalation();
    test_pull_views_row_rules();
    test_frozen_sequence_lock();
    test_efficiency_derive();
    test_wasm_rule_wired_double_gate();
    test_plc_filter_units();
    test_plc_noise_flood();
    test_async_settlement();
    test_trust_tiers();
    std::printf("----\nchecks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
