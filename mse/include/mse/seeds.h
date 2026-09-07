#pragma once

#include <string>

// ============================================================================
// mse/seeds.h —— 种子定义:系统键 + 整车厂业务打样
//
// 依据《开发说明》:业务开发 = 四件定义工作(编词典、立行为、立法、开窗口),
// 产出的是定义数据。种子 = 经 DefinitionLayer 结算的首批定义事件。
// ============================================================================

namespace mse {

class DefinitionLayer;

/// 系统键(一切皆属性:type/id/actor/space/time/evidence/corrects/
/// idempotency_key/aggregates 也须在字典登记,writers="*")。
void load_system_keys(DefinitionLayer& defs);

/// 整车厂打样定义(西门子 46 视图案例的子集实例化):
///   字典:状态/计划状态/车漆/缺陷级别/锁定状态/返修内容/复检结论/报工数量/
///        过点区域/车型/班次产量/可用率/性能率/良品率/OEE(派生) 等
///        (方面名 + 值域 + 单位与语义),含 entity 演化桥直写的观测键
///        (观测标识/观测来源/载具类型,writers=EntityObserved)
///   行为:OrderReceived/PlanReleased/PlanRolledBack/VehicleEnteredZone/
///        DefectRegistered/DefectCancelled/VehicleLocked/VehicleUnlocked/
///        ReworkRequested/ReworkRecorded/ReworkSubmitted/RecheckJudged/
///        JudgementOverruled/ReworkClosed/ProductionReported/ReportReversed/
///        CompositionDeclared(修正类型配平)
///   立法:A3 五状态机过滤族(01/02 可调、03/04 须退回、05 绝对禁止)、
///        缺陷级别≥2 → 车辆须锁定、锁定车辆不得下线、复检 NOK → 触发新返修
///        候选、车间产量推导(derive)
///   开窗口:V-PLAN-A3(作业计划下发队列·终态)、V-AVI-C1(车辆实时位置·
///        终态)、V-REWORK-001(返修流程·流水+角色变体)、V-LOCK-F13(车辆
///        锁定列表·终态)、V-REPORT-D2(生产目标达成看板·上卷以终态呈现)
///   锚点:整车厂/总装车间/总装线/工位01..03、返修区、终检工位
void load_auto_plant_seeds(DefinitionLayer& defs);

/// WASM 种子规则(规则即 WASM 全链路打样):读 assets_dir/wasm/plan_adjust.wasm,
/// base64 后经烘焙管线注册 R-PLAN-ADJUST-WASM(filter,deps=["计划状态"])。
/// 产物文件缺失不致命(stderr 提示后跳过);烘焙失败则打回执并跳过。
void load_wasm_seeds(DefinitionLayer& defs, const std::string& assets_dir);

} // namespace mse
