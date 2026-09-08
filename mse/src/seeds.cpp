// ============================================================================
// mse/seeds.cpp —— 种子定义:系统键 + 整车厂业务全覆盖(46 视图的机制实例化)
//
// 全部经 DefinitionLayer::settle_definition 注册(定义也走候选-结算,自同构)。
// 注册顺序:锚点(自顶向下,父锚点先注册)→ 属性 → 规则 → 事件类型(修正类型
// 先于引用它的类型)→ 视图。回执逐条检查:种子是可信定义,rejected 即 bug——
// 注册完毕打印 (已成功数/总数),任何 rejected 逐条打印 violations。
//
// 业务覆盖(docs/mes/西门子MES整车厂案例_视图提取.md §4.2 命名为准):
//   A 生产计划:A1 订单接收/A2 排序(含冻结)/A4 工位指导/A5 状态监控/A6 效率
//     (达成率派生)/A7 报工流水(A3 队列既有);
//   B 物料:B1 主数据/B2 BOM/B3 库存/B4 防错(拦截)/B5 追溯(遍历)/B6 缺料呼叫
//     /B7-B10 四类拉动(规则决定行)/B11 线边库;
//   C AVI:C2 跟踪查询/C3 区域跟踪(C1 既有);
//   D ANDON:D1 安灯大屏/D3 呼叫停线记录(D2 既有,FTT 派生);
//   E PMC:E1 设备监控/E2 计数/E3 产量统计/E4 故障分析(规则决定行)/E5 报警查询
//     /F15 逐级报警(同检点计数 derive + 3 次科长/10 次部长 trigger);
//   F 质量:F2 检验计划/F4 任务分配/F7 车辆参数/F8 随车卡/F9 位置历史/F10 缺陷
//     采集(拦截)/F12 质量记录/F14 追溯(遍历)/F16 综合报表(F11/F13 既有);
//   G 设备:G1 台账/G2 状态详情;
//   H 集成:H3 接口监控(ERP 报文进系统的事实锚)。
//   F1/F3/F5/F6/F15 规则侧/H1/H2 为配置视图,由定义层四集合本身承载(见 README);
//   E6/E7/E8 由 E1/E2 的 variants 与机制覆盖;I1-I3 平台建模层不做。
// ============================================================================

#include "mse/seeds.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "mse/dictionary.h"

namespace mse {

namespace {

// ---- 种子回执纪律:种子是可信定义,rejected 即 bug,逐条打印 ----
struct SeedTally {
    int ok = 0;
    int total = 0;
};
SeedTally g_tally;

void tally(const Receipt& r, const std::string& what) {
    ++g_tally.total;
    if (r.status == Receipt::Status::kSettled) {
        ++g_tally.ok;
        return;
    }
    std::printf("[seeds] 定义候选被拒(bug): %s\n", what.c_str());
    for (const std::string& v : r.violations) std::printf("  - %s\n", v.c_str());
}

// 属性字典条目的便捷组装
json attr_payload(const std::string& key, const std::string& semantic,
                  const std::string& datatype, const std::string& unit,
                  std::initializer_list<json> range,
                  std::initializer_list<const char*> writers,
                  const std::string& kind = "原生",
                  const std::string& rule_ref = "") {
    return {{"key", key},         {"semantic", semantic},
            {"datatype", datatype},{"unit", unit},
            {"range", json(range)},{"writers", json(writers)},
            {"kind", kind},       {"rule_ref", rule_ref}};
}

// 事件类型条目的便捷组装(修正类型配平由调用方保证先注册;
// presets 非空才写入负载——固定写入值,按钮语义自带的表单预填)
json type_payload(const std::string& type,
                  std::initializer_list<const char*> required,
                  std::initializer_list<const char*> optional,
                  std::initializer_list<const char*> rules,
                  const std::string& correction, bool multi_target,
                  const json& presets = json::object()) {
    json p = {{"type", type},
              {"required_keys", json(required)},
              {"optional_keys", json(optional)},
              {"rules", json(rules)},
              {"correction", correction},
              {"multi_target", multi_target},
              {"settlement", "sync"}};
    if (!presets.empty()) p["presets"] = presets;
    return p;
}

// 规则条目的便捷组装(logic 为 JSON-logic 方言文本;on_types 仅 derive/trigger 有效)
json rule_payload(const std::string& rule_id,
                  std::initializer_list<const char*> deps, const std::string& effect,
                  const std::string& target_key, const char* logic_text,
                  const std::string& consumers = "both",
                  std::initializer_list<const char*> on_types = {}) {
    return {{"rule_id", rule_id}, {"deps", json(deps)},
            {"effect", effect},   {"target_key", target_key},
            {"logic", json::parse(logic_text)},
            {"consumers", consumers},
            {"on_types", json(on_types)}};
}

void reg_anchor(DefinitionLayer& defs, const std::string& path, const std::string& name) {
    tally(defs.settle_definition("AnchorRegistered",
                                 {{"path", path}, {"name", name}, {"meta", json::object()}}),
          "AnchorRegistered " + path);
}

void reg_attr(DefinitionLayer& defs, const json& payload) {
    tally(defs.settle_definition("AttributeRegistered", payload),
          "AttributeRegistered " + payload.value("key", "?"));
}

void reg_rule(DefinitionLayer& defs, const json& payload) {
    tally(defs.settle_definition("RuleRegistered", payload),
          "RuleRegistered " + payload.value("rule_id", "?"));
}

void reg_type(DefinitionLayer& defs, const json& payload) {
    tally(defs.settle_definition("EventTypeRegistered", payload),
          "EventTypeRegistered " + payload.value("type", "?"));
}

void reg_view(DefinitionLayer& defs, const json& payload) {
    tally(defs.settle_definition("ViewRegistered", payload),
          "ViewRegistered " + payload.value("view_id", "?"));
}

} // namespace

// ----------------------------------------------------------------------------
// 系统键:一切皆属性,type/id/actor 等也须在字典登记(writers="*")。
// ----------------------------------------------------------------------------
void load_system_keys(DefinitionLayer& defs) {
    g_tally = SeedTally{};  // 统计口径:一次完整装填(系统键 + 业务种子)
    reg_attr(defs, attr_payload("type", "事件类型(注册表引用)", "string", "", {}, {"*"}));
    reg_attr(defs, attr_payload("id", "本体句柄:现实事物的数字 id", "string", "", {}, {"*"}));
    reg_attr(defs, attr_payload("actor", "发起者本体 id", "string", "", {}, {"*"}));
    reg_attr(defs, attr_payload("space", "时空戳:空间参照(锚点/坐标/模糊原文)", "string", "", {}, {"*"}));
    reg_attr(defs, attr_payload("time", "时空戳:发生时间(ISO,可模糊)", "string", "", {}, {"*"}));
    reg_attr(defs, attr_payload("evidence", "依据(扫描原始值/图片哈希/外部单号)", "string", "", {}, {"*"}));
    reg_attr(defs, attr_payload("corrects", "因果引用:被修正的原事件 id", "string", "", {}, {"*"}));
    reg_attr(defs, attr_payload("idempotency_key", "候选层幂等键(来源方生成)", "string", "", {}, {"*"}));
    reg_attr(defs, attr_payload("aggregates", "聚合集声明:[{key, from}],多属索引", "list", "", {}, {"*"}));
    reg_attr(defs, attr_payload("writes", "变化集:{本体id: {属性键: 新值}}", "object", "", {}, {"*"}));
}

// ----------------------------------------------------------------------------
// 整车厂业务种子:锚点 → 属性 → 规则 → 事件类型 → 视图(46 视图机制全覆盖)。
// ----------------------------------------------------------------------------
void load_auto_plant_seeds(DefinitionLayer& defs) {
    // ---- ① 锚点(层级自顶向下:父锚点须先注册,引用完整性校验) ----
    reg_anchor(defs, "整车厂", "整车厂");
    reg_anchor(defs, "整车厂/总装车间", "总装车间");
    reg_anchor(defs, "整车厂/总装车间/总装线", "总装线");
    reg_anchor(defs, "整车厂/总装车间/总装线/工位01", "工位01");
    reg_anchor(defs, "整车厂/总装车间/总装线/工位02", "工位02");
    reg_anchor(defs, "整车厂/总装车间/总装线/工位03", "工位03");
    reg_anchor(defs, "整车厂/总装车间/返修区", "返修区");
    reg_anchor(defs, "整车厂/总装车间/终检工位", "终检工位");
    reg_anchor(defs, "整车厂/总装车间/线边库", "线边库");

    // ---- ② 属性(方面名 + 值域 + 语义;写授权逐键填准) ----
    // -- A 生产计划(含既有) --
    reg_attr(defs, attr_payload("计划状态",
        "订单计划五状态机:01 新建/02 已审核/03 已下发/04 生产中/05 完工关闭",
        "enum", "", {"01", "02", "03", "04", "05"}, {"PlanReleased", "PlanRolledBack"}));
    reg_attr(defs, attr_payload("车型", "车辆型号(如 SUV-A / Sedan-B)",
        "string", "", {}, {"OrderReceived", "OrderRevoked", "OrderReReceived", "OrderInserted"}));
    reg_attr(defs, attr_payload("优先级", "排产优先级(数值越小越靠前)",
        "integer", "", {}, {"SequenceAdjusted", "SequenceAdjustReversed", "OrderInserted"}));
    reg_attr(defs, attr_payload("订单状态", "生产订单接收状态:接收/取消",
        "enum", "", {"接收", "取消"},
        {"OrderReceived", "OrderRevoked", "OrderReReceived", "OrderCancelled",
         "OrderCancelReversed", "OrderRevokeReversed"}));
    reg_attr(defs, attr_payload("冻结状态", "排序冻结:已冻结订单禁止调序/换单",
        "enum", "", {"未冻结", "已冻结"}, {"OrderFrozen", "OrderFreezeReversed"}));
    reg_attr(defs, attr_payload("序列号", "生产序列中的顺序号(A2 排序结果)",
        "integer", "", {}, {"SequenceAdjusted", "SequenceAdjustReversed", "OrderInserted",
                            "OrderInsertReversed", "OrderSwapped", "OrderSwapReversed"}));
    reg_attr(defs, attr_payload("交付期", "订单交付期(ERP 下达)",
        "string", "", {}, {"OrderReceived", "OrderReReceived", "OrderInserted"}));
    reg_attr(defs, attr_payload("计划产量", "计划下达的目标产量",
        "integer", "件", {}, {"PlanReleased", "PlanRolledBack", "OrderReceived", "OrderReReceived"}));
    reg_attr(defs, attr_payload("实际产量", "报工累计的实际产出",
        "integer", "件", {}, {"ProductionReported", "ReportReversed"}));
    reg_attr(defs, attr_payload("达成率", "效率指标 = 实际产量/计划产量(规则推导;计划为 0 → null)",
        "number", "", {}, {}, "派生", "R-KPI-002"));

    // -- B 物料 --
    reg_attr(defs, attr_payload("物料编号", "物料主数据编号(扫码比对的键)",
        "string", "", {}, {"MaterialRegistered", "MaterialRetired", "MaterialVerified",
                           "StockUpdated", "PullOrderCreated",
                           "MaterialCallRaised", "MaterialCallCancelled"}));
    reg_attr(defs, attr_payload("批次号", "物料批次号(追溯锚)",
        "string", "", {}, {"MaterialRegistered", "StockUpdated"}));
    reg_attr(defs, attr_payload("库存数量", "实时库存数量",
        "integer", "件", {}, {"MaterialRegistered", "StockUpdated", "PullOrderCreated"}));
    reg_attr(defs, attr_payload("库位", "库存存放库位(WMS 同步)",
        "string", "", {}, {"MaterialRegistered", "StockUpdated"}));
    reg_attr(defs, attr_payload("库存阈值", "JIT 拉动触发阈值(低于即拉动)",
        "integer", "件", {}, {"MaterialRegistered"}));
    reg_attr(defs, attr_payload("BOM清单", "工位/订单的物料清单(防错比对依据)",
        "list", "", {}, {"BomReceived"}));
    reg_attr(defs, attr_payload("物料需求", "本工位物料需求清单(A4 工位指导)",
        "list", "", {}, {"BomReceived"}));
    reg_attr(defs, attr_payload("工艺信息", "本工位工艺信息(A4 工位指导)",
        "string", "", {}, {"BomReceived"}));
    reg_attr(defs, attr_payload("拉动类型", "拉动单类型:Andon/Kanban/紧急/JIS/JIT",
        "enum", "", {"Andon", "Kanban", "紧急", "JIS", "JIT"},
        {"PullOrderCreated", "PullOrderCancelled"}));
    reg_attr(defs, attr_payload("拉动状态", "拉动单状态机:已创建/已发货/已收货/已取消",
        "enum", "", {"已创建", "已发货", "已收货", "已取消"},
        {"PullOrderCreated", "PullOrderShipped", "PullOrderReceived", "PullOrderCancelled"}));
    reg_attr(defs, attr_payload("呼叫状态", "呼叫状态机:呼叫中/已响应/已取消(缺料呼叫与 ANDON 通用呼叫共用)",
        "enum", "", {"呼叫中", "已响应", "已取消"},
        {"MaterialCallRaised", "MaterialCallAnswered", "MaterialCallCancelled",
         "CallRaised", "CallAcknowledged", "CallCancelled"}));
    reg_attr(defs, attr_payload("缺料工位", "缺料呼叫发起的工位(本体 id,关系即属性)",
        "ref", "", {}, {"MaterialCallRaised", "MaterialCallCancelled"}));
    reg_attr(defs, attr_payload("批次绑定", "车辆绑定的物料批次(本体 id,B5/F14 遍历边)",
        "ref", "", {}, {"BatchBoundToVIN", "BindingVoided"}));
    reg_attr(defs, attr_payload("VIN绑定", "批次绑定的车辆(本体 id,反向遍历边)",
        "ref", "", {}, {"BatchBoundToVIN", "BindingVoided"}));
    reg_attr(defs, attr_payload("供应商", "批次的供应商(本体 id,遍历链末端)",
        "ref", "", {}, {"MaterialRegistered"}));

    // -- C AVI --
    reg_attr(defs, attr_payload("过点区域", "AVI 过点观测的区域标识",
        "string", "", {}, {"VehicleEnteredZone", "VehicleExitedZone", "PassageCorrected"}));

    // -- D ANDON --
    reg_attr(defs, attr_payload("线状态", "产线运行状态:运行/停线/暂停",
        "enum", "", {"运行", "停线", "暂停"}, {"LineStopped", "LineResumed"}));
    reg_attr(defs, attr_payload("停线原因", "停线原因描述(拉环停线记录)",
        "string", "", {}, {"LineStopped"}));
    reg_attr(defs, attr_payload("呼叫类型", "ANDON 通用呼叫类型:缺料/质量/设备",
        "enum", "", {"缺料", "质量", "设备"}, {"CallRaised", "CallCancelled"}));
    reg_attr(defs, attr_payload("缓冲区计数", "线旁缓冲区在制品计数(安灯大屏)",
        "integer", "件", {}, {"CountUpdated"}));
    reg_attr(defs, attr_payload("首次合格数", "首次通过检验的合格品累计(FTT 分子)",
        "integer", "件", {}, {"CountUpdated"}));
    reg_attr(defs, attr_payload("FTT", "首次合格率 = 首次合格数/实际产量(规则推导;产量为 0 → null)",
        "number", "", {}, {}, "派生", "R-KPI-003"));

    // -- E PMC --
    reg_attr(defs, attr_payload("设备状态", "设备运行状态:运行/故障/停机/保养",
        "enum", "", {"运行", "故障", "停机", "保养"},
        {"FaultAlarmed", "FaultCleared", "EquipmentStatusReported", "EquipmentStatusCorrected"}));
    reg_attr(defs, attr_payload("故障码", "设备故障代码(PLC 采集)",
        "string", "", {}, {"FaultAlarmed", "FaultCleared"}));
    reg_attr(defs, attr_payload("故障级别", "故障严重度级别(数值越大越重)",
        "integer", "", {}, {"FaultAlarmed"}));
    reg_attr(defs, attr_payload("检点", "故障发生的检点标识(同检点计数粒度)",
        "string", "", {}, {"FaultAlarmed", "FaultCleared", "AlarmEscalated", "AlarmAcknowledged"}));
    reg_attr(defs, attr_payload("产量计数", "PMC 产量计数(过点计数)",
        "integer", "件", {}, {"CountUpdated"}));
    reg_attr(defs, attr_payload("停线计数", "PMC 停线次数累计",
        "integer", "次", {}, {"CountUpdated"}));
    reg_attr(defs, attr_payload("同检点故障计数", "同检点故障累计(逐级报警输入;规则推导,null→0 起步)",
        "integer", "次", {}, {}, "派生", "R-ALM-COUNT"));
    reg_attr(defs, attr_payload("报警级别", "逐级报警责任人级别:科长/部长",
        "enum", "", {"科长", "部长"}, {"AlarmEscalated", "AlarmAcknowledged"}));

    // -- F 质量(既有 + 补全) --
    reg_attr(defs, attr_payload("锁定状态", "质量锁定:已锁车辆不得下线/报工",
        "enum", "", {"未锁", "已锁"}, {"VehicleLocked", "VehicleUnlocked"}));
    reg_attr(defs, attr_payload("车漆", "漆面状态(终检/报缺观测值)",
        "enum", "", {"完好", "划痕", "凹陷", "色差"}, {"DefectRegistered", "DefectCancelled"}));
    reg_attr(defs, attr_payload("缺陷级别", "缺陷严重度:1 轻微/2 一般/3 严重(≥2 须锁定)",
        "integer", "", {1, 2, 3}, {"DefectRegistered", "DefectCancelled"}));
    reg_attr(defs, attr_payload("缺陷总数", "缺陷登记累计(质量报表;规则推导,null→0 起步)",
        "integer", "项", {}, {}, "派生", "R-QUAL-COUNT"));
    reg_attr(defs, attr_payload("返修内容", "返修作业内容描述",
        "string", "", {}, {"ReworkRecorded"}));
    reg_attr(defs, attr_payload("复检结论", "返修后复检结论:OK 放行/NOK 再返修",
        "enum", "", {"OK", "NOK"}, {"RecheckJudged", "JudgementOverruled"}));
    reg_attr(defs, attr_payload("检验计划号", "检验计划编号(F2 检验计划)",
        "string", "", {}, {"InspectionPlanIssued", "InspectionTaskAssigned"}));
    reg_attr(defs, attr_payload("检验工位", "检验任务指派到的工位(本体 id)",
        "ref", "", {}, {"InspectionTaskAssigned"}));
    reg_attr(defs, attr_payload("尾气检测值", "尾气检测数值(检测线设备写入)",
        "number", "", {}, {"InspectionDataRecorded", "InspectionDataCorrected"}));
    reg_attr(defs, attr_payload("大灯检测值", "大灯检测数值(检测线设备写入)",
        "number", "", {}, {"InspectionDataRecorded", "InspectionDataCorrected"}));
    reg_attr(defs, attr_payload("淋雨结论", "淋雨检测结论:合格/不合格",
        "enum", "", {"合格", "不合格"}, {"InspectionDataRecorded", "InspectionDataCorrected"}));

    // -- G 设备 --
    reg_attr(defs, attr_payload("设备编号", "设备台账编号",
        "string", "", {}, {"EquipmentStatusReported", "EquipmentStatusCorrected"}));
    reg_attr(defs, attr_payload("刀具寿命", "刀具剩余寿命(%)",
        "integer", "%", {}, {"EquipmentStatusReported", "EquipmentStatusCorrected"}));

    // -- H 集成 --
    reg_attr(defs, attr_payload("来源系统", "集成报文来源系统(ERP/PLM/WMS)",
        "string", "", {}, {"InboundMessageRecorded", "InboundMessageVoided"}));
    reg_attr(defs, attr_payload("报文类型", "集成报文类型(IDOC/RFC/自定义)",
        "string", "", {}, {"InboundMessageRecorded", "InboundMessageVoided"}));
    reg_attr(defs, attr_payload("外部单号", "外部系统单号(对账锚)",
        "string", "", {}, {"InboundMessageRecorded", "InboundMessageVoided"}));

    // -- 报工/OEE/entity 桥(既有) --
    reg_attr(defs, attr_payload("报工数量", "单次生产报工的合格品数量",
        "integer", "件", {}, {"ProductionReported", "ReportReversed"}));
    reg_attr(defs, attr_payload("可用率", "OEE 输入:设备时间可用率(0~1)",
        "number", "", {}, {"CompositionDeclared"}));
    reg_attr(defs, attr_payload("性能率", "OEE 输入:性能开动率(0~1)",
        "number", "", {}, {"CompositionDeclared"}));
    reg_attr(defs, attr_payload("良品率", "OEE 输入:合格品率(0~1)",
        "number", "", {}, {"CompositionDeclared"}));
    reg_attr(defs, attr_payload("OEE", "设备综合效率 = 可用率×性能率×良品率(规则推导)",
        "number", "", {}, {}, "派生", "R-KPI-001"));
    // entity 演化桥:非人力观测浮现的属性(观测值,EntityObserved 直写)。
    reg_attr(defs, attr_payload("观测标识", "非人力观测浮现的实体标识(如 RFID EPC)",
        "string", "", {}, {"EntityObserved"}));
    reg_attr(defs, attr_payload("观测来源", "浮现观测的来源通道(读头/相机/人工)",
        "string", "", {}, {"EntityObserved"}));
    reg_attr(defs, attr_payload("载具类型", "非人力观测浮现的载具类型",
        "string", "", {}, {"EntityObserved"}));

    // ---- ③ 规则(JSON-logic 方言;deps 引用的键均已登记) ----
    // -- 写侧 filter(consumers=both:写侧 L3 与读侧按钮可用性同一份) --
    // A3 五状态机:仅 01/02 可直接调整顺序,03/04 须退回再调整,05 绝对禁止。
    reg_rule(defs, rule_payload("R-PLAN-ADJUST", {"计划状态"}, "filter", "",
        R"({"if":[{"or":[{"==":[{"var":"计划状态"},null]},
                         {"in":[{"var":"计划状态"},["01","02"]]}]},
                  {"pass":true},
                  {"reject":"R-PLAN-ADJUST:03/04 须退回再调整,05 绝对禁止调整"}]})"));
    // 计划下发合法迁移:01→02→03→04→05,03/04 可退回 01;新车单初始只能进入 01。
    reg_rule(defs, rule_payload("R-PLAN-RELEASE", {"计划状态"}, "filter", "",
        R"({"if":[{"==":[{"var":"计划状态"},null]},
                  {"if":[{"==":[{"write":"计划状态"},"01"]},
                         {"pass":true},
                         {"reject":"R-PLAN-RELEASE:初始只能进入 01"}]},
                  {"or":[{"and":[{"==":[{"var":"计划状态"},"01"]},{"==":[{"write":"计划状态"},"02"]}]},
                         {"and":[{"==":[{"var":"计划状态"},"02"]},{"==":[{"write":"计划状态"},"03"]}]},
                         {"and":[{"==":[{"var":"计划状态"},"03"]},{"==":[{"write":"计划状态"},"04"]}]},
                         {"and":[{"==":[{"var":"计划状态"},"04"]},{"==":[{"write":"计划状态"},"05"]}]},
                         {"and":[{"==":[{"var":"计划状态"},"03"]},{"==":[{"write":"计划状态"},"01"]}]},
                         {"and":[{"==":[{"var":"计划状态"},"04"]},{"==":[{"write":"计划状态"},"01"]}]}]},
                  {"pass":true},
                  {"reject":"R-PLAN-RELEASE:仅允许 01→02→03→04→05 或 03/04 退回 01"}]})"));
    // A2 排序冻结:已冻结订单禁止调序/换单(挂到 SequenceAdjusted/OrderSwapped)。
    reg_rule(defs, rule_payload("R-SEQ-FROZEN", {"冻结状态"}, "filter", "",
        R"({"if":[{"==":[{"var":"冻结状态"},"已冻结"]},
                  {"reject":"R-SEQ-FROZEN:已冻结订单禁止调序/换单"},
                  {"pass":true}]})"));
    // B4 防错防漏:扫描物料编号须在本工位 BOM 清单内(target=工位本体)。
    // B4 防错防漏:扫描物料编号须在本工位 BOM 清单内(target=工位本体);
    // 工位无 BOM(非装配工位/未下达)→ 明确拒绝(写侧 null 注入后必走到)。
    reg_rule(defs, rule_payload("R-MAT-PKE", {"BOM清单"}, "filter", "",
        R"({"if":[{"==":[{"var":"BOM清单"},null]},
                  {"reject":"R-MAT-PKE:该工位无 BOM 清单,禁止校验"},
                  {"if":[{"in":[{"write":"物料编号"},{"var":"BOM清单"}]},
                         {"pass":true},
                         {"reject":"R-MAT-PKE:扫描物料不在 BOM 内,错装/漏装报警"}]}]})"));
    // 质量锁定:已锁车辆不得下线(下线类事件:ProductionReported)。
    reg_rule(defs, rule_payload("R-QUAL-LOCK", {"锁定状态"}, "filter", "",
        R"({"if":[{"==":[{"var":"锁定状态"},"已锁"]},
                  {"reject":"R-QUAL-LOCK:锁定车辆不得下线"},
                  {"pass":true}]})"));
    // presets 写侧强约束示范:应答类事件的固定写入值由 filter 立法强制一致
    // (挂到 MaterialCallAnswered / CallAcknowledged;其余带 presets 的类型
    //  如需强约束照此立法,为免规则爆炸不逐类型配)。
    reg_rule(defs, rule_payload("R-CALL-ANSWER-VAL", {"呼叫状态"}, "filter", "",
        R"({"if":[{"==":[{"write":"呼叫状态"},"已响应"]},
                  {"pass":true},
                  {"reject":"R-CALL-ANSWER-VAL:应答必须把呼叫状态写为已响应"}]})"));

    // -- trigger --
    // 缺陷级别 ≥ 2 → 对同一辆车触发锁定候选(trigger 靠属性谓词自动匹配)。
    reg_rule(defs, rule_payload("R-QUAL-TRIGGER", {"缺陷级别"}, "trigger", "",
        R"({"if":[{">=":[{"write":"缺陷级别"},2]},
                  {"emit":{"type":"VehicleLocked","id":{"cand":"__target"},
                           "锁定状态":"已锁","actor":"rule:R-QUAL-TRIGGER"}},
                  {"noop":true}]})"));
    // 复检 NOK → 对同一辆车发起新返修候选。
    reg_rule(defs, rule_payload("R-REWORK-TRIGGER", {"复检结论"}, "trigger", "",
        R"({"if":[{"==":[{"var":"复检结论"},"NOK"]},
                  {"emit":{"type":"ReworkRequested","id":{"cand":"__target"},
                           "actor":"rule:R-REWORK-TRIGGER"}},
                  {"noop":true}]})"));
    // F15 逐级报警:同检点故障 ≥3 预警科长、≥10 预警部长(检点随候选带出)。
    reg_rule(defs, rule_payload("R-ALM-003", {"同检点故障计数", "检点"}, "trigger", "",
        R"({"if":[{">=":[{"var":"同检点故障计数"},3]},
                  {"emit":{"type":"AlarmEscalated","id":{"cand":"__target"},
                           "报警级别":"科长","检点":{"var":"检点"}}},
                  {"noop":true}]})", "both", {"FaultAlarmed"}));
    reg_rule(defs, rule_payload("R-ALM-010", {"同检点故障计数", "检点"}, "trigger", "",
        R"({"if":[{">=":[{"var":"同检点故障计数"},10]},
                  {"emit":{"type":"AlarmEscalated","id":{"cand":"__target"},
                           "报警级别":"部长","检点":{"var":"检点"}}},
                  {"noop":true}]})", "both", {"FaultAlarmed"}));

    // -- derive(on_types 变化驱动;deps 中含一个该事件写及的键作求值闸,
    //    计数器的 target 键自身可缺省——null→0 起步) --
    // OEE 推导:可用率 × 性能率 × 良品率。
    reg_rule(defs, rule_payload("R-KPI-001", {"可用率", "性能率", "良品率"}, "derive", "OEE",
        R"({"*":[{"var":"可用率"},{"var":"性能率"},{"var":"良品率"}]})"));
    // 达成率 = 实际产量/计划产量(计划产量为 0 → null,不给无意义值)。
    reg_rule(defs, rule_payload("R-KPI-002", {"实际产量", "计划产量"}, "derive", "达成率",
        R"({"if":[{"==":[{"var":"计划产量"},0]},
                  {"return":null},
                  {"return":{"/":[{"var":"实际产量"},{"var":"计划产量"}]}}]})",
        "both", {"ProductionReported", "ReportReversed"}));
    // FTT = 首次合格数/实际产量(实际产量为 0 → null;冲正同样驱动回摆)。
    reg_rule(defs, rule_payload("R-KPI-003", {"首次合格数", "实际产量"}, "derive", "FTT",
        R"({"if":[{"==":[{"var":"实际产量"},0]},
                  {"return":null},
                  {"return":{"/":[{"var":"首次合格数"},{"var":"实际产量"}]}}]})",
        "both", {"ProductionReported", "ReportReversed"}));
    // 缺陷总数:DefectRegistered 结算一次 +1。
    reg_rule(defs, rule_payload("R-QUAL-COUNT", {"缺陷总数", "缺陷级别"}, "derive", "缺陷总数",
        R"({"return":{"+":[{"var":"缺陷总数"},1]}})", "both", {"DefectRegistered"}));
    // 同检点故障计数:FaultAlarmed 结算一次 +1(F15 逐级报警输入)。
    reg_rule(defs, rule_payload("R-ALM-COUNT", {"同检点故障计数", "检点"}, "derive", "同检点故障计数",
        R"({"return":{"+":[{"var":"同检点故障计数"},1]}})", "both", {"FaultAlarmed"}));

    // -- 读侧行过滤(consumers=read:规则决定行——只渲染命中的行,不进写侧) --
    // B6 缺料呼叫看板:只看呼叫中的行。
    reg_rule(defs, rule_payload("R-VIEW-CALL", {"呼叫状态"}, "filter", "",
        R"({"if":[{"==":[{"var":"呼叫状态"},"呼叫中"]},
                  {"pass":true},
                  {"reject":"R-VIEW-CALL:呼叫已响应/取消,不在缺料呼叫看板"}]})", "read"));
    // F13 车辆锁定列表:只显示当前已锁的行(规则决定行)
    reg_rule(defs, rule_payload("R-VIEW-LOCKED", {"锁定状态"}, "filter", "",
        R"({"if":[{"==":[{"var":"锁定状态"},"已锁"]},
                  {"pass":true},
                  {"reject":"R-VIEW-LOCKED:未锁定,不在锁定列表"}]})", "read"));
    // B7-B10 四类拉动单视图:各自只出本类型的行。
    reg_rule(defs, rule_payload("R-VIEW-KANBAN", {"拉动类型"}, "filter", "",
        R"({"if":[{"==":[{"var":"拉动类型"},"Kanban"]},{"pass":true},
                  {"reject":"R-VIEW-KANBAN:非 Kanban 拉动单"}]})", "read"));
    reg_rule(defs, rule_payload("R-VIEW-PULL", {"拉动类型"}, "filter", "",
        R"({"if":[{"==":[{"var":"拉动类型"},"紧急"]},{"pass":true},
                  {"reject":"R-VIEW-PULL:非紧急拉动单"}]})", "read"));
    reg_rule(defs, rule_payload("R-VIEW-JIS", {"拉动类型"}, "filter", "",
        R"({"if":[{"==":[{"var":"拉动类型"},"JIS"]},{"pass":true},
                  {"reject":"R-VIEW-JIS:非 JIS 拉动单"}]})", "read"));
    reg_rule(defs, rule_payload("R-VIEW-JIT", {"拉动类型"}, "filter", "",
        R"({"if":[{"==":[{"var":"拉动类型"},"JIT"]},{"pass":true},
                  {"reject":"R-VIEW-JIT:非 JIT 拉动单"}]})", "read"));
    // E4 故障显示分析:只看设备状态=故障的行。
    reg_rule(defs, rule_payload("R-VIEW-FAULT", {"设备状态"}, "filter", "",
        R"({"if":[{"==":[{"var":"设备状态"},"故障"]},{"pass":true},
                  {"reject":"R-VIEW-FAULT:非故障设备"}]})", "read"));

    // ---- ④ 事件类型(修正类型先于引用它的类型注册) ----
    // -- A 生产计划 --
    reg_type(defs, type_payload("OrderRevoked", {"id", "actor"}, {"车型", "订单状态"}, {}, "", false));
    reg_type(defs, type_payload("OrderReceived", {"id", "actor", "车型"},
                                {"计划产量", "交付期", "订单状态"}, {}, "OrderRevoked", true));
    reg_type(defs, type_payload("OrderRevokeReversed", {"id", "actor"}, {"订单状态"}, {}, "", false));
    reg_type(defs, type_payload("OrderReReceived", {"id", "actor", "车型"},
                                {"订单状态", "交付期", "计划产量"}, {}, "OrderRevokeReversed", false));
    reg_type(defs, type_payload("OrderFreezeReversed", {"id", "actor", "冻结状态"}, {}, {}, "", false,
                                {{"冻结状态", "未冻结"}}));
    reg_type(defs, type_payload("OrderFrozen", {"id", "actor", "冻结状态"}, {}, {}, "OrderFreezeReversed", false,
                                {{"冻结状态", "已冻结"}}));
    reg_type(defs, type_payload("OrderCancelReversed", {"id", "actor", "订单状态"}, {}, {}, "", false));
    reg_type(defs, type_payload("OrderCancelled", {"id", "actor", "订单状态"}, {}, {}, "OrderCancelReversed", false));
    reg_type(defs, type_payload("OrderInsertReversed", {"id", "actor"}, {"序列号"}, {}, "", false));
    reg_type(defs, type_payload("OrderInserted", {"id", "actor", "车型", "序列号"},
                                {"优先级", "交付期"}, {}, "OrderInsertReversed", false));
    reg_type(defs, type_payload("OrderSwapReversed", {"id", "actor"}, {"序列号"}, {}, "", true));
    reg_type(defs, type_payload("OrderSwapped", {"id", "actor", "序列号"}, {},
                                {"R-SEQ-FROZEN"}, "OrderSwapReversed", true));

    reg_type(defs, type_payload("PlanRolledBack", {"id", "actor"}, {"计划状态", "计划产量"}, {}, "", false));
    reg_type(defs, type_payload("PlanReleased", {"id", "actor", "计划状态"}, {"计划产量"},
                                {"R-PLAN-RELEASE"}, "PlanRolledBack", false));

    reg_type(defs, type_payload("SequenceAdjustReversed", {"id", "actor"}, {"优先级", "序列号"}, {}, "", false));
    reg_type(defs, type_payload("SequenceAdjusted", {"id", "actor"}, {"优先级", "序列号"},
                                {"R-PLAN-ADJUST", "R-SEQ-FROZEN"}, "SequenceAdjustReversed", false));

    // -- B 物料 --
    reg_type(defs, type_payload("MaterialRetired", {"id", "actor"}, {"物料编号"}, {}, "", false));
    reg_type(defs, type_payload("MaterialRegistered", {"id", "actor", "物料编号"},
                                {"批次号", "库存数量", "库位", "库存阈值", "供应商"}, {}, "MaterialRetired", false));
    reg_type(defs, type_payload("BomReceived", {"id", "actor", "BOM清单"},
                                {"物料需求", "工艺信息"}, {}, "", false));
    reg_type(defs, type_payload("StockUpdated", {"id", "actor", "库存数量"},
                                {"物料编号", "批次号", "库位"}, {}, "", false));
    reg_type(defs, type_payload("MaterialVerified", {"id", "actor", "物料编号"},
                                {}, {"R-MAT-PKE"}, "", false));
    reg_type(defs, type_payload("BindingVoided", {"id", "actor"}, {"批次绑定", "VIN绑定"}, {}, "", true));
    reg_type(defs, type_payload("BatchBoundToVIN", {"id", "actor"}, {"批次绑定", "VIN绑定"},
                                {}, "BindingVoided", true));
    reg_type(defs, type_payload("MaterialCallCancelled", {"id", "actor", "呼叫状态"}, {"缺料工位"}, {}, "", false,
                                {{"呼叫状态", "已取消"}}));
    reg_type(defs, type_payload("MaterialCallRaised", {"id", "actor", "呼叫状态", "缺料工位"},
                                {}, {}, "MaterialCallCancelled", false,
                                {{"呼叫状态", "呼叫中"}}));
    reg_type(defs, type_payload("MaterialCallAnswered", {"id", "actor", "呼叫状态"}, {},
                                {"R-CALL-ANSWER-VAL"}, "", false,
                                {{"呼叫状态", "已响应"}}));
    reg_type(defs, type_payload("PullOrderCancelled", {"id", "actor", "拉动状态"}, {"拉动类型"}, {}, "", false,
                                {{"拉动状态", "已取消"}}));
    reg_type(defs, type_payload("PullOrderCreated", {"id", "actor", "拉动类型", "拉动状态", "物料编号"},
                                {"库存数量"}, {}, "PullOrderCancelled", false,
                                {{"拉动状态", "已创建"}}));
    reg_type(defs, type_payload("PullOrderShipped", {"id", "actor", "拉动状态"}, {}, {}, "", false,
                                {{"拉动状态", "已发货"}}));
    reg_type(defs, type_payload("PullOrderReceived", {"id", "actor", "拉动状态"}, {}, {}, "", false,
                                {{"拉动状态", "已收货"}}));

    // -- C AVI --
    reg_type(defs, type_payload("PassageCorrected", {"id", "actor"}, {"过点区域"}, {}, "", false));
    reg_type(defs, type_payload("VehicleEnteredZone", {"id", "actor"}, {"过点区域"}, {}, "PassageCorrected", false));
    reg_type(defs, type_payload("VehicleExitedZone", {"id", "actor"}, {"过点区域"}, {}, "PassageCorrected", false));

    // -- D ANDON --
    reg_type(defs, type_payload("CallCancelled", {"id", "actor", "呼叫状态"}, {"呼叫类型"}, {}, "", false,
                                {{"呼叫状态", "已取消"}}));
    reg_type(defs, type_payload("CallRaised", {"id", "actor", "呼叫类型", "呼叫状态"},
                                {}, {}, "CallCancelled", false,
                                {{"呼叫状态", "呼叫中"}}));
    reg_type(defs, type_payload("CallAcknowledged", {"id", "actor", "呼叫状态"}, {},
                                {"R-CALL-ANSWER-VAL"}, "", false,
                                {{"呼叫状态", "已响应"}}));
    reg_type(defs, type_payload("LineStopped", {"id", "actor", "线状态"}, {"停线原因"}, {}, "", false,
                                {{"线状态", "停线"}}));
    reg_type(defs, type_payload("LineResumed", {"id", "actor", "线状态"}, {}, {}, "", false,
                                {{"线状态", "运行"}}));

    // -- E PMC --
    reg_type(defs, type_payload("FaultAlarmed", {"id", "actor", "检点", "故障码"},
                                {"设备状态", "故障级别"}, {}, "", false,
                                {{"设备状态", "故障"}}));
    reg_type(defs, type_payload("FaultCleared", {"id", "actor", "设备状态"}, {"检点", "故障码"}, {}, "", false,
                                {{"设备状态", "运行"}}));
    reg_type(defs, type_payload("CountUpdated", {"id", "actor"},
                                {"产量计数", "停线计数", "首次合格数", "缓冲区计数"}, {}, "", false));
    reg_type(defs, type_payload("AlarmAcknowledged", {"id", "actor"}, {"报警级别", "检点"}, {}, "", false));
    reg_type(defs, type_payload("AlarmEscalated", {"id", "actor", "报警级别"}, {"检点"},
                                {}, "AlarmAcknowledged", false));

    // -- F 质量 --
    reg_type(defs, type_payload("DefectCancelled", {"id", "actor"}, {"车漆", "缺陷级别"}, {}, "", false));
    reg_type(defs, type_payload("DefectRegistered", {"id", "actor", "车漆", "缺陷级别"},
                                {"evidence"}, {}, "DefectCancelled", false));
    reg_type(defs, type_payload("VehicleUnlocked", {"id", "actor"}, {"锁定状态"}, {}, "", false,
                                {{"锁定状态", "未锁"}}));
    reg_type(defs, type_payload("VehicleLocked", {"id", "actor", "锁定状态"}, {}, {}, "VehicleUnlocked", false,
                                {{"锁定状态", "已锁"}}));
    reg_type(defs, type_payload("ReworkCancelled", {"id", "actor"}, {}, {}, "", false));
    reg_type(defs, type_payload("ReworkRequested", {"id", "actor"}, {}, {}, "ReworkCancelled", false));
    reg_type(defs, type_payload("ReworkRecorded", {"id", "actor", "返修内容"}, {}, {}, "", false));
    reg_type(defs, type_payload("ReworkSubmitted", {"id", "actor"}, {}, {}, "", false));
    reg_type(defs, type_payload("JudgementOverruled", {"id", "actor"}, {"复检结论"}, {}, "", false));
    reg_type(defs, type_payload("RecheckJudged", {"id", "actor", "复检结论"}, {}, {}, "JudgementOverruled", false));
    reg_type(defs, type_payload("ReworkClosed", {"id", "actor"}, {}, {}, "", false));
    reg_type(defs, type_payload("InspectionPlanIssued", {"id", "actor", "检验计划号"}, {}, {}, "", false));
    reg_type(defs, type_payload("InspectionTaskAssigned", {"id", "actor", "检验工位"},
                                {"检验计划号"}, {}, "", false));
    reg_type(defs, type_payload("InspectionDataCorrected", {"id", "actor"},
                                {"尾气检测值", "大灯检测值", "淋雨结论"}, {}, "", false));
    reg_type(defs, type_payload("InspectionDataRecorded", {"id", "actor"},
                                {"尾气检测值", "大灯检测值", "淋雨结论"}, {}, "InspectionDataCorrected", false));

    // -- G 设备 --
    reg_type(defs, type_payload("EquipmentStatusCorrected", {"id", "actor"},
                                {"设备编号", "设备状态", "刀具寿命"}, {}, "", false));
    reg_type(defs, type_payload("EquipmentStatusReported", {"id", "actor", "设备编号", "设备状态"},
                                {"刀具寿命"}, {}, "EquipmentStatusCorrected", false));

    // -- H 集成 --
    reg_type(defs, type_payload("InboundMessageVoided", {"id", "actor"},
                                {"来源系统", "报文类型", "外部单号"}, {}, "", false));
    reg_type(defs, type_payload("InboundMessageRecorded", {"id", "actor", "来源系统", "报文类型", "外部单号"},
                                {}, {}, "InboundMessageVoided", false));

    // -- 报工/组成声明/entity 桥(既有) --
    reg_type(defs, type_payload("ReportReversed", {"id", "actor"}, {"报工数量", "实际产量"}, {}, "", false));
    reg_type(defs, type_payload("ProductionReported", {"id", "actor", "报工数量"}, {"实际产量"},
                                {"R-QUAL-LOCK"}, "ReportReversed", true));
    reg_type(defs, type_payload("CompositionDeclared", {"id", "actor", "aggregates"},
                                {"可用率", "性能率", "良品率"}, {}, "", true));
    reg_type(defs, type_payload("EntityObserved", {"id", "actor"}, {}, {}, "", false));

    // ---- ⑤ 视图(引用的键/规则/事件类型均已注册) ----
    // ============ A 生产计划 ============
    // A1 生产订单接收列表(终态):接收/删除/重接收。
    reg_view(defs, {
        {"view_id", "V-ORDER-A1"},
        {"queries", {{"events", {"OrderReceived", "OrderRevoked", "OrderReReceived",
                                 "OrderCancelled", "OrderCancelReversed"}},
                     {"fold", "L1"}}},
        {"selects", {"车型", "订单状态", "交付期", "计划产量"}},
        {"rules", json::array()},
        {"emits", {"OrderReceived", "OrderRevoked", "OrderReReceived"}},
        {"render_mode", "终态"}});
    // A2 生产排序视图(终态·有序):冻结/撤单/插单/换单/序列调整。
    reg_view(defs, {
        {"view_id", "V-SEQ-A2"},
        {"queries", {{"events", {"OrderReceived", "OrderFrozen", "OrderFreezeReversed",
                                 "OrderSwapped", "OrderInserted", "SequenceAdjusted",
                                 "SequenceAdjustReversed", "OrderCancelled"}},
                     {"fold", "L1"}}},
        {"selects", {"序列号", "车型", "优先级", "冻结状态"}},
        {"rules", json::array()},
        {"emits", {"OrderFrozen", "OrderSwapped", "SequenceAdjusted", "OrderInserted"}},
        {"render_mode", "终态"}});
    // A3 作业计划下发队列(既有):行可见、按钮按状态机置灰(rules 留空,理由见既有注释)。
    reg_view(defs, {
        {"view_id", "V-PLAN-A3"},
        {"queries", {{"events", {"OrderReceived", "PlanReleased", "PlanRolledBack", "SequenceAdjusted"}},
                     {"fold", "L1"}}},
        {"selects", {"车型", "优先级", "计划状态"}},
        {"rules", json::array()},
        {"emits", {"PlanReleased", "PlanRolledBack", "SequenceAdjusted"}},
        {"render_mode", "终态"},
        {"variants", {{"计划员", json::object()},
                      {"调度", {{"columns", {"车型", "计划状态"}}}}}}});
    // A4 工位作业指导(终态):工位本体的物料需求/工艺信息 + 在站车辆的车型同屏。
    reg_view(defs, {
        {"view_id", "V-STATION-A4"},
        {"queries", {{"events", {"BomReceived", "VehicleEnteredZone"}}, {"fold", "L1"}}},
        {"selects", {"车型", "物料需求", "工艺信息"}},
        {"rules", json::array()},
        {"emits", {"MaterialVerified"}},
        {"render_mode", "终态"}});
    // A5 生产状态监控(终态·时空切片:总装车间)。
    reg_view(defs, {
        {"view_id", "V-MONITOR-A5"},
        {"queries", {{"events", {"VehicleEnteredZone", "VehicleExitedZone", "ProductionReported"}},
                     {"fold", "L1"}, {"spacetime", "整车厂/总装车间"}}},
        {"selects", {"车型", "过点区域", "实际产量", "报工数量"}},
        {"rules", json::array()},
        {"emits", {"ProductionReported"}},
        {"render_mode", "终态"}});
    // A6 生产效率分析(终态上卷):计划/实际/达成率(派生)同屏。
    reg_view(defs, {
        {"view_id", "V-EFF-A6"},
        {"queries", {{"events", {"OrderReceived", "PlanReleased", "ProductionReported",
                                 "ReportReversed", "CompositionDeclared"}},
                     {"fold", "L1"}}},
        {"selects", {"计划产量", "实际产量", "达成率"}},
        {"rules", json::array()},
        {"emits", {"ProductionReported"}},
        {"render_mode", "终态"}});
    // A7 报工流水(流水,L1+L2:报工与冲正同列)。
    reg_view(defs, {
        {"view_id", "V-REPORT-A7"},
        {"queries", {{"events", {"ProductionReported", "ReportReversed"}}, {"fold", "L1+L2"}}},
        {"selects", {"报工数量", "实际产量"}},
        {"rules", json::array()},
        {"emits", {"ProductionReported", "ReportReversed"}},
        {"render_mode", "流水"}});

    // ============ B 物料 ============
    // B1 物料主数据列表(终态)。
    reg_view(defs, {
        {"view_id", "V-MAT-B1"},
        {"queries", {{"events", {"MaterialRegistered", "MaterialRetired"}}, {"fold", "L1"}}},
        {"selects", {"物料编号", "批次号", "库存数量", "库位", "库存阈值", "供应商"}},
        {"rules", json::array()},
        {"emits", {"MaterialRegistered"}},
        {"render_mode", "终态"}});
    // B2 BOM 结构(终态;entity 参数取某个工位/订单的 BOM 断面)。
    reg_view(defs, {
        {"view_id", "V-BOM-B2"},
        {"queries", {{"events", {"BomReceived"}}, {"fold", "L1"}}},
        {"selects", {"BOM清单", "物料需求", "工艺信息"}},
        {"rules", json::array()},
        {"emits", {"BomReceived"}},
        {"render_mode", "终态"}});
    // B3 实时库存(终态)。
    reg_view(defs, {
        {"view_id", "V-STOCK-B3"},
        {"queries", {{"events", {"StockUpdated"}}, {"fold", "L1"}}},
        {"selects", {"物料编号", "批次号", "库存数量", "库位"}},
        {"rules", json::array()},
        {"emits", {"StockUpdated"}},
        {"render_mode", "终态"}});
    // B4 防错防漏校验(拦截模式):主角是 L0-L3 拦截的即时反馈。
    reg_view(defs, {
        {"view_id", "V-PKE-B4"},
        {"queries", {{"events", {"MaterialVerified"}}, {"fold", "L1"}}},
        {"selects", {"物料编号"}},
        {"rules", json::array()},
        {"emits", {"MaterialVerified"}},
        {"render_mode", "拦截"}});
    // B5 物料追溯(遍历):VIN→批次→供应商,正向追踪/反向追溯。
    reg_view(defs, {
        {"view_id", "V-TRACE-B5"},
        {"queries", {{"events", {"BatchBoundToVIN", "BindingVoided", "MaterialRegistered"}},
                     {"fold", "L1"}}},
        {"selects", {"批次绑定", "VIN绑定", "供应商", "物料编号", "批次号"}},
        {"rules", json::array()},
        {"emits", {"BatchBoundToVIN", "BindingVoided"}},
        {"render_mode", "遍历"}});
    // B6 缺料呼叫看板(终态;规则决定行:只出呼叫中)。
    reg_view(defs, {
        {"view_id", "V-CALL-B6"},
        {"queries", {{"events", {"MaterialCallRaised", "MaterialCallAnswered",
                                 "MaterialCallCancelled"}},
                     {"fold", "L1"}}},
        {"selects", {"呼叫状态", "缺料工位"}},
        {"rules", {"R-VIEW-CALL"}},
        {"emits", {"MaterialCallRaised", "MaterialCallAnswered", "MaterialCallCancelled"}},
        {"render_mode", "终态"}});
    // B7-B10 四类拉动单视图(终态;同一事件集,读侧规则各出本类型的行;
    // 视图级 emit_presets 把发起 PullOrderCreated 的拉动类型固定为本视图口径)。
    reg_view(defs, {
        {"view_id", "V-KANBAN-B7"},
        {"queries", {{"events", {"PullOrderCreated", "PullOrderShipped", "PullOrderReceived",
                                 "PullOrderCancelled"}},
                     {"fold", "L1"}}},
        {"selects", {"拉动类型", "拉动状态", "物料编号"}},
        {"rules", {"R-VIEW-KANBAN"}},
        {"emits", {"PullOrderCreated", "PullOrderShipped", "PullOrderReceived", "PullOrderCancelled"}},
        {"emit_presets", {{"PullOrderCreated", {{"拉动类型", "Kanban"}}}}},
        {"render_mode", "终态"}});
    reg_view(defs, {
        {"view_id", "V-PULL-B8"},
        {"queries", {{"events", {"PullOrderCreated", "PullOrderShipped", "PullOrderReceived",
                                 "PullOrderCancelled"}},
                     {"fold", "L1"}}},
        {"selects", {"拉动类型", "拉动状态", "物料编号"}},
        {"rules", {"R-VIEW-PULL"}},
        {"emits", {"PullOrderCreated", "PullOrderShipped", "PullOrderReceived", "PullOrderCancelled"}},
        {"emit_presets", {{"PullOrderCreated", {{"拉动类型", "紧急"}}}}},
        {"render_mode", "终态"}});
    reg_view(defs, {
        {"view_id", "V-JIS-B9"},
        {"queries", {{"events", {"PullOrderCreated", "PullOrderShipped", "PullOrderReceived",
                                 "PullOrderCancelled"}},
                     {"fold", "L1"}}},
        {"selects", {"拉动类型", "拉动状态", "物料编号"}},
        {"rules", {"R-VIEW-JIS"}},
        {"emits", {"PullOrderCreated", "PullOrderShipped", "PullOrderReceived", "PullOrderCancelled"}},
        {"emit_presets", {{"PullOrderCreated", {{"拉动类型", "JIS"}}}}},
        {"render_mode", "终态"}});
    reg_view(defs, {
        {"view_id", "V-JIT-B10"},
        {"queries", {{"events", {"PullOrderCreated", "PullOrderShipped", "PullOrderReceived",
                                 "PullOrderCancelled"}},
                     {"fold", "L1"}}},
        {"selects", {"拉动类型", "拉动状态", "物料编号"}},
        {"rules", {"R-VIEW-JIT"}},
        {"emits", {"PullOrderCreated", "PullOrderShipped", "PullOrderReceived", "PullOrderCancelled"}},
        {"emit_presets", {{"PullOrderCreated", {{"拉动类型", "JIT"}}}}},
        {"render_mode", "终态"}});
    // B11 线边库库存(终态·时空切片:线边库)。
    reg_view(defs, {
        {"view_id", "V-LINESIDE-B11"},
        {"queries", {{"events", {"StockUpdated"}}, {"fold", "L1"},
                     {"spacetime", "整车厂/总装车间/线边库"}}},
        {"selects", {"物料编号", "批次号", "库存数量", "库位", "库存阈值"}},
        {"rules", json::array()},
        {"emits", {"StockUpdated"}},
        {"render_mode", "终态"}});

    // ============ C AVI ============
    // C1 车辆实时位置(既有,终态)。
    reg_view(defs, {
        {"view_id", "V-AVI-C1"},
        {"queries", {{"events", {"VehicleEnteredZone", "PassageCorrected"}}, {"fold", "L1"}}},
        {"selects", {"过点区域", "车型"}},
        {"rules", json::array()},
        {"emits", {"VehicleEnteredZone"}},
        {"render_mode", "终态"}});
    // C2 车辆跟踪查询(流水,entity 纵切:单车过点史,L1+L2 补录同列)。
    reg_view(defs, {
        {"view_id", "V-TRACK-C2"},
        {"queries", {{"events", {"VehicleEnteredZone", "VehicleExitedZone", "PassageCorrected"}},
                     {"fold", "L1+L2"}}},
        {"selects", {"过点区域"}},
        {"rules", json::array()},
        {"emits", {"VehicleEnteredZone", "VehicleExitedZone", "PassageCorrected"}},
        {"render_mode", "流水"}});
    // C3 区域跟踪(终态·时空切片:总装车间)。
    reg_view(defs, {
        {"view_id", "V-ZONE-C3"},
        {"queries", {{"events", {"VehicleEnteredZone", "VehicleExitedZone"}}, {"fold", "L1"},
                     {"spacetime", "整车厂/总装车间"}}},
        {"selects", {"过点区域", "车型"}},
        {"rules", json::array()},
        {"emits", {"VehicleEnteredZone"}},
        {"render_mode", "终态"}});

    // ============ D ANDON ============
    // D1 安灯状态大屏(终态:产线行的实时状态断面)。
    reg_view(defs, {
        {"view_id", "V-ANDON-D1"},
        {"queries", {{"events", {"CallRaised", "CallAcknowledged", "CallCancelled",
                                 "LineStopped", "LineResumed", "CountUpdated"}},
                     {"fold", "L1"}}},
        {"selects", {"线状态", "停线原因", "缓冲区计数", "呼叫状态"}},
        {"rules", json::array()},
        {"emits", {"CallRaised", "CallAcknowledged", "CallCancelled",
                    "LineStopped", "LineResumed"}},
        {"render_mode", "终态"}});
    // D2 生产目标达成看板(既有)。
    reg_view(defs, {
        {"view_id", "V-REPORT-D2"},
        {"queries", {{"events", {"ProductionReported", "ReportReversed", "CompositionDeclared"}}, {"fold", "L1"}}},
        {"selects", {"计划产量", "实际产量", "OEE"}},
        {"rules", json::array()},
        {"emits", {"ProductionReported"}},
        {"render_mode", "终态"}});
    // D3 呼叫与停线记录(流水;emits 含 CallCancelled:流水行内可发起冲正)。
    reg_view(defs, {
        {"view_id", "V-CALLLOG-D3"},
        {"queries", {{"events", {"CallRaised", "CallAcknowledged", "CallCancelled",
                                 "LineStopped", "LineResumed"}},
                     {"fold", "L1"}}},
        {"selects", {"呼叫类型", "呼叫状态", "线状态", "停线原因"}},
        {"rules", json::array()},
        {"emits", {"CallRaised", "CallAcknowledged", "CallCancelled",
                    "LineStopped", "LineResumed"}},
        {"render_mode", "流水"}});

    // ============ E PMC ============
    // E1 工艺设备实时监控(终态)。
    reg_view(defs, {
        {"view_id", "V-PMC-E1"},
        {"queries", {{"events", {"FaultAlarmed", "FaultCleared", "EquipmentStatusReported",
                                 "EquipmentStatusCorrected"}},
                     {"fold", "L1"}}},
        {"selects", {"设备编号", "设备状态", "故障码", "故障级别"}},
        {"rules", json::array()},
        {"emits", {"FaultAlarmed", "FaultCleared"}},
        {"render_mode", "终态"}});
    // E2 产量与停线计数(终态上卷)。
    reg_view(defs, {
        {"view_id", "V-COUNT-E2"},
        {"queries", {{"events", {"CountUpdated"}}, {"fold", "L1"}}},
        {"selects", {"产量计数", "停线计数", "首次合格数", "缓冲区计数"}},
        {"rules", json::array()},
        {"emits", {"CountUpdated"}},
        {"render_mode", "终态"}});
    // E3 产量统计图表(终态上卷:图表数据源 = 计数与报工的终态行)。
    reg_view(defs, {
        {"view_id", "V-CHART-E3"},
        {"queries", {{"events", {"CountUpdated", "ProductionReported"}}, {"fold", "L1"}}},
        {"selects", {"产量计数", "实际产量", "计划产量"}},
        {"rules", json::array()},
        {"emits", {"CountUpdated"}},
        {"render_mode", "终态"}});
    // E4 故障显示分析(终态;规则决定行:只出设备状态=故障)。
    reg_view(defs, {
        {"view_id", "V-FAULT-E4"},
        {"queries", {{"events", {"FaultAlarmed", "FaultCleared", "EquipmentStatusReported",
                                 "EquipmentStatusCorrected"}},
                     {"fold", "L1"}}},
        {"selects", {"设备编号", "设备状态", "故障码", "故障级别"}},
        {"rules", {"R-VIEW-FAULT"}},
        {"emits", {"FaultAlarmed", "FaultCleared"}},
        {"render_mode", "终态"}});
    // E5 故障报警查询(流水)。
    reg_view(defs, {
        {"view_id", "V-ALARM-E5"},
        {"queries", {{"events", {"FaultAlarmed", "FaultCleared"}}, {"fold", "L1"}}},
        {"selects", {"检点", "故障码", "设备状态"}},
        {"rules", json::array()},
        {"emits", {"FaultAlarmed", "FaultCleared"}},
        {"render_mode", "流水"}});
    // F15 报警列表(流水,L1+L2:升级与知悉同列;规则侧由 R-ALM-003/010 承载)。
    reg_view(defs, {
        {"view_id", "V-ALARM-F15"},
        {"queries", {{"events", {"AlarmEscalated", "AlarmAcknowledged"}}, {"fold", "L1+L2"}}},
        {"selects", {"报警级别", "检点"}},
        {"rules", json::array()},
        {"emits", {"AlarmEscalated", "AlarmAcknowledged"}},
        {"render_mode", "流水"}});

    // ============ F 质量 ============
    // F2 检验计划(终态)。
    reg_view(defs, {
        {"view_id", "V-PLAN-F2"},
        {"queries", {{"events", {"InspectionPlanIssued"}}, {"fold", "L1"}}},
        {"selects", {"检验计划号"}},
        {"rules", json::array()},
        {"emits", {"InspectionPlanIssued"}},
        {"render_mode", "终态"}});
    // F4 检验任务分配(终态)。
    reg_view(defs, {
        {"view_id", "V-TASK-F4"},
        {"queries", {{"events", {"InspectionTaskAssigned"}}, {"fold", "L1"}}},
        {"selects", {"检验计划号", "检验工位"}},
        {"rules", json::array()},
        {"emits", {"InspectionTaskAssigned"}},
        {"render_mode", "终态"}});
    // F7 车辆参数详情(终态,entity 纵切单车断面)。
    reg_view(defs, {
        {"view_id", "V-CAR-F7"},
        {"queries", {{"events", {"OrderReceived", "VehicleEnteredZone", "InspectionDataRecorded",
                                 "DefectRegistered"}},
                     {"fold", "L1"}}},
        {"selects", {"车型", "过点区域", "尾气检测值", "大灯检测值", "淋雨结论", "锁定状态"}},
        {"rules", json::array()},
        {"emits", json::array()},
        {"render_mode", "终态"}});
    // F8 电子随车卡(终态,entity 当前断面 + 可发起操作)。
    reg_view(defs, {
        {"view_id", "V-CARD-F8"},
        {"queries", {{"events", {"OrderReceived", "VehicleEnteredZone", "VehicleExitedZone",
                                 "InspectionDataRecorded"}},
                     {"fold", "L1"}}},
        {"selects", {"车型", "过点区域", "计划状态", "锁定状态", "复检结论"}},
        {"rules", json::array()},
        {"emits", {"InspectionDataRecorded", "DefectRegistered"}},
        {"render_mode", "终态"}});
    // F9 车辆位置历史(流水,entity 纵切:到达/离开时间戳)。
    reg_view(defs, {
        {"view_id", "V-HIST-F9"},
        {"queries", {{"events", {"VehicleEnteredZone", "VehicleExitedZone"}}, {"fold", "L1"}}},
        {"selects", {"过点区域"}},
        {"rules", json::array()},
        {"emits", json::array()},
        {"render_mode", "流水"}});
    // F10 缺陷采集(拦截模式):误录/越界候选的即时拦截反馈。
    reg_view(defs, {
        {"view_id", "V-DEFECT-F10"},
        {"queries", {{"events", {"DefectRegistered"}}, {"fold", "L1"}}},
        {"selects", {"车漆", "缺陷级别"}},
        {"rules", json::array()},
        {"emits", {"DefectRegistered"}},
        {"render_mode", "拦截"}});
    // F11 返修流程(既有,流水 + 角色变体;emits 含 JudgementOverruled/
    // ReworkCancelled:流水行内可发起改判/撤销冲正)。
    reg_view(defs, {
        {"view_id", "V-REWORK-001"},
        {"queries", {{"events", {"DefectRegistered", "DefectCancelled",
                                 "ReworkRequested", "ReworkRecorded", "ReworkSubmitted",
                                 "RecheckJudged", "JudgementOverruled", "ReworkClosed"}},
                     {"fold", "L1+L2"}}},
        {"selects", {"车型", "车漆", "返修内容", "复检结论"}},
        {"rules", json::array()},
        {"emits", {"ReworkRequested", "ReworkCancelled", "ReworkRecorded",
                    "ReworkSubmitted", "RecheckJudged", "JudgementOverruled",
                    "ReworkClosed"}},
        {"render_mode", "流水"},
        {"variants", {{"返修工", {{"columns", {"车型", "返修内容"}},
                                 {"emits", {"ReworkRecorded", "ReworkSubmitted"}}}},
                      {"复检员", {{"columns", {"车型", "复检结论"}},
                                 {"emits", {"RecheckJudged"}}}}}}});
    // F12 车辆质量记录(流水,entity 纵切:检测设备数据,L1+L2 修正同列)。
    reg_view(defs, {
        {"view_id", "V-QUALREC-F12"},
        {"queries", {{"events", {"InspectionDataRecorded", "InspectionDataCorrected"}},
                     {"fold", "L1+L2"}}},
        {"selects", {"尾气检测值", "大灯检测值", "淋雨结论"}},
        {"rules", json::array()},
        {"emits", {"InspectionDataRecorded", "InspectionDataCorrected"}},
        {"render_mode", "流水"}});
    // F13 车辆锁定列表(既有)。
    reg_view(defs, {
        {"view_id", "V-LOCK-F13"},
        {"queries", {{"events", {"VehicleLocked", "VehicleUnlocked"}}, {"fold", "L1"}}},
        {"selects", {"锁定状态", "车漆", "缺陷级别"}},
        {"rules", {"R-VIEW-LOCKED"}},
        {"emits", {"VehicleUnlocked"}},
        {"render_mode", "终态"}});
    // F14 车辆追溯(遍历):车辆→批次→供应商,法规件追溯。
    reg_view(defs, {
        {"view_id", "V-TRACE-F14"},
        {"queries", {{"events", {"VehicleEnteredZone", "BatchBoundToVIN", "DefectRegistered",
                                 "RecheckJudged"}},
                     {"fold", "L1"}}},
        {"selects", {"批次绑定", "VIN绑定", "供应商", "过点区域", "车型"}},
        {"rules", json::array()},
        {"emits", json::array()},
        {"render_mode", "遍历"}});
    // F16 质量综合报表(终态上卷):缺陷总数/FTT/实际产量。
    reg_view(defs, {
        {"view_id", "V-QUALITY-F16"},
        {"queries", {{"events", {"DefectRegistered", "ProductionReported", "CountUpdated"}},
                     {"fold", "L1"}}},
        {"selects", {"缺陷总数", "FTT", "实际产量"}},
        {"rules", json::array()},
        {"emits", json::array()},
        {"render_mode", "终态"}});

    // ============ G 设备 ============
    // G1 设备台账/运维列表(终态)。
    reg_view(defs, {
        {"view_id", "V-EQ-G1"},
        {"queries", {{"events", {"EquipmentStatusReported", "EquipmentStatusCorrected"}},
                     {"fold", "L1"}}},
        {"selects", {"设备编号", "设备状态", "刀具寿命"}},
        {"rules", json::array()},
        {"emits", {"EquipmentStatusReported"}},
        {"render_mode", "终态"}});
    // G2 设备状态详情(终态,entity 纵切单台设备)。
    reg_view(defs, {
        {"view_id", "V-EQ-G2"},
        {"queries", {{"events", {"EquipmentStatusReported", "EquipmentStatusCorrected",
                                 "FaultAlarmed", "FaultCleared"}},
                     {"fold", "L1"}}},
        {"selects", {"设备编号", "设备状态", "刀具寿命", "故障码"}},
        {"rules", json::array()},
        {"emits", {"EquipmentStatusReported"}},
        {"render_mode", "终态"}});

    // ============ H 集成 ============
    // H3 系统集成接口监控(流水,L1+L2:报文与作废同列)。
    reg_view(defs, {
        {"view_id", "V-INTEG-H3"},
        {"queries", {{"events", {"InboundMessageRecorded", "InboundMessageVoided"}},
                     {"fold", "L1+L2"}}},
        {"selects", {"来源系统", "报文类型", "外部单号"}},
        {"rules", json::array()},
        {"emits", {"InboundMessageRecorded", "InboundMessageVoided"}},
        {"render_mode", "流水"}});

    // ============ P4 适配层:PLC 迁移沿(信任分级 + 异步结算的展示实例) ============
    // 高频机器信号不是事件类型:原始流经适配层三层过滤(见 plc_filter.h),
    // 迁移沿经聚合网关翻译成 PlcEdgeReported 候选。min_trust=1:仅 PLC 网关级
    // 凭证可提交(无凭证的 HTTP 提交在 L2 被拦);settlement=async:四层校验
    // 后入队,drain_async 统一串行结算。修正配平:PlcEdgeVoided 先注册。
    reg_attr(defs, attr_payload("工位占用",
        "工位占用状态(PLC 光电信号经适配层三层过滤后的可信迁移沿)",
        "enum", "", {"空闲", "占用"}, {"PlcEdgeReported", "PlcEdgeVoided"}));
    {
        json p = type_payload("PlcEdgeVoided", {"id", "actor"}, {"工位占用"}, {}, "", false);
        p["min_trust"] = 1;  // PLC 网关级
        reg_type(defs, p);
    }
    {
        json p = type_payload("PlcEdgeReported", {"id", "actor", "工位占用"}, {}, {},
                              "PlcEdgeVoided", false);
        p["settlement"] = "async";  // 高频入口:已验未结悬在队列,drain 时串行落账
        p["min_trust"]  = 1;        // PLC 网关级
        reg_type(defs, p);
    }

    // ---- 回执总账:种子是可信定义,rejected 即 bug ----
    std::printf("[seeds] 种子定义结算:%d/%d 成功\n", g_tally.ok, g_tally.total);
}

} // namespace mse
