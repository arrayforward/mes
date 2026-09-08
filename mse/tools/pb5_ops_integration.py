#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# mse/tools/pb5_ops_integration.py —— 剧本5:运维、集成与不停机演化
#
# 剧情:运维值班夜。ERP 报文进出集成接口,一条误报文作废留痕;两台设备
# 上台账,一台误报状态被修正;PLC 网关经适配层上报工位占用(信任分级 +
# 异步结算,drain 才落账);车间 OEE 三率一填即派生;午夜模拟断电——
# 停库重启后事件/视图/定义逐比特一致,幂等键跨重启不重复入账。
#
# 说明(按任务口径,这两块不在 HTTP 覆盖面内,由进程内 SDK 测试兜底):
#   - 定义热更新(注册即生效):定义入口是进程内 SDK
#     (DefinitionLayer::settle_definition),HTTP 未暴露定义端点;
#     覆盖见 mse_tests(--seed 强制重放路径同语义)。
#   - 定距快照:mse_server 默认快照间隔 0(纯日志重放),快照机制
#     覆盖见 mse_tests / mse_demo(SnapshotStore,每 20 条一份)。
#
# 自管理:自建 Server(临时库 + 随机端口),全部数据本剧本自造。
# 用法:python mse/tools/pb5_ops_integration.py   退出码 0 = 全部符合预期。
# ============================================================================
import json
from playbook_lib import (Server, set_base, post, view, row_of, events_of,
                          events_all, drain, api, view_text, check, banner,
                          summary)

with Server() as s:
    set_base(s.base)
    banner("剧本5:运维、集成与不停机演化")

    # ---- 第一幕 集成接口:ERP 报文与误报文作废(集成运维 integrator-xu) -----
    print("\n[第一幕] 集成接口监控")
    r = post("InboundMessageRecorded", "MSG-ERP-001",
             {"来源系统": "ERP", "报文类型": "生产计划", "外部单号": "PO-EXT-9001"},
             "erp-adapter", evidence="idoc:0000009001")
    check(r["status"] == "settled", "1.1 ERP 生产计划报文入账(外部单号+evidence)")
    rows = view("V-INTEG-H3").get("rows", [])
    row = next((x for x in rows if "MSG-ERP-001" in x.get("writes", {})), None)
    check(row is not None
          and row["writes"]["MSG-ERP-001"].get("外部单号") == "PO-EXT-9001",
          "1.2 V-INTEG-H3 流水:报文行在列,外部单号可对账")
    ev = events_of("InboundMessageRecorded")
    check(ev and ev[0].get("evidence") == "idoc:0000009001",
          "1.3 报文 evidence(IDOC 号)随事件永存")
    r = post("InboundMessageRecorded", "MSG-ERP-002",
             {"来源系统": "ERP", "报文类型": "生产计划", "外部单号": "PO-EXT-9002"},
             "erp-adapter")
    check(r["status"] == "settled", "1.4 误报文入账(重复推送的计划)")
    wrong_id = r["event_id"]
    r = post("InboundMessageVoided", "MSG-ERP-002", {}, "integrator-xu",
             corrects=wrong_id)
    check(r["status"] == "settled" and r.get("event_id"),
          "1.5 InboundMessageVoided 作废误报文(corrects 指向原报文)",
          json.dumps(r, ensure_ascii=False))
    rows = view("V-INTEG-H3").get("rows", [])
    orig = next((x for x in rows if x["event_id"] == wrong_id), None)
    fix = next((x for x in rows if x.get("corrects") == wrong_id), None)
    check(orig is not None and fix is not None and fix.get("is_correction"),
          "1.6 H3 流水 L1+L2:误报文与作废同列可见")

    # ---- 第二幕 设备台账与误报修正(设备运维 maintenance-chen) --------------
    print("\n[第二幕] 设备台账")
    r = post("EquipmentStatusReported", "EQ-PB5-01",
             {"设备编号": "EQ-PB5-01", "设备状态": "运行", "刀具寿命": 80},
             "maintenance-chen")
    check(r["status"] == "settled", "2.1 设备 EQ-PB5-01 上台账(运行)")
    r = post("EquipmentStatusReported", "EQ-PB5-02",
             {"设备编号": "EQ-PB5-02", "设备状态": "运行"}, "maintenance-chen")
    check(r["status"] == "settled", "2.2 设备 EQ-PB5-02 上台账(运行)")
    v = view("V-EQ-G1")
    check(row_of(v, "EQ-PB5-01") is not None
          and row_of(v, "EQ-PB5-02") is not None,
          "2.3 V-EQ-G1 台账列表两台在列")
    check(row_of(v, "EQ-PB5-01").get("刀具寿命") == 80,
          "2.4 台账行:设备编号/状态/刀具寿命同屏")
    r0 = post("EquipmentStatusReported", "EQ-PB5-01",
              {"设备编号": "EQ-PB5-01", "设备状态": "故障"}, "maintenance-chen")
    check(r0["status"] == "settled", "2.5 误报:EQ-PB5-01 状态写成故障")
    r = post("EquipmentStatusCorrected", "EQ-PB5-01",
             {"设备状态": "保养"}, "maintenance-chen", corrects=r0["event_id"])
    check(r["status"] == "settled", "2.6 EquipmentStatusCorrected 修正为保养")
    row = row_of(view("V-EQ-G2", entity="EQ-PB5-01"), "EQ-PB5-01")
    check(row is not None and row.get("设备状态") == "保养",
          "2.7 V-EQ-G2(entity)详情:终态取修正后值=保养")
    check(row_of(view("V-EQ-G1"), "EQ-PB5-01").get("设备状态") == "保养",
          "2.8 G1 台账终态同步=保养")
    check(row_of(view("V-EQ-G2", entity="EQ-PB5-02"), "EQ-PB5-02")
          .get("设备状态") == "运行",
          "2.9 EQ-PB5-02 不受波及(entity 纵切隔离)")

    # ---- 第三幕 PLC 适配层:信任分级 + 异步结算(plc-gw) --------------------
    print("\n[第三幕] PLC 适配层:迁移沿上报")
    r = post("PlcEdgeReported", "工位01", {"工位占用": "占用"}, "plc-gw",
             token=None)
    check(r["status"] == "rejected" and r.get("layer") == 2
          and any("信任级不足" in x for x in r["violations"]),
          "3.1 无凭证提交被 L2 拦(min_trust=1:仅网关级凭证)",
          json.dumps(r, ensure_ascii=False))
    r = post("PlcEdgeReported", "工位01", {"工位占用": "占用"}, "plc-gw",
             token="plc-gw-token")
    check(r["status"] == "accepted" and r.get("queue_seq"),
          "3.2 网关凭证提交:已验未结(async 入队)",
          json.dumps(r, ensure_ascii=False))
    check(len(events_of("PlcEdgeReported")) == 0,
          "3.3 入队未落账:事件流尚不可见(结算边界清晰)")
    d = drain()
    check(d.get("drained", 0) >= 1, "3.4 POST /drain 手动落账")
    check(len(events_of("PlcEdgeReported")) == 1,
          "3.5 drain 后迁移沿成事实(事件流可见)")
    r = post("PlcEdgeVoided", "工位01", {"工位占用": "空闲"}, "plc-gw",
             token="plc-gw-token")
    check(r["status"] == "settled", "3.6 修正配平 PlcEdgeVoided(工位空闲)")

    # ---- 第四幕 OEE 派生:三率一填即出(统计员 statistician) -----------------
    print("\n[第四幕] OEE 派生")
    r = post("OrderReceived", "总装线",
             {"车型": "混流产线", "计划产量": 100}, "planner-wang")
    check(r["status"] == "settled", "4.1 产线计划基线:计划产量=100")
    r = post("ProductionReported", "总装线",
             {"报工数量": 5, "实际产量": 5}, "op-zhang")
    check(r["status"] == "settled", "4.2 产线报工:实际产量=5")
    r = post("CompositionDeclared", "总装车间",
             {"aggregates": [{"key": "实际产量", "from": "总装线"},
                             {"key": "计划产量", "from": "总装线"}],
              "可用率": 0.9, "性能率": 0.9, "良品率": 0.9}, "statistician")
    check(r["status"] == "settled", "4.3 组成声明:车间聚合产线 + OEE 三率")
    row = row_of(view("V-REPORT-D2"), "总装车间")
    check(row is not None and row.get("实际产量") == 5
          and row.get("计划产量") == 100,
          "4.4 V-REPORT-D2 车间行:产量聚合自总装线(多属回填)",
          json.dumps(row, ensure_ascii=False))
    check(row is not None and abs((row.get("OEE") or 0) - 0.9 * 0.9 * 0.9) < 1e-9,
          "4.5 OEE 派生=可用率×性能率×良品率=0.729(R-KPI-001)")
    row = row_of(view("V-REPORT-D2"), "总装线")
    check(row is not None and row.get("计划产量") == 100,
          "4.6 D2 产线行同屏(上卷两层层层可读)")

    # ---- 第五幕 停机前留影 + 幂等键首提(运维) ------------------------------
    print("\n[第五幕] 停机前留影与幂等键首提")
    r1 = post("MaterialCallRaised", "CALL-PB5-01",
              {"呼叫状态": "呼叫中", "缺料工位": "工位02"}, "op-zhang",
              idempotency_key="PB5-IDEMP-1")
    check(r1["status"] == "settled" and r1.get("event_id"),
          "5.1 幂等键首提:缺料呼叫入账(PB5-IDEMP-1)")
    idem_id = r1["event_id"]
    ev_before = events_all()
    txt_g1 = view_text("V-EQ-G1")
    txt_h3 = view_text("V-INTEG-H3")
    txt_d2 = view_text("V-REPORT-D2")
    n_views = len(api("GET", "/meta/views"))
    print(f"  (留影:事件 {len(ev_before)} 条,视图 {n_views} 个,三份视图原文)")

    # ---- 第六幕 模拟断电:停库重启,逐比特比对(崩溃恢复) --------------------
    print("\n[第六幕] 模拟断电:stop → restart")
    s.restart()
    set_base(s.base)
    ev_after = events_all()
    check(len(ev_after) == len(ev_before),
          f"6.1 重启后 /meta/events 数量一致({len(ev_before)} 条)")
    check(json.dumps(ev_after, ensure_ascii=False, sort_keys=True)
          == json.dumps(ev_before, ensure_ascii=False, sort_keys=True),
          "6.2 事件日志全量逐条一致(重放逐比特)")
    check(view_text("V-EQ-G1") == txt_g1,
          "6.3 V-EQ-G1 渲染与停机前逐字节一致")
    check(view_text("V-INTEG-H3") == txt_h3,
          "6.4 V-INTEG-H3 渲染与停机前逐字节一致")
    check(view_text("V-REPORT-D2") == txt_d2,
          "6.5 V-REPORT-D2(含 OEE 派生)渲染逐字节一致")
    check(len(api("GET", "/meta/views")) == n_views,
          "6.6 定义层一致(视图定义全量仍在)")
    types = {t.get("type") for t in api("GET", "/meta/event-types")}
    check({"OrderReceived", "BatchBoundToVIN", "InboundMessageRecorded"} <= types,
          "6.7 事件类型定义抽查:重启后注册表完好")

    # ---- 第七幕 幂等跨重启:同键重发不重复入账 ------------------------------
    print("\n[第七幕] 幂等跨重启")
    r2 = post("MaterialCallRaised", "CALL-PB5-01",
              {"呼叫状态": "呼叫中", "缺料工位": "工位02"}, "op-zhang",
              idempotency_key="PB5-IDEMP-1")
    check(r2["status"] == "settled" and r2.get("event_id") == idem_id,
          "7.1 重启后重发同键:返回原 event_id",
          json.dumps(r2, ensure_ascii=False))
    check(len(events_all()) == len(ev_before),
          "7.2 日志不增(重复提交零新事件)")
    check(row_of(view("V-CALL-B6"), "CALL-PB5-01") is not None,
          "7.3 呼叫行跨重启仍在看板(投影重建正确)")

    # ---- 附注:HTTP 覆盖面之外的两块(进程内 SDK 测试兜底) -------------------
    print("\n[附注]")
    print("  - 定义热更新(注册即生效):HTTP 无定义端点,由 mse_tests 覆盖"
          "(进程内 SDK settle_definition)")
    print("  - 定距快照:mse_server 默认快照间隔 0,快照机制由 mse_tests / "
          "mse_demo(SnapshotStore)覆盖")

summary()
