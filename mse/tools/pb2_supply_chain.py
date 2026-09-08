#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# mse/tools/pb2_supply_chain.py —— 剧本2:供应链与批次追溯
#
# 剧情:清晨到货,库管登记三只物料、下达工位 BOM;中央库与线边库分仓记账;
# 四类拉动单(Andon/Kanban/紧急/JIS)各归各看板;批次绑定上车,一次错绑、
# 作废、重绑,全程留痕;最后从批次反向追溯回车辆与供应商。
#
# 自管理:自建 Server(临时库 + 随机端口),全部数据本剧本自造。
# 用法:python mse/tools/pb2_supply_chain.py   退出码 0 = 全部符合预期。
# ============================================================================
import json
from playbook_lib import (Server, set_base, post, view, row_of, btn_of,
                          action_of, events_of, check, banner, summary,
                          node_ids, has_edge)

with Server() as s:
    set_base(s.base)
    banner("剧本2:供应链与批次追溯")

    # ---- 第一幕 清晨到货:物料主数据与工位 BOM(库管 warehouse-wang) --------
    print("\n[第一幕] 物料主数据与工位 BOM")
    mats = [("MAT-SC01", "BATCH-INIT-01", 500, "中央库-A01"),
            ("MAT-SC02", "BATCH-INIT-02", 300, "中央库-A02"),
            ("MAT-SC03", "BATCH-INIT-03", 120, "中央库-B01")]
    for code, batch, qty, loc in mats:
        r = post("MaterialRegistered", code,
                 {"物料编号": code, "批次号": batch, "库存数量": qty, "库位": loc},
                 "warehouse-wang")
        check(r["status"] == "settled", f"1.x 物料主数据登记 {code}",
              json.dumps(r, ensure_ascii=False))
    v = view("V-MAT-B1")
    row = row_of(v, "MAT-SC01")
    check(row is not None and row.get("批次号") == "BATCH-INIT-01"
          and row.get("库存数量") == 500 and row.get("库位") == "中央库-A01",
          "1.4 V-MAT-B1 列表行:编号/批次/库存/库位同屏",
          json.dumps(row, ensure_ascii=False))
    check(all(row_of(v, m[0]) is not None for m in mats),
          "1.5 V-MAT-B1 三只物料全部在列")

    bom = ["MAT-SC01", "MAT-SC02", "MAT-SC03"]
    r = post("BomReceived", "工位01",
             {"BOM清单": bom, "物料需求": ["MAT-SC01×2", "MAT-SC02×1", "MAT-SC03×4"],
              "工艺信息": "先装 MAT-SC02,力矩 25N·m"}, "planner-wang")
    check(r["status"] == "settled", "1.6 BOM 下达到工位01")
    v = view("V-BOM-B2", entity="工位01")
    row = row_of(v, "工位01")
    check(row is not None and row.get("BOM清单") == bom,
          "1.7 V-BOM-B2(entity=工位01) BOM 清单三件齐全",
          json.dumps(row, ensure_ascii=False))
    check(row is not None and row.get("物料需求") and row.get("工艺信息"),
          "1.8 V-BOM-B2 物料需求与工艺信息同屏")
    v = view("V-STATION-A4", entity="工位01")
    row = row_of(v, "工位01")
    check(row is not None and row.get("工艺信息") == "先装 MAT-SC02,力矩 25N·m",
          "1.9 V-STATION-A4 工位指导同步可见(同一事实两个切面)")

    # ---- 第二幕 分仓记账:中央库与线边库(库管) ----------------------------
    print("\n[第二幕] 分仓库存")
    r = post("StockUpdated", "STK-CENTRAL-MAT-SC01",
             {"物料编号": "MAT-SC01", "批次号": "BATCH-INIT-01",
              "库存数量": 480, "库位": "中央库-A01"}, "warehouse-wang")
    check(r["status"] == "settled", "2.1 中央库记账(无时空戳)")
    r = post("StockUpdated", "STK-LINESIDE-MAT-SC01",
             {"物料编号": "MAT-SC01", "批次号": "BATCH-INIT-01",
              "库存数量": 20, "库位": "线边库-L01"}, "warehouse-wang",
             space="整车厂/总装车间/线边库")
    check(r["status"] == "settled", "2.2 线边库记账(挂线边库锚点)")
    v = view("V-STOCK-B3")
    rc = row_of(v, "STK-CENTRAL-MAT-SC01")
    rl = row_of(v, "STK-LINESIDE-MAT-SC01")
    check(rc is not None and rc.get("库存数量") == 480
          and rl is not None and rl.get("库存数量") == 20,
          "2.3 V-STOCK-B3 全仓视图两行可见")
    v = view("V-LINESIDE-B11")
    check(row_of(v, "STK-LINESIDE-MAT-SC01") is not None
          and row_of(v, "STK-CENTRAL-MAT-SC01") is None,
          "2.4 V-LINESIDE-B11 时空切片:只见线边库行")
    check(row_of(v, "STK-LINESIDE-MAT-SC01").get("库位") == "线边库-L01",
          "2.5 线边库行库位正确")

    # ---- 第三幕 四类拉动:各归各看板(物流员 logistics-zhao) ----------------
    print("\n[第三幕] 四类拉动单")
    for vid, ptype in [("V-KANBAN-B7", "Kanban"), ("V-PULL-B8", "紧急"),
                       ("V-JIS-B9", "JIS"), ("V-JIT-B10", "JIT")]:
        act = action_of(view(vid), "PullOrderCreated")
        check(act and act["presets"].get("拉动类型") == ptype,
              f"3.x {vid} 发起拉动单预填 拉动类型={ptype}",
              json.dumps(act, ensure_ascii=False) if act else "action 缺失")
    orders = [("PO-ANDON-1", "Andon"), ("PO-KANBAN-1", "Kanban"),
              ("PO-URGENT-1", "紧急"), ("PO-JIS-1", "JIS")]
    for pid, ptype in orders:
        r = post("PullOrderCreated", pid,
                 {"拉动类型": ptype, "拉动状态": "已创建", "物料编号": "MAT-SC02"},
                 "logistics-zhao")
        check(r["status"] == "settled", f"3.x 创建 {ptype} 拉动单 {pid}",
              json.dumps(r, ensure_ascii=False))
    v7, v8, v9, v10 = (view(x) for x in
                       ("V-KANBAN-B7", "V-PULL-B8", "V-JIS-B9", "V-JIT-B10"))
    check([r["id"] for r in v7.get("rows", [])] == ["PO-KANBAN-1"],
          "3.7 B7 规则决定行:只见 Kanban 单")
    check([r["id"] for r in v8.get("rows", [])] == ["PO-URGENT-1"],
          "3.8 B8 只见紧急单")
    check([r["id"] for r in v9.get("rows", [])] == ["PO-JIS-1"],
          "3.9 B9 只见 JIS 单")
    check(all(row_of(v, "PO-ANDON-1") is None for v in (v7, v8, v9, v10)),
          "3.10 Andon 单在 B7/B8/B9/B10 均不可见(四类视图各管各)")
    # Kanban 单全生命周期:已创建 → 已发货 → 已收货
    r = post("PullOrderShipped", "PO-KANBAN-1", {"拉动状态": "已发货"},
             "logistics-zhao")
    check(r["status"] == "settled", "3.11 Kanban 单发货")
    r = post("PullOrderReceived", "PO-KANBAN-1", {"拉动状态": "已收货"},
             "logistics-zhao")
    check(r["status"] == "settled", "3.12 Kanban 单收货")
    row = row_of(view("V-KANBAN-B7"), "PO-KANBAN-1")
    check(row is not None and row.get("拉动状态") == "已收货",
          "3.13 B7 行终态=已收货(生命周期走完)")
    # 误建取消:Andon 单直接作废(拉动状态预填 已取消)
    row = row_of(view("V-KANBAN-B7"), "PO-KANBAN-1")
    b = btn_of(row, "PullOrderCancelled")
    check(b and b["presets"].get("拉动状态") == "已取消",
          "3.14 取消按钮预填 拉动状态=已取消")
    r = post("PullOrderCancelled", "PO-ANDON-1", {"拉动状态": "已取消"},
             "logistics-zhao")
    check(r["status"] == "settled", "3.15 Andon 单取消")
    check(any("PO-ANDON-1" in json.dumps(e["writes"], ensure_ascii=False)
              for e in events_of("PullOrderCancelled")),
          "3.16 取消事实永存(事件流可查)")

    # ---- 第四幕 批次绑定与正向追溯(物流员) --------------------------------
    print("\n[第四幕] 批次绑定与正向追溯")
    r = post("MaterialRegistered", "BATCH-S1",
             {"物料编号": "MAT-SC01", "批次号": "BATCH-S1", "供应商": "SUP-BOSCH"},
             "warehouse-wang")
    check(r["status"] == "settled", "4.1 批次 BATCH-S1 登记(供应商 SUP-BOSCH)")
    r = post("MaterialRegistered", "BATCH-S2",
             {"物料编号": "MAT-SC01", "批次号": "BATCH-S2",
              "供应商": "SUP-CONTINENTAL"}, "warehouse-wang")
    check(r["status"] == "settled", "4.2 批次 BATCH-S2 登记(供应商 SUP-CONTINENTAL)")
    r = post("OrderReceived", "VIN-SC01",
             {"车型": "SUV-A", "订单状态": "接收", "交付期": "2026-09-20"},
             "planner-wang")
    check(r["status"] == "settled", "4.3 车辆 VIN-SC01 订单接收")
    # 一次发生、双向写:VIN 记批次,批次记 VIN(关系即属性)
    r = post("BatchBoundToVIN", None, {}, "logistics-zhao",
             writes={"VIN-SC01": {"批次绑定": "BATCH-S1"},
                     "BATCH-S1": {"VIN绑定": "VIN-SC01"}})
    check(r["status"] == "settled", "4.4 BatchBoundToVIN 双向写一次结算",
          json.dumps(r, ensure_ascii=False))
    v = view("V-TRACE-B5", entity="VIN-SC01")
    check(v.get("root") == "VIN-SC01", "4.5 遍历根=VIN")
    ids = node_ids(v)
    check({"BATCH-S1", "SUP-BOSCH"} <= ids,
          "4.6 VIN→批次→供应商 三段链节点齐全",
          json.dumps(sorted(ids), ensure_ascii=False))
    check(has_edge(v, "VIN-SC01", "批次绑定", "BATCH-S1"),
          "4.7 遍历边:VIN-SC01 -批次绑定→ BATCH-S1")
    check(has_edge(v, "BATCH-S1", "供应商", "SUP-BOSCH"),
          "4.8 遍历边:BATCH-S1 -供应商→ SUP-BOSCH")

    # ---- 第五幕 错绑、作废与重绑:三段永存(物流员→质量员) ------------------
    print("\n[第五幕] 错绑演示:作废与重绑")
    r = post("BatchBoundToVIN", None, {}, "logistics-zhao",
             writes={"VIN-SC01": {"批次绑定": "BATCH-S2"},
                     "BATCH-S2": {"VIN绑定": "VIN-SC01"}})
    check(r["status"] == "settled", "5.1 错绑:BATCH-S2 绑上 VIN-SC01")
    wrong_id = r["event_id"]
    check("BATCH-S2" in node_ids(view("V-TRACE-B5", entity="VIN-SC01")),
          "5.2 错绑即时生效(遍历图出现 BATCH-S2)")
    r = post("BindingVoided", None, {}, "qc-liu", corrects=wrong_id,
             writes={"VIN-SC01": {"批次绑定": ""},
                     "BATCH-S2": {"VIN绑定": ""}})
    check(r["status"] == "settled" and r.get("event_id"),
          "5.3 BindingVoided 作废(corrects 指向错绑事件)",
          json.dumps(r, ensure_ascii=False))
    ids = node_ids(view("V-TRACE-B5", entity="VIN-SC01"))
    check("BATCH-S2" not in ids,
          "5.4 作废后终态:BATCH-S2 从遍历图消失(空串=没有关系)")
    r = post("BatchBoundToVIN", None, {}, "logistics-zhao",
             writes={"VIN-SC01": {"批次绑定": "BATCH-S1"},
                     "BATCH-S1": {"VIN绑定": "VIN-SC01"}})
    check(r["status"] == "settled", "5.5 重绑 BATCH-S1")
    ids = node_ids(view("V-TRACE-B5", entity="VIN-SC01"))
    check({"BATCH-S1", "SUP-BOSCH"} <= ids
          and "BATCH-S2" not in ids and "SUP-CONTINENTAL" not in ids,
          "5.6 重绑后遍历图正确(只认 BATCH-S1 一脉)",
          json.dumps(sorted(ids), ensure_ascii=False))
    voids = events_of("BindingVoided")
    binds = events_of("BatchBoundToVIN")
    check(len(binds) >= 3 and len(voids) >= 1
          and voids[0].get("corrects") == wrong_id,
          "5.7 事件流三段永存:错绑/作废/重绑,作废带 corrects 因果链")

    # ---- 第六幕 反向追溯:从批次走回车辆(法规员 auditor-fang) --------------
    print("\n[第六幕] 反向追溯")
    v = view("V-TRACE-B5", entity="BATCH-S1")
    ids = node_ids(v)
    check(v.get("root") == "BATCH-S1" and "VIN-SC01" in ids,
          "6.1 反向:entity=批次 → 命中 VIN 节点(ref 双向游走)")
    check(has_edge(v, "BATCH-S1", "VIN绑定", "VIN-SC01"),
          "6.2 反向遍历边:BATCH-S1 -VIN绑定→ VIN-SC01")
    check("SUP-BOSCH" in ids, "6.3 反向同样可达供应商节点")

summary()
