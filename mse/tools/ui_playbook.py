#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# mse/tools/ui_playbook.py —— 《H5业务流模拟操作手册》的可执行版本
#
# 按手册剧本逐幕驱动真实 mse_server(与 H5 界面发同一批 HTTP 请求),
# 逐步校验手册里的预期结果。帮助函数/计数来自公共库 playbook_lib。
#
# 本剧本依赖演示背景数据(BOM@工位01、批次绑定等),不打自管理服务器:
#   1) 删库重启 mse_server(出厂状态)
#   2) mse_demo --server 127.0.0.1:18080   (灌入"昨日"背景数据)
#   3) python mse/tools/ui_playbook.py [host:port]
# 退出码 0 = 剧本全部符合预期。自管理剧本见 pb2~pb5(自建 Server、自造数据)。
# ============================================================================
import json, sys
from playbook_lib import (set_base, api, post, view, row_of, btn_of,
                          events_of, check, banner, summary)

set_base(sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1:18080")
banner("H5 业务流剧本验证")

# ---- 第一幕 早会接单与排产 ------------------------------------------------
print("\n[第一幕] 早会接单与排产")
r = post("OrderReceived", "VIN-LBV0101", {"车型": "SUV-A", "交付期": "2026-09-10"}, "planner-wang")
check(r["status"] == "settled", "1.1 接收订单 VIN-LBV0101", json.dumps(r, ensure_ascii=False))
r = post("OrderReceived", "VIN-LBV0102", {"车型": "Sedan-B", "交付期": "2026-09-10"}, "planner-wang")
check(r["status"] == "settled", "1.2 接收订单 VIN-LBV0102")
r = post("SequenceAdjusted", "VIN-LBV0101", {"序列号": 1}, "planner-wang")
check(r["status"] == "settled", "1.3 定序 VIN-LBV0101 序列号=1")
r = post("SequenceAdjusted", "VIN-LBV0102", {"序列号": 2}, "planner-wang")
check(r["status"] == "settled", "1.4 定序 VIN-LBV0102 序列号=2")

v = view("V-PLAN-A3")
row = row_of(v, "VIN-LBV0101")
b = btn_of(row, "PlanReleased")
check(b and b["options"].get("计划状态") == ["01"], "1.5 新车下发:计划状态合法选项只有 01",
      json.dumps(b, ensure_ascii=False) if b else "按钮缺失")
r = post("PlanReleased", "VIN-LBV0101", {"计划状态": "01"}, "planner-wang")
check(r["status"] == "settled", "1.5 下发 01 结算")

# 01 状态:合法选项只有 02(先验选项,再下发)
v = view("V-PLAN-A3")
b = btn_of(row_of(v, "VIN-LBV0101"), "PlanReleased")
check(b["options"].get("计划状态") == ["02"], "1.6 01 状态只能进 02",
      json.dumps(b["options"], ensure_ascii=False))
r = post("PlanReleased", "VIN-LBV0101", {"计划状态": "02"}, "planner-wang")
check(r["status"] == "settled", "1.6 下发 02 结算")
r = post("PlanReleased", "VIN-LBV0101", {"计划状态": "03"}, "planner-wang")
check(r["status"] == "settled", "1.6 下发 03 结算")
v = view("V-PLAN-A3")
b = btn_of(row_of(v, "VIN-LBV0101"), "PlanReleased")
check(sorted(b["options"].get("计划状态", [])) == ["01", "04"], "1.6 03 状态可退回01/推进04",
      json.dumps(b["options"], ensure_ascii=False))

post("PlanReleased", "VIN-LBV0102", {"计划状态": "01"}, "planner-wang")
post("PlanReleased", "VIN-LBV0102", {"计划状态": "02"}, "planner-wang")
v = view("V-SEQ-A2")
b = btn_of(row_of(v, "VIN-LBV0102"), "OrderFrozen")
check(b and b["presets"].get("冻结状态") == "已冻结", "1.7 冻结按钮预填 冻结状态=已冻结",
      json.dumps(b, ensure_ascii=False) if b else "按钮缺失")
r = post("OrderFrozen", "VIN-LBV0102", {"冻结状态": "已冻结"}, "planner-wang")
check(r["status"] == "settled", "1.7 冻结 VIN-LBV0102")
r = post("SequenceAdjusted", "VIN-LBV0102", {"序列号": 9}, "planner-wang")
check(r["status"] == "rejected" and any("R-SEQ-FROZEN" in x for x in r["violations"]),
      "1.8 冻结后调序被 R-SEQ-FROZEN 拦截", json.dumps(r, ensure_ascii=False))

# ---- 第二幕 开工与物料防错 ------------------------------------------------
print("\n[第二幕] 开工与物料防错")
v = view("V-STATION-A4", entity="工位01")
check(v.get("rows") or v.get("id") is not None, "2.1 V-STATION-A4 工位01 有内容(BOM/工艺)")

r = post("VehicleEnteredZone", "VIN-LBV0101", {"过点区域": "工位01"}, "op-zhang",
         space="整车厂/总装车间/总装线/工位01")
check(r["status"] == "settled", "2.2 车辆进工位01")

n_rej_before = len(view("V-PKE-B4").get("rejections", []))
r = post("MaterialVerified", "工位01", {"物料编号": "MAT-9999"}, "op-zhang")
check(r["status"] == "rejected" and r["layer"] == 3 and any("R-MAT-PKE" in x for x in r["violations"]),
      "2.3 扫错料被 R-MAT-PKE 拦截(L3)", json.dumps(r, ensure_ascii=False))
n_rej_after = len(view("V-PKE-B4").get("rejections", []))
check(n_rej_after > n_rej_before, "2.3 拦截卡片出现在 B4 拦截视图")
check(not any(e["type"] == "MaterialVerified" and "MAT-9999" in json.dumps(e["writes"], ensure_ascii=False)
              for e in events_of("MaterialVerified")),
      "2.3 被拦候选零污染(事件流无此事件)")
r = post("MaterialVerified", "工位01", {"物料编号": "MAT-1001"}, "op-zhang")
check(r["status"] == "settled", "2.4 扫对料通过")
r = post("VehicleEnteredZone", "VIN-LBV0101", {"过点区域": "工位02"}, "op-zhang",
         space="整车厂/总装车间/总装线/工位02")
r = post("VehicleEnteredZone", "VIN-LBV0101", {"过点区域": "工位03"}, "op-zhang",
         space="整车厂/总装车间/总装线/工位03")
check(r["status"] == "settled", "2.5 过工位02/03")

# ---- 第三幕 缺料呼叫与拉动补给 --------------------------------------------
print("\n[第三幕] 缺料呼叫与拉动补给")
r = post("MaterialCallRaised", "CALL-1001", {"呼叫状态": "呼叫中", "缺料工位": "工位03",
                                             "物料编号": "MAT-1002"}, "op-zhang")
check(r["status"] == "settled", "3.1 缺料呼叫发起")
v = view("V-CALL-B6")
check(row_of(v, "CALL-1001") is not None, "3.1 呼叫行出现在缺料看板")
v = view("V-KANBAN-B7")
act = next((a for a in v.get("actions", []) if a["type"] == "PullOrderCreated"), None)
check(act and act["presets"].get("拉动类型") == "Kanban", "3.3 B7 创建拉动单预填 拉动类型=Kanban",
      json.dumps(act, ensure_ascii=False) if act else "action 缺失")
r = post("PullOrderCreated", "PO-1001", {"拉动类型": "Kanban", "拉动状态": "已创建",
                                         "物料编号": "MAT-1002"}, "logistics-zhao")
check(r["status"] == "settled", "3.3 创建 Kanban 拉动单 PO-1001")
r = post("PullOrderShipped", "PO-1001", {"拉动状态": "已发货"}, "logistics-zhao")
check(r["status"] == "settled", "3.4 拉动单发货")
r = post("PullOrderReceived", "PO-1001", {"拉动状态": "已收货"}, "logistics-zhao")
check(r["status"] == "settled", "3.4 拉动单收货")
r = post("MaterialCallAnswered", "CALL-1001", {"呼叫状态": "已响应"}, "logistics-zhao")
check(r["status"] == "settled", "3.5 呼叫应答")
v = view("V-CALL-B6")
check(row_of(v, "CALL-1001") is None, "3.5 应答后行从看板消失(规则决定行)")
check(any(e["type"] == "MaterialCallAnswered" for e in events_of("MaterialCallAnswered")),
      "3.5 事实永存(事件流可查)")

# ---- 第四幕 设备故障与停线 ------------------------------------------------
print("\n[第四幕] 设备故障与停线")
r = post("LineStopped", "总装线", {"线状态": "停线", "停线原因": "拧紧枪故障(E-501)"}, "leader-li")
check(r["status"] == "settled", "4.1 ANDON 停线")
v = view("V-ANDON-D1")
check(row_of(v, "总装线").get("线状态") == "停线", "4.1 大屏线状态=停线")

esc_before = len(events_of("AlarmEscalated"))
for i in range(3):
    r = post("FaultAlarmed", "EQ-NG02", {"检点": "工位02-拧紧点P2", "故障码": "E-501",
                                         "故障级别": 2}, "maintenance-chen")
    check(r["status"] == "settled", f"4.2/4.3 FaultAlarmed 第{i+1}次")
esc = [e for e in events_of("AlarmEscalated") if "rule:R-ALM-003" in e.get("actor", "")]
check(len(events_of("AlarmEscalated")) > esc_before and len(esc) >= 1,
      "4.3 同检点第 3 次故障自动触发 AlarmEscalated(科长,rule:R-ALM-003)")
v = view("V-ALARM-F15")
check(any(row.get("type") == "AlarmEscalated" for row in v.get("rows", [])),
      "4.4 F15 报警列表可见升级记录")
r = post("FaultCleared", "EQ-NG02", {"设备状态": "运行"}, "maintenance-chen")
check(r["status"] == "settled", "4.5 故障消除")
r = post("LineResumed", "总装线", {"线状态": "运行"}, "leader-li")
check(r["status"] == "settled" and row_of(view("V-ANDON-D1"), "总装线").get("线状态") == "运行",
      "4.5 复线,大屏恢复运行")

# ---- 第五幕 质检、锁车与返修 ----------------------------------------------
print("\n[第五幕] 质检、锁车与返修")
r = post("DefectRegistered", "VIN-LBV0101", {"车漆": "划痕", "缺陷级别": 2}, "qc-liu",
         space={"raw": "左前门"}, evidence="img:photo-5501.jpg")
check(r["status"] == "settled", "5.1 登记缺陷(级别2)")
locks = [e for e in events_of("VehicleLocked")
         if "VIN-LBV0101" in json.dumps(e["writes"], ensure_ascii=False)]
check(len(locks) >= 1 and locks[-1]["actor"].startswith("rule:R-QUAL-TRIGGER"),
      "5.1 触发链:自动锁车(rule:R-QUAL-TRIGGER)")
check(row_of(view("V-LOCK-F13"), "VIN-LBV0101") is not None, "5.2 锁定列表可见")
r = post("ProductionReported", "VIN-LBV0101", {"报工数量": 1}, "op-zhang")
check(r["status"] == "rejected" and any("R-QUAL-LOCK" in x for x in r["violations"]),
      "5.3 锁定车报工被 R-QUAL-LOCK 拦截", json.dumps(r, ensure_ascii=False))

r = post("ReworkRequested", "VIN-LBV0101", {}, "qc-liu")
check(r["status"] == "settled", "5.4 发起返修")
r = post("ReworkRecorded", "VIN-LBV0101", {"返修内容": "左前门点漆修复"}, "reworker-wang")
check(r["status"] == "settled", "5.4 记录返修内容")
r = post("ReworkSubmitted", "VIN-LBV0101", {}, "reworker-wang")
check(r["status"] == "settled", "5.4 递交复检")
rw_before = len(events_of("ReworkRequested"))
r = post("RecheckJudged", "VIN-LBV0101", {"复检结论": "NOK"}, "rechecker-zhao")
check(r["status"] == "settled", "5.5 复检 NOK")
check(len(events_of("ReworkRequested")) > rw_before, "5.5 NOK 自动再开返修(rule:R-REWORK-TRIGGER)")
nok_event_id = r["event_id"]
r = post("ReworkRecorded", "VIN-LBV0101", {"返修内容": "左前门整面重喷"}, "reworker-wang")
post("ReworkSubmitted", "VIN-LBV0101", {}, "reworker-wang")
r = post("RecheckJudged", "VIN-LBV0101", {"复检结论": "OK"}, "rechecker-zhao")
check(r["status"] == "settled", "5.6 二次复检 OK")
r = post("ReworkClosed", "VIN-LBV0101", {}, "rechecker-zhao")
check(r["status"] == "settled", "5.6 返修关闭")
r = post("JudgementOverruled", "VIN-LBV0101", {"复检结论": "OK"}, "qc-chief",
         corrects=nok_event_id)
check(r["status"] == "settled" and r.get("event_id"), "5.7 误判改判(corrects 指向原 NOK)")
v = view("V-REWORK-001", observer="复检员")
rows = v.get("rows", [])
orig = next((x for x in rows if x["event_id"] == nok_event_id), None)
fix = next((x for x in rows if x.get("corrects") == nok_event_id), None)
check(orig is not None and fix is not None and fix.get("is_correction"),
      "5.7 原判定与改判同列可见(L1+L2)")
r = post("VehicleUnlocked", "VIN-LBV0101", {"锁定状态": "未锁"}, "qc-liu")
check(r["status"] == "settled" and row_of(view("V-LOCK-F13"), "VIN-LBV0101") is None,
      "5.8 解锁,锁定列表移除(锁定史永存)")

# ---- 第六幕 报工、看板与日结 ----------------------------------------------
print("\n[第六幕] 报工、看板与日结")
r = post("ProductionReported", "VIN-LBV0101", {"报工数量": 1}, "op-zhang",
         space="整车厂/总装车间/终检工位")
check(r["status"] == "settled", "6.1 解锁后报工结算")
v = view("V-HIST-F9", entity="VIN-LBV0101")
check(len(v.get("rows", [])) >= 3, "6.2 位置历史(纵切)有全天过点")
v = view("V-TRACE-F14", entity="VIN-LBV0101")
check(len(v.get("nodes", [])) >= 1 and v.get("root") == "VIN-LBV0101",
      "6.3 车辆追溯遍历图(根=VIN)")
v = view("V-EFF-A6")
line = row_of(v, "总装线")
check(line is not None and line.get("计划产量") is not None,
      "6.4 产线看板:计划/实际/达成率就位")
v42 = view("V-AVI-C1", t="42")
vnow = view("V-AVI-C1")
check(len(v42.get("rows", [])) < len(vnow.get("rows", [])),
      "6.5 AS OF t=42 回到过去(行数少于当前)")

# ---- 第七幕 日结 ----------------------------------------------------------
print("\n[第七幕] 日结")
r1 = post("MaterialCallRaised", "CALL-2001", {"呼叫状态": "呼叫中", "缺料工位": "工位02",
                                              "物料编号": "MAT-1003"}, "op-zhang",
          idempotency_key="PLAYBOOK-IDEMP-1")
r2 = post("MaterialCallRaised", "CALL-2001", {"呼叫状态": "呼叫中", "缺料工位": "工位02",
                                              "物料编号": "MAT-1003"}, "op-zhang",
          idempotency_key="PLAYBOOK-IDEMP-1")
check(r1.get("event_id") == r2.get("event_id") and r2["status"] == "settled",
      "7.x 幂等:重复提交返回原 event_id,日志不增")

summary()
