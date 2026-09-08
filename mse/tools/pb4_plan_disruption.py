#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# mse/tools/pb4_plan_disruption.py —— 剧本4:计划异常日
#
# 剧情:计划员最倒霉的一天。客户来电撤单又反悔(撤单/恢复/重接收三段);
# 紧急插单、换单;VIN-PD01 下到 03 想调序被拦,退回 01 才调成,一路推到
# 05 再调序被原生规则 + WASM 沙盒双拦截;统计员误报工,冲正后达成率回摆;
# 排序冻结令一下,换单立刻被 R-SEQ-FROZEN 挡在门外。
#
# 自管理:自建 Server(临时库 + 随机端口),全部数据本剧本自造。
# 用法:python mse/tools/pb4_plan_disruption.py   退出码 0 = 全部符合预期。
# ============================================================================
import json
from playbook_lib import (Server, set_base, post, view, row_of, events_of,
                          check, banner, summary)

with Server() as s:
    set_base(s.base)
    banner("剧本4:计划异常日")

    # ---- 第一幕 撤单与恢复(计划员 planner-wang,客户电话来回) --------------
    print("\n[第一幕] 撤单与恢复")
    r = post("OrderReceived", "VIN-PD01",
             {"车型": "SUV-A", "订单状态": "接收", "交付期": "2026-09-12"},
             "planner-wang")
    check(r["status"] == "settled", "1.1 接收订单 VIN-PD01")
    row = row_of(view("V-ORDER-A1"), "VIN-PD01")
    check(row is not None and row.get("订单状态") == "接收",
          "1.2 A1 行:订单状态=接收")
    r = post("OrderRevoked", "VIN-PD01", {"订单状态": "取消"}, "planner-wang")
    check(r["status"] == "settled", "1.3 客户来电:撤单")
    revoked_id = r["event_id"]
    row = row_of(view("V-ORDER-A1"), "VIN-PD01")
    check(row is not None and row.get("订单状态") == "取消",
          "1.4 A1 行状态变化:订单状态=取消")
    # OrderRevokeReversed 现已获授权写 订单状态(seeds 修复:修正类型应能
    # 写它要修正的属性)——恢复事件直接携带恢复后的状态,不再只是空留痕
    r = post("OrderRevokeReversed", "VIN-PD01", {"订单状态": "接收"},
             "planner-wang", corrects=revoked_id)
    check(r["status"] == "settled" and r.get("event_id"),
          "1.5 客户反悔:OrderRevokeReversed 修正配平(corrects 指向撤单,"
          "恢复 订单状态=接收)",
          json.dumps(r, ensure_ascii=False))
    row = row_of(view("V-ORDER-A1"), "VIN-PD01")
    check(row is not None and row.get("订单状态") == "接收",
          "1.6 恢复后 A1 行状态回到 接收")
    r = post("OrderReReceived", "VIN-PD01",
             {"车型": "SUV-A", "订单状态": "接收", "交付期": "2026-09-12"},
             "planner-wang")
    check(r["status"] == "settled", "1.7 重接收 OrderReReceived(恢复写入随它落)")
    row = row_of(view("V-ORDER-A1"), "VIN-PD01")
    check(row is not None and row.get("订单状态") == "接收",
          "1.8 A1 行恢复:订单状态=接收")
    check(len(events_of("OrderRevoked")) >= 1
          and len(events_of("OrderRevokeReversed")) >= 1
          and len(events_of("OrderReReceived")) >= 1,
          "1.9 撤单/恢复/重接收三段永存(事件流可查)")

    # ---- 第二幕 插单与换单(计划员) ----------------------------------------
    print("\n[第二幕] 插单与换单")
    r = post("SequenceAdjusted", "VIN-PD01", {"序列号": 1}, "planner-wang")
    check(r["status"] == "settled", "2.1 定序 VIN-PD01 序列号=1")
    r = post("OrderReceived", "VIN-PD02",
             {"车型": "Sedan-B", "订单状态": "接收", "交付期": "2026-09-15"},
             "planner-wang")
    check(r["status"] == "settled", "2.2 接收订单 VIN-PD02")
    r = post("SequenceAdjusted", "VIN-PD02", {"序列号": 2}, "planner-wang")
    check(r["status"] == "settled", "2.3 定序 VIN-PD02 序列号=2")
    r = post("OrderReceived", "VIN-PD03",
             {"车型": "MPV-C", "订单状态": "接收", "交付期": "2026-09-18"},
             "planner-wang")
    check(r["status"] == "settled", "2.4 接收订单 VIN-PD03")
    r = post("SequenceAdjusted", "VIN-PD03", {"序列号": 3}, "planner-wang")
    check(r["status"] == "settled", "2.5 定序 VIN-PD03 序列号=3")
    v = view("V-SEQ-A2")
    check(row_of(v, "VIN-PD01").get("序列号") == 1
          and row_of(v, "VIN-PD02").get("序列号") == 2
          and row_of(v, "VIN-PD03").get("序列号") == 3,
          "2.6 A2 排序视图:1/2/3 就位")
    r = post("OrderInserted", "VIN-PD04",
             {"车型": "EV-D", "序列号": 2, "优先级": 1, "交付期": "2026-09-11"},
             "planner-wang")
    check(r["status"] == "settled", "2.7 紧急插单 VIN-PD04 插队序列号=2")
    row = row_of(view("V-SEQ-A2"), "VIN-PD04")
    check(row is not None and row.get("序列号") == 2
          and row.get("优先级") == 1,
          "2.8 A2 插单行可见(序列号=2,优先级=1)")
    # 换单:一单两写(VIN-PD01 ↔ VIN-PD03 的序列号互换),multi_target
    r = post("OrderSwapped", None, {}, "planner-wang",
             writes={"VIN-PD01": {"序列号": 3}, "VIN-PD03": {"序列号": 1}})
    check(r["status"] == "settled", "2.9 OrderSwapped 换单(一单双目标)",
          json.dumps(r, ensure_ascii=False))
    v = view("V-SEQ-A2")
    check(row_of(v, "VIN-PD01").get("序列号") == 3
          and row_of(v, "VIN-PD03").get("序列号") == 1,
          "2.10 换单生效:VIN-PD01↔VIN-PD03 序列号互换")

    # ---- 第三幕 退回再调与 05 双拦截(计划员 vs 状态机) ----------------------
    print("\n[第三幕] 退回再调:03 被拦,回 01 调成,05 双拦截")
    for st in ("01", "02", "03"):
        r = post("PlanReleased", "VIN-PD01", {"计划状态": st}, "planner-wang")
        check(r["status"] == "settled", f"3.x 下发 {st} 结算")
    r = post("SequenceAdjusted", "VIN-PD01", {"序列号": 9}, "planner-wang")
    check(r["status"] == "rejected"
          and any("R-PLAN-ADJUST" in x for x in r["violations"]),
          "3.4 03 状态调序被 R-PLAN-ADJUST 拦(03/04 须退回再调整)",
          json.dumps(r, ensure_ascii=False))
    r = post("PlanRolledBack", "VIN-PD01", {"计划状态": "01"}, "planner-wang")
    check(r["status"] == "settled", "3.5 PlanRolledBack 退回 01")
    r = post("SequenceAdjusted", "VIN-PD01", {"序列号": 1}, "planner-wang")
    check(r["status"] == "settled", "3.6 退回 01 后调序成功")
    for st in ("02", "03", "04", "05"):
        r = post("PlanReleased", "VIN-PD01", {"计划状态": st}, "planner-wang")
        check(r["status"] == "settled", f"3.7 重发 {st} 结算")
    r = post("SequenceAdjusted", "VIN-PD01", {"序列号": 7}, "planner-wang")
    check(r["status"] == "rejected"
          and any("R-PLAN-ADJUST-WASM" in x for x in r["violations"])
          and any(v.startswith("R-PLAN-ADJUST:") or v == "R-PLAN-ADJUST"
                  for v in r["violations"]),
          "3.8 05 调序被 R-PLAN-ADJUST + R-PLAN-ADJUST-WASM 双拦截",
          json.dumps(r, ensure_ascii=False))
    check(r["status"] == "rejected" and len(r.get("violations", [])) >= 2,
          "3.9 violations 两条(同一立法,原生与沙盒各说一遍)")
    check(not any(e["type"] == "SequenceAdjusted"
                  and "7" == json.dumps(e["writes"].get("VIN-PD01", {}).get("序列号"))
                  for e in events_of("SequenceAdjusted")),
          "3.10 被拦候选零污染(事件流无序列号=7 的调序)")

    # ---- 第四幕 误报工冲正:达成率回摆(统计员 statistician) -----------------
    print("\n[第四幕] 误报工冲正")
    # 产线计划基线:计划产量写在产线本体(同 demo 惯例;CompositionDeclared
    # 无 计划产量 写授权——该键 writers 只含 PlanReleased/PlanRolledBack/
    # OrderReceived/OrderReReceived,用 OrderReceived 落基线)。
    r = post("OrderReceived", "总装线",
             {"车型": "混流产线", "计划产量": 100}, "planner-wang")
    check(r["status"] == "settled", "4.1 产线计划基线:计划产量=100")
    r = post("CompositionDeclared", "总装车间",
             {"aggregates": [{"key": "实际产量", "from": "总装线"},
                             {"key": "计划产量", "from": "总装线"}]},
             "statistician")
    check(r["status"] == "settled", "4.2 组成声明:车间聚合产线(多属)")
    r = post("CountUpdated", "总装线", {"首次合格数": 2}, "statistician")
    check(r["status"] == "settled", "4.3 计数:首次合格数=2(FTT 分子)")
    row = row_of(view("V-EFF-A6"), "总装线")
    check(row is not None and row.get("计划产量") == 100,
          "4.4 V-EFF-A6 产线行:计划产量=100")
    r = post("ProductionReported", "总装线",
             {"报工数量": 2, "实际产量": 2}, "op-zhang")
    check(r["status"] == "settled", "4.5 报工 2 件")
    row = row_of(view("V-EFF-A6"), "总装线")
    check(row is not None and abs((row.get("达成率") or 0) - 0.02) < 1e-9,
          "4.6 达成率派生=0.02(R-KPI-002:实际/计划)",
          json.dumps(row, ensure_ascii=False))
    check(abs((row.get("实际产量") or 0) - 2) < 1e-9,
          "4.7 实际产量=2")
    f16 = row_of(view("V-QUALITY-F16"), "总装线")
    check(f16 is not None and abs((f16.get("FTT") or 0) - 1.0) < 1e-9,
          "4.8 FTT 派生=1.0(R-KPI-003:首次合格数/实际产量)")
    # 误报工:实际产量多写 1
    r = post("ProductionReported", "总装线",
             {"报工数量": 1, "实际产量": 3}, "op-zhang")
    check(r["status"] == "settled", "4.9 误报工:实际产量误写 3")
    wrong_id = r["event_id"]
    row = row_of(view("V-EFF-A6"), "总装线")
    check(abs((row.get("达成率") or 0) - 0.03) < 1e-9,
          "4.10 误报即时生效:达成率=0.03")
    r = post("ReportReversed", "总装线", {"实际产量": 2}, "statistician",
             corrects=wrong_id)
    check(r["status"] == "settled", "4.11 ReportReversed 冲正(corrects 指向误报)")
    row = row_of(view("V-EFF-A6"), "总装线")
    check(abs((row.get("达成率") or 0) - 0.02) < 1e-9,
          "4.12 冲正后达成率回摆 0.02(derive 随动)")
    f16 = row_of(view("V-QUALITY-F16"), "总装线")
    check(abs((f16.get("FTT") or 0) - 1.0) < 1e-9,
          "4.13 FTT 同步回摆 1.0")
    rows = view("V-REPORT-A7").get("rows", [])
    orig = next((x for x in rows if x["event_id"] == wrong_id), None)
    fix = next((x for x in rows if x.get("corrects") == wrong_id), None)
    check(orig is not None and fix is not None and fix.get("is_correction"),
          "4.14 A7 报工流水:误报与冲正同列(L1+L2)")

    # ---- 第五幕 排序冻结(计划员) -------------------------------------------
    print("\n[第五幕] 排序冻结")
    r = post("OrderFrozen", "VIN-PD03", {"冻结状态": "已冻结"}, "planner-wang")
    check(r["status"] == "settled", "5.1 冻结 VIN-PD03")
    n_swaps = len(events_of("OrderSwapped"))
    r = post("OrderSwapped", None, {}, "planner-wang",
             writes={"VIN-PD03": {"序列号": 5}, "VIN-PD02": {"序列号": 1}})
    check(r["status"] == "rejected"
          and any("R-SEQ-FROZEN" in x for x in r["violations"]),
          "5.2 冻结后换单被 R-SEQ-FROZEN 拦截", json.dumps(r, ensure_ascii=False))
    check(len(events_of("OrderSwapped")) == n_swaps,
          "5.3 被拦换单零污染(事件流 OrderSwapped 数量不增)")
    row = row_of(view("V-SEQ-A2"), "VIN-PD03")
    check(row is not None and row.get("序列号") == 1
          and row.get("冻结状态") == "已冻结",
          "5.4 A2 行:冻结状态可见,序列号未被改写")

summary()
