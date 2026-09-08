#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# mse/tools/pb3_quality_compliance.py —— 剧本3:质量与法规追溯
#
# 剧情:法规审核周。VIN-QC01 上线,检验计划与任务先行;检测线设备上传
# 尾气/大灯/淋雨数据,其中一条"淋雨不合格"是设备误传,修正留痕;一条误录
# 缺陷被作废(缺陷累计口径不回减);两个法规件批次绑定上车,正向追踪与
# 反向追溯双向可走;严重缺陷触发自动锁车,缺陷车辆不出厂。
#
# 自管理:自建 Server(临时库 + 随机端口),全部数据本剧本自造。
# 用法:python mse/tools/pb3_quality_compliance.py   退出码 0 = 全部符合预期。
# ============================================================================
import json
from playbook_lib import (Server, set_base, post, view, row_of, events_of,
                          check, banner, summary, node_ids, has_edge)

with Server() as s:
    set_base(s.base)
    banner("剧本3:质量与法规追溯")

    # ---- 第一幕 建档:订单、过点与检验计划(质检员 qc-liu) ------------------
    print("\n[第一幕] 建档:订单、过点、检验计划与任务")
    r = post("OrderReceived", "VIN-QC01",
             {"车型": "Sedan-B", "订单状态": "接收", "交付期": "2026-09-25"},
             "planner-wang")
    check(r["status"] == "settled", "1.1 订单 VIN-QC01 接收")
    for z in ("工位01", "工位02", "工位03"):
        r = post("VehicleEnteredZone", "VIN-QC01", {"过点区域": z}, "op-zhang",
                 space=f"整车厂/总装车间/总装线/{z}")
        check(r["status"] == "settled", f"1.2 过点 {z}")
    r = post("InspectionPlanIssued", "PLAN-QC01", {"检验计划号": "PLAN-QC01"},
             "qc-liu")
    check(r["status"] == "settled", "1.3 检验计划下达")
    row = row_of(view("V-PLAN-F2"), "PLAN-QC01")
    check(row is not None and row.get("检验计划号") == "PLAN-QC01",
          "1.4 V-PLAN-F2 检验计划行可见")
    r = post("InspectionTaskAssigned", "TASK-QC01",
             {"检验工位": "工位01", "检验计划号": "PLAN-QC01"}, "qc-liu")
    check(r["status"] == "settled", "1.5 检验任务指派到工位01")
    row = row_of(view("V-TASK-F4"), "TASK-QC01")
    check(row is not None and row.get("检验工位") == "工位01"
          and row.get("检验计划号") == "PLAN-QC01",
          "1.6 V-TASK-F4 任务行:工位(ref)+计划号同屏")

    # ---- 第二幕 检测数据与设备误传修正(检测线设备 → 质检员) ----------------
    print("\n[第二幕] 检测数据流水与设备误传修正")
    r = post("InspectionDataRecorded", "VIN-QC01",
             {"尾气检测值": 0.42, "大灯检测值": 15800.0, "淋雨结论": "合格"},
             "detect-line-gw", evidence="dev:FT-LINE-07")
    check(r["status"] == "settled", "2.1 检测数据上传(尾气/大灯/淋雨)")
    v = view("V-QUALREC-F12", entity="VIN-QC01")
    check(len(v.get("rows", [])) == 1
          and v["rows"][0]["writes"]["VIN-QC01"].get("淋雨结论") == "合格",
          "2.2 V-QUALREC-F12 流水首行在列")
    # 设备误传:又报一条"淋雨不合格"
    r = post("InspectionDataRecorded", "VIN-QC01", {"淋雨结论": "不合格"},
             "detect-line-gw", evidence="dev:FT-LINE-07")
    check(r["status"] == "settled", "2.3 设备误传:淋雨结论=不合格")
    wrong_id = r["event_id"]
    row = row_of(view("V-CAR-F7", entity="VIN-QC01"), "VIN-QC01")
    check(row is not None and row.get("淋雨结论") == "不合格",
          "2.4 误传即时生效(终态断面取到不合格)")
    r = post("InspectionDataCorrected", "VIN-QC01", {"淋雨结论": "合格"},
             "qc-liu", corrects=wrong_id)
    check(r["status"] == "settled" and r.get("event_id"),
          "2.5 InspectionDataCorrected 修正(corrects 指向误传事件)")
    rows = view("V-QUALREC-F12", entity="VIN-QC01").get("rows", [])
    orig = next((x for x in rows if x["event_id"] == wrong_id), None)
    fix = next((x for x in rows if x.get("corrects") == wrong_id), None)
    check(orig is not None and fix is not None and fix.get("is_correction"),
          "2.6 流水 L1+L2:误传与修正同列可见")
    row = row_of(view("V-CAR-F7", entity="VIN-QC01"), "VIN-QC01")
    check(row is not None and row.get("淋雨结论") == "合格"
          and row.get("尾气检测值") == 0.42,
          "2.7 终态取修正后值(合格),其余检测值不受波及")

    # ---- 第三幕 误录缺陷作废与累计口径(质检员) ----------------------------
    print("\n[第三幕] 误录缺陷:作废不回减")
    r = post("DefectRegistered", "VIN-QC01", {"车漆": "凹陷", "缺陷级别": 1},
             "qc-liu", space={"raw": "右后门"})
    check(r["status"] == "settled", "3.1 误录缺陷登记(凹陷,级别1)")
    row = row_of(view("V-QUALITY-F16"), "VIN-QC01")
    check(row is not None and row.get("缺陷总数") == 1,
          "3.2 缺陷总数派生 +1(R-QUAL-COUNT)")
    r = post("DefectCancelled", "VIN-QC01", {"车漆": "完好"}, "qc-chief",
             corrects=r["event_id"])
    check(r["status"] == "settled", "3.3 DefectCancelled 作废误录")
    row = row_of(view("V-QUALITY-F16"), "VIN-QC01")
    check(row is not None and row.get("缺陷总数") == 1,
          "3.4 缺陷总数口径=登记累计,作废不回减(derive 无 on_types 挂钩"
          " DefectCancelled;口径说明:审计要的是'登记过几笔',不是'现存几条')")
    # 真实缺陷:级别 ≥2 触发自动锁车
    r = post("DefectRegistered", "VIN-QC01", {"车漆": "划痕", "缺陷级别": 2},
             "qc-liu", evidence="img:qc-8801.jpg")
    check(r["status"] == "settled", "3.5 真实缺陷登记(划痕,级别2)")
    locks = [e for e in events_of("VehicleLocked")
             if "VIN-QC01" in json.dumps(e["writes"], ensure_ascii=False)]
    check(len(locks) >= 1 and locks[-1]["actor"].startswith("rule:R-QUAL-TRIGGER"),
          "3.6 触发链:级别≥2 自动锁车(rule:R-QUAL-TRIGGER)")
    check(row_of(view("V-LOCK-F13"), "VIN-QC01") is not None,
          "3.7 V-LOCK-F13 锁定列表:缺陷车辆不出厂")
    row = row_of(view("V-QUALITY-F16"), "VIN-QC01")
    check(row is not None and row.get("缺陷总数") == 2,
          "3.8 质量报表缺陷总数=2(误录 1 + 真实 1,累计口径)")

    # ---- 第四幕 法规追溯:两个法规件批次上车(物流员→法规员) ----------------
    print("\n[第四幕] 法规追溯:双向可走")
    r = post("MaterialRegistered", "BATCH-QC-ESP",
             {"物料编号": "MAT-ESP", "批次号": "BATCH-QC-ESP",
              "供应商": "SUP-BOSCH"}, "warehouse-wang")
    check(r["status"] == "settled", "4.1 法规件批次 ESP 登记(供应商 SUP-BOSCH)")
    r = post("MaterialRegistered", "BATCH-QC-AIRBAG",
             {"物料编号": "MAT-AIRBAG", "批次号": "BATCH-QC-AIRBAG",
              "供应商": "SUP-AUTOLIV"}, "warehouse-wang")
    check(r["status"] == "settled", "4.2 法规件批次 AIRBAG 登记(SUP-AUTOLIV)")
    for batch in ("BATCH-QC-ESP", "BATCH-QC-AIRBAG"):
        r = post("BatchBoundToVIN", None, {}, "logistics-zhao",
                 writes={"VIN-QC01": {"批次绑定": batch},
                         batch: {"VIN绑定": "VIN-QC01"}})
        check(r["status"] == "settled", f"4.3 批次 {batch} 绑定 VIN-QC01")
    v = view("V-TRACE-F14", entity="VIN-QC01")
    ids = node_ids(v)
    check({"BATCH-QC-ESP", "BATCH-QC-AIRBAG", "SUP-BOSCH", "SUP-AUTOLIV"} <= ids,
          "4.4 V-TRACE-F14 节点含两个法规件批次与两家供应商",
          json.dumps(sorted(ids), ensure_ascii=False))
    check(has_edge(v, "VIN-QC01", "批次绑定", "BATCH-QC-AIRBAG")
          and has_edge(v, "BATCH-QC-ESP", "VIN绑定", "VIN-QC01")
          and has_edge(v, "BATCH-QC-AIRBAG", "VIN绑定", "VIN-QC01"),
          "4.5 边语义:VIN 侧 批次绑定 单值后写覆盖;批次侧 VIN绑定 各自留存")
    # 正向追踪:法规件出问题时,从批次找装车 VIN
    v = view("V-TRACE-F14", entity="BATCH-QC-AIRBAG")
    check("VIN-QC01" in node_ids(v),
          "4.6 正向追踪:entity=批次 → 命中 VIN(召回定位)")
    check("SUP-AUTOLIV" in node_ids(v),
          "4.7 批次节点同样带出供应商")

    # ---- 第五幕 滞留定位与质量报表(班组长) --------------------------------
    print("\n[第五幕] 滞留定位与质量报表")
    v = view("V-HIST-F9", entity="VIN-QC01")
    zones = [r["writes"]["VIN-QC01"].get("过点区域") for r in v.get("rows", [])]
    check(zones == ["工位01", "工位02", "工位03"],
          "5.1 V-HIST-F9 位置历史:工位01→02→03 全程留痕",
          json.dumps(zones, ensure_ascii=False))
    v = view("V-AVI-C1")
    row = row_of(v, "VIN-QC01")
    check(row is not None and row.get("过点区域") == "工位03",
          "5.2 V-AVI-C1 实时位置=工位03(滞留定位断面)")
    row = row_of(view("V-QUALITY-F16"), "VIN-QC01")
    check(row is not None and (row.get("缺陷总数") or 0) >= 1,
          "5.3 V-QUALITY-F16 质量报表行:缺陷总数≥1")
    r = post("ProductionReported", "VIN-QC01", {"报工数量": 1}, "op-zhang")
    check(r["status"] == "rejected" and any("R-QUAL-LOCK" in x for x in r["violations"]),
          "5.4 锁定车报工被 R-QUAL-LOCK 拦截(缺陷车辆不出厂)",
          json.dumps(r, ensure_ascii=False))

summary()
