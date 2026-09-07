# mse — 车间世界模型(MES 内核)

> **定位**:挂在 entity / geocore / knowledge / storage / voxel 五个底座之上的**业务模块**,
> 是 `docs/mes` 五份文档(《系统API设计_事件即接口》v3、《车间世界模型_系统实现方案》v1 等)
> 的工程落地——一个制造执行系统内核:四原语(本体/事件/规则/视图)+ 两个动词
> (`POST /events`、`GET /views/{viewId}`),没有第三种资源。
>
> 一句话:写侧一个端点 + 一张事件类型注册表;读侧一个端点 + 一张视图注册表。
> 没有 update、没有 delete,API 的演化是 append-only 的,与事件树同构。

```
接入层    UI(视图引擎渲染) │ 扫码/RFID │ PLC 网关 │ ERP 适配 │ LLM 适配
                │  变化描述(纯 k-v,候选)
写侧      API 网关 → 四层校验 [0]字典登记 [1]可聚合性 [2]类型 schema [3]规则过滤
                → 决策环(预演 → 结算,单写者)→ 事件日志 append(FACT,域内全序)
存储投影  事件日志(storage RecordBackend)+ 投影引擎(fold → 本体聚合属性集,多属)
读侧      视图引擎(四元组求值:queries/selects/rules/emits;终态/流水/拦截;AS OF t)
定义层    四个数据集合(属性字典/事件类型注册表/规则集/视图注册表)+ 锚点表,
          定义也走候选-结算,运行中热更新;四集合 = 定义事件流的物化投影
时空树    锚点/精度匹配/轨迹/AS OF t:voxel(stmb)运行期 + voxelstore 持久化
协助层    knowledge 只产出建议(属性提取/协助推理);entity 浮现新实体——
          两者的产物都只是候选,四层校验是唯一结算入口
```

## 四原语与两个动词

| 原语 | 落地形态 |
|---|---|
| 本体(体) | 唯一 id + k-v 属性聚合(投影),没有类;诞生 = 首个携带该 id 键的事件 |
| 事件(变) | `Event = (type, writes{id:{键:值}}, spacetime, actor, evidence?, corrects?)`;append-only,已结算永不修改 |
| 规则(法) | 属性谓词(deps)+ JSON-logic 纯函数;filter/derive/trigger 三效果,无环境沙盒 |
| 视图(看) | 注册表四元组(queries/selects/rules/emits);只读,无任何写路径 |

| 动词 | 端点 | 语义 |
|---|---|---|
| 提交变化描述 | `POST /events` | 声明一次发生,返回候选回执(settled / rejected+layer+violations) |
| 求值视图 | `GET /views/{viewId}?observer=&entity=&t=` | 按视图契约渲染截面;`t` = AS OF 事件序号 |

## 目录结构

```
mse/
├── include/mse/         全部公开头文件(契约先行)
├── src/                 17 个实现文件(见下表)
├── assets/              knowledge 协助用种子(制造域本体 + 词典)+ wasm/ 预烘焙规则产物
├── demo/sim_auto_flow.cpp   整车厂总装车间的一天:端到端综合流程(交付演示;
│                            `--server host:port` 切换为纯 HTTP 客户端模式打 mse_server)
├── server/main.cpp          mse_server:人工验证用长驻 HTTP 服务器(自省端点 + 静态托管)
├── web/                     免构建 H5 验证台(React 18 UMD + htm,vendor 本地化)
├── tests/test_main.cpp      红线测试(极简自带框架,466 项断言)
└── CMakeLists.txt       引入兄弟模块(knowledge/geocore/entity/voxel→storage)+ wasm3
```

## 组件清单(源码 ↔ 文档章节)

| 文件 | 职责 | 文档依据 |
|---|---|---|
| `src/model.cpp` | 核心数据形态:SpaceRef/ChangeSet/Event/Receipt 及确定性序列化 | API 文档 §二、§五 |
| `src/event_log.cpp` | 事件日志:append-only、域内全序,内存镜像 + 启动重放 | 实现方案 §3.1 |
| `src/dictionary.cpp` | 定义层:四集合 + 锚点表;定义候选-结算、引用完整性强校验、热更新 | API 文档 §六 |
| `src/rules.cpp` | 规则集 + 无环境沙盒求值器(JSON-logic 方言)、静态检查 | 实现方案 §3.3 |
| `src/wasm_sandbox.cpp` | WASM 规则沙盒(wasm3)+ 烘焙管线:静态闸、试跑、版本钉死 | 实现方案 §3.3 |
| `src/projection.cpp` | 投影引擎:fold → 本体聚合属性集(属性多属)、确定性哈希、定距快照存取 | 实现方案 §2.2/§3.4 |
| `src/pipeline.cpp` | 写侧管线:四层校验 + 决策环(单写者)、幂等(持久化)、异步结算边界(drain_async)、触发/推导(on_types 变化驱动)、拦截记录 | API 文档 §三、实现方案 §3.2 |
| `src/plc_filter.cpp` | 适配层:PLC/传感器三层过滤(迟滞+驻留+聚合去重)与映射表网关,迁移沿 → 候选 | API 文档 §七、实现方案 §3.9 |
| `src/view_engine.cpp` | 读侧视图引擎:四元组求值,终态/流水/拦截/遍历四模式,AS OF t(快照加速),按钮可用性,读侧行过滤 | API 文档 §八 |
| `src/api.cpp` | API 门面:两个动词(进程内),扁平载荷 → Candidate 归一化 | API 文档 §一 |
| `src/http_server.cpp` | 极简 HTTP/1.1 服务器与客户端(原生 socket,零第三方依赖) | 实现方案 §3.8 |
| `src/spacetime.cpp` | 时空树:锚点精度匹配、时空存证(stmb 块 + 版本链)、轨迹、AS OF t | API 文档 §五、实现方案 §3.6 |
| `src/knowledge_assist.cpp` | knowledge 协助:自由文本属性提取、规则协助推理(只产建议) | API 文档 §七 |
| `src/entity_bridge.cpp` | entity 演化桥:观测流 → 浮现实体 → EntityObserved 候选 | 用户约定(非人力实体创建) |
| `src/seeds.cpp` | 种子定义:系统键 + 整车厂业务全覆盖(46 视图的机制实例化,216 条定义全结算;含 PLC 迁移沿类型) | 视图提取文档(46 视图) |
| `src/seeds_wasm.cpp` | WASM 种子规则:读 assets/wasm 预烘焙产物,经同一条 settle_definition 注册 | 实现方案 §3.3 |
| `src/system.cpp` | 系统装配:构造即完成定义加载、日志重放、投影(快照加速)与时空重建 | 实现方案 §一 |
| `server/main.cpp` | mse_server:人工验证长驻 HTTP 服务器——两个动词 + /meta/* 只读自省 + web/ 静态托管 + X-MSE-Token 信任分级;种子含 SequenceAdjusted 的 WASM 规则接入 | 实现方案 §3.8 |
| `web/`(index.html / app.js / vendor) | 免构建 H5 验证台:视图菜单(A–H 分组)、四种渲染模式、事件表单(字典驱动)、拦截卡片、事件流侧栏、AS OF t | API 文档 §八(视图即数据,前端无业务逻辑) |

## 构建与测试

```bash
cmake -S mse -B mse/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build mse/build -j
ctest --test-dir mse/build --output-on-failure   # mse_tests + mse_demo_smoke
./mse/build/mse_demo        # 汽车生产短流程端到端模拟(生成 mse_demo.db)
./mse/build/mse_server --port 18080   # 人工验证服务器(H5 见下节)
```

- `mse_tests`:27 组红线测试(memory 后端,466 项断言)——重放逐比特一致、
  四层校验各自拦截、修正链、读写一致性、定义热更新、时空、规则 DSL、
  HTTP 端到端、knowledge 协助、entity 演化桥、WASM 沙盒(烘焙/静态闸/全链路)、
  定距快照与崩溃恢复、AS OF 快照加速一致、幂等键持久化、on_types 变化驱动、
  遍历视图,以及业务覆盖组:B5 遍历、B4 拦截、F15 逐级报警、B7-B10 规则决定行、
  冻结禁调序、达成率 derive(除零保护)、WASM 接入后 05 双拦截,
  与 P4 组:PLC 三层过滤单元、噪声洪峰(10 万采样 → 2 事件)、异步结算边界、
  信任分级(min_trust L2 + HTTP X-MSE-Token + accepted 回执序列化)。
- `mse_demo_smoke`:demo 全量跑一遍(约 4s,自带红线断言,退出码非零即失败)。
- mse 库与两个 exe 均 `-Wall -Wextra` 零警告;MinGW 下运行库静态链接,exe 可独立运行。

## HTTP API 示例

`POST /events`(扁平载荷,系统键之外皆属性写入;"一次发生、多处变化"用 writes 显式形态):

```json
{
  "type": "DefectRegistered",
  "id": "VIN-LBV8W3105KM123456",
  "actor": "QC-liuyang",
  "车漆": "划痕",
  "缺陷级别": 2,
  "space": {"raw": "左前门"},
  "time": "2026-09-02T14:03:11+08:00",
  "evidence": "img:defect-8821.jpg",
  "idempotency_key": "qc-20260902-0001"
}
```

回执(被拒候选零事件、零补偿、零污染;拦截层号 0..3):

```json
{"status":"settled","event_id":21}
{"status":"accepted","queue_seq":3}
{"status":"rejected","layer":3,"violations":["R-QUAL-LOCK:锁定车辆不得下线"]}
```

`accepted` 为异步结算类型(`settlement="async"`)的受理回执:四层校验已通过、
候选在队列中悬而未结,`drain_async` 后才进事件日志。

`GET /views/V-PLAN-A3?observer=计划员`(节选):行 = 本体终态,按钮可用性与写侧 L3 同一份规则——
"界面上能点的 ⇔ 系统能结算的";`?t=<event_id>` 对所有视图做 AS OF 时间回溯。

```json
{"view_id":"V-PLAN-A3","render_mode":"终态",
 "rows":[{"id":"VIN-LBV0001","车型":"SUV-A","计划状态":"05",
          "buttons":[{"type":"SequenceAdjusted","enabled":false,
                      "reasons":["R-PLAN-ADJUST:...05 绝对禁止调整"]}]}]}
```

## 人工验证 H5(mse_server + web/)

`mse_server` 把内核装配成长驻 HTTP 服务:两个动词 + 只读自省端点 + 静态托管
`web/` 验证台(React 18 UMD + htm,免构建,vendor 已本地化,离线可用)。

```bash
./mse/build/mse_server --port 18080     # 空库自动装种子(216 条定义 + WASM 规则接入)
# 浏览器打开 http://127.0.0.1:18080/
```

- **菜单组织**:左侧视图列表按视图号尾字母分 A–H 八组(A 计划 / B 物料 / C AVI /
  D ANDON / E PMC / F 质量 / G 设备 / H 集成);尾号非字母的(如 V-REWORK-001)归"其他"。
- **凭证**:顶栏"凭证"输入框即 `X-MSE-Token`:`ui-token`(L2,默认,存 localStorage)、
  `plc-gw-token`(L1)、`erp-token`(L1);不带/错凭证 trust=0——`PlcEdgeReported`
  (min_trust=1)会被 L2 拒,可现场演示信任分级。
- **发起事件**:"+ 发起事件"按事件类型注册表自动生成表单(系统键 + 属性键;
  enum 下拉/值域/单位/必填全部来自属性字典);回执三态:settled(落账 event_id)、
  rejected(layer + violations 红框展示)、accepted(异步悬态,点"落账"触发
  POST /drain 后进日志)。
- **拦截反馈**:拦截视图(V-PKE-B4、V-DEFECT-F10)把 RejectionLog 渲染为
  分层着色卡片(L0 格式 / L1 字典 / L2 信任 / L3 规则),附候选原文。
- **观察者 / 实体 / AS OF**:观察者下拉来自当前视图的 variants;entity 喂遍历与
  纵切视图(节点可点击继续游走);AS OF t 对所有视图做时间回溯。
- **端到端驱动**:`./mse/build/mse_demo --server 127.0.0.1:18080` —— 九节场景
  全部经真实 HTTP 打该服务器(客户端模式:不构造 System、不装种子;entity 桥 /
  knowledge / 投影哈希 / 崩溃恢复等进程内小节自动跳过,收尾改用 /meta/events
  事件总数、拦截视图 rejections、幂等键重放、AS OF 对比验证)。

服务端点一览:`POST /events`、`GET /views/{id}`、`POST /drain`、
`GET /meta/views|event-types|attributes|anchors|ontologies|events`;
`GET /` 与任意非 API 路径按 web/ 根静态文件处理(防 `..` 穿越,未命中 404)。

## demo 场景(sim_auto_flow):整车厂总装车间的一天

sqlite 后端 + voxelstore 时空持久化 + 全量业务种子 + WASM 种子 + 定距快照(间隔 20),
102 条事件全程真实 HTTP 驱动(PLC 适配层小节走进程内适配器入口),九节。
`--server host:port` 切换为纯客户端模式:不构造 System、全部动作打外部 mse_server,
进程内能力小节(entity 桥/快照/哈希重放/崩溃恢复)跳过,收尾走 HTTP 可观测验证:

1. **开班**:ERP 订单经 `InboundMessageRecorded` 进系统 → `OrderReceived`×3 +
   产线计划基线 → A2 排序定序列号、`OrderSwapped` 换单 → `OrderFrozen` 冻结一单 →
   冻结后调序被 **R-SEQ-FROZEN** 拦截 → A3 五连发两单 → 05 调序被
   **R-PLAN-ADJUST + R-PLAN-ADJUST-WASM 双重拦截**(jsonlogic 与 WASM 同一立法);
2. **物料**:`BomReceived` 下达工位 BOM/物料需求/工艺信息 → B4 防错:扫错料被
   **R-MAT-PKE** 拦截(进 RejectionLog)、扫对通过 → 批次登记(带供应商 ref)→
   `BatchBoundToVIN` 两车各绑两批(一次发生、VIN 与批次双向 ref 同写)→
   线边/中央库存、JIS 拉动全生命周期 + 三类拉动单 → 缺料呼叫→响应,一单留在看板;
3. **生产**:AVI 两车过工位 01-03(VIN2 离区再入)→ ANDON 呼叫→停线→复线 →
   PMC 同检点连发 10 次故障:**同检点故障计数 derive 递增(null→0 起步),
   第 3 次触发科长预警、第 10 次触发部长预警**(trigger 产物仍走四层校验);
   **PLC 噪声洪峰**:工位01 占用光电 100000 个含噪采样(LCG 确定性)经适配层
   三层过滤 → 迁移沿 2 个 → 异步受理 2 条 → drain 落账 2 事件(零污染、有界);
   信任分级实况:无凭证/错凭证 POST PlcEdgeReported 被 L2 拒(信任级不足),
   带 PLC 网关凭证 accepted → drain 后落账;
4. **质量**:检验计划/任务 → 检测线数据写入 → 划痕(级别 2)自动锁车 →
   锁定期间报工被 **R-QUAL-LOCK** 拦截 → 误扫缺陷(级别越界)被 L2 拦截 →
   返修全流程(NOK 自动再开→OK 关闭)→ `JudgementOverruled` 误判改判(corrects 链)
   → 解锁;VIN2 误录缺陷 → `DefectCancelled` 冲正;
5. **设备**:两台设备状态上报(一台故障→运行);
6. **收尾**:报工一次发生(VIN 与产线同时就位,属性多属)→ 误报工 → `ReportReversed`
   冲正,**达成率/FTT derive 随动回摆** → `CompositionDeclared` 声明车间聚合产线的
   产量属性(多属回填)+ OEE 三率派生 → entity 桥浮现新载具;
7. **读侧**:44 个视图全清单逐个 GET 打印摘要 + 12 个重点视图详打
   (A1/A2/A3、B4 拦截、B5/F14 遍历图、B6 规则决定行、D1、F15、返修流水、
   F9 位置历史、A6 达成率、F16 质量报表)+ AS OF t 对比;
8. **收尾**:事件 102 / 拦截 7 / 最近快照 #100 → 投影哈希 ×3 重放逐比特一致 →
   同 db 重建 System(快照 + 增量重放)哈希一致 → 幂等键跨进程重放返回原回执。

## 46 视图覆盖对照表

机制实例 44 个视图定义(全部经 `ViewRegistered` 结算);配置类视图由定义层四集合
本身承载(它们就是四集合的编辑界面);I 平台建模层不做。

| 模块 | 视图 | 落地 |
|---|---|---|
| A 生产计划 | A1 订单接收 / A2 排序 / A3 下发队列 / A4 工位指导 / A5 状态监控 / A6 效率分析 / A7 报工流水 | `V-ORDER-A1` / `V-SEQ-A2` / `V-PLAN-A3` / `V-STATION-A4` / `V-MONITOR-A5`(时空切片) / `V-EFF-A6`(R-KPI-002 达成率) / `V-REPORT-A7`(L1+L2) |
| B 物料 | B1 主数据 / B2 BOM / B3 库存 / B4 防错 / B5 追溯 / B6 缺料呼叫 / B7-B10 四类拉动 / B11 线边库 | `V-MAT-B1` / `V-BOM-B2` / `V-STOCK-B3` / `V-PKE-B4`(**拦截**,R-MAT-PKE) / `V-TRACE-B5`(**遍历**) / `V-CALL-B6`(R-VIEW-CALL 规则决定行) / `V-KANBAN-B7`·`V-PULL-B8`·`V-JIS-B9`·`V-JIT-B10`(同一事件集,R-VIEW-* 各出本类型行) / `V-LINESIDE-B11`(线边库切片) |
| C AVI | C1 实时位置 / C2 跟踪查询 / C3 区域跟踪 | `V-AVI-C1` / `V-TRACK-C2`(流水 entity,L1+L2) / `V-ZONE-C3`(时空切片) |
| D ANDON | D1 安灯大屏 / D2 达成看板 / D3 呼叫停线记录 | `V-ANDON-D1` / `V-REPORT-D2` / `V-CALLLOG-D3`(流水) |
| E PMC | E1 设备监控 / E2 计数 / E3 产量统计 / E4 故障分析 / E5 报警查询 / E6 作息配置 / E7 统计报表 / E8 Web 门户 | `V-PMC-E1` / `V-COUNT-E2` / `V-CHART-E3` / `V-FAULT-E4`(R-VIEW-FAULT 规则决定行) / `V-ALARM-E5`(流水);E6 为配置视图(定义层承载);E7 = E2/E3 的上卷同构(variants 口径);E8 = HTTP 读接口本身(GET /views 即只读门户) |
| F 质量 | F1 缺陷字典 / F2 检验计划 / F3 动态检验规则 / F4 任务分配 / F5 布局建模 / F6 车型关联 / F7 车辆参数 / F8 随车卡 / F9 位置历史 / F10 缺陷采集 / F11 返修流程 / F12 质量记录 / F13 锁定列表 / F14 车辆追溯 / F15 报警规则+列表 / F16 综合报表 | F1/F3/F6 为配置视图:字典=属性字典+规则集(R-QUAL-* 即动态检验规则),布局=锚点表+聚合集声明(`CompositionDeclared`),关联=定义事件——均由定义层承载;`V-PLAN-F2` / `V-TASK-F4` / `V-CAR-F7` / `V-CARD-F8` / `V-HIST-F9`(流水 entity) / `V-DEFECT-F10`(**拦截**) / `V-REWORK-001` / `V-QUALREC-F12`(L1+L2) / `V-LOCK-F13` / `V-TRACE-F14`(**遍历**) / F15 规则侧=R-ALM-003/010(trigger 定义即规则配置),列表侧=`V-ALARM-F15` / `V-QUALITY-F16`(缺陷总数/FTT/实际产量) |
| G 设备 | G1 台账 / G2 状态详情 | `V-EQ-G1` / `V-EQ-G2`(entity 纵切) |
| H 报表/系统/集成 | H1 报表管理 / H2 系统管理 / H3 接口监控 | H1 = 上卷类视图的公共机制(D2/A6/F16 即实例);H2 为配置视图(定义层承载);`V-INTEG-H3`(流水 L1+L2) |
| I 平台建模层 | I1-I3 | 不做(平台级建模/仿真/监视超出 MES 内核范围) |

## WASM 规则沙盒与烘焙管线

规则可以是 WASM 产物(`runtime="wasm"`),与 JSON-logic 规则同走一条
`settle_definition` 路径。引擎为 wasm3 解释器(vendored `third_party/wasm3`,
只用核心:不启用 WASI/文件/环境能力——无环境纪律在引擎配置层锁死)。

- **ABI**(宿主导入模块 `"mse"`,全部确定性纯函数):`attr_len/attr_get` 读当前属性
  (JSON 文本)、`write_len/write_get` 读候选对目标本体写入的新值、`result` 回传结论
  (kind ∈ pass/reject/value/noop + payload;trigger 的 emit 结论以
  `{"emit":[候选,...]}` JSON 经 value 通道回传)。客体导出 memory + `mse_eval`。
  deps 声明即最小权限:宿主在 ABI 层对 deps 之外的键一律返回"不存在"。
- **烘焙管线**(定义结算的固定环节):base64 产物 → 解码 → 静态检查(deps 已登记、
  导出存在、**导入不超出 ABI 白名单**——`evil_clock.wasm` 伸手 `env.clock` 即被拒)
  → 沙盒试跑 → 钉 `artifact_hash`(FNV-1a 64,内容寻址)+ `engine_version`
  (`"wasm3 " M3_VERSION`)。同产物 + 同引擎 = 逐比特重放;编译失败 = 定义候选被拒。
- **实例**:`assets/wasm/plan_adjust.wat/.wasm`(随仓库提交的预烘焙二进制,构建期不
  依赖 wabt)——A3 五状态机调序过滤的 WASM 版,`load_wasm_seeds` 注册为
  `R-PLAN-ADJUST-WASM`;接入 `SequenceAdjusted` 规则族后与 jsonlogic 版
  `R-PLAN-ADJUST` 同一立法、双重把关(demo 第 1 幕的 05 双拦截)。

## 运行时能力(本期新增)

- **定距快照**:`WritePipeline::set_snapshots(store, n)` 每 n 条已结算事件落一份
  投影快照(表 `mse_snapshots`,append-only);崩溃恢复 = 最近快照 + 增量重放,
  AS OF t = floor 快照 + 增量重放(与全量重放逐字节一致,测试 13 钉死)。
- **幂等键持久化**:候选层 `idempotency_key` 的结算回执落 `mse_idempotency` 表,
  跨进程重启重复提交仍返回原回执、日志不增(System 装配默认开启)。
- **遍历视图**:`render_mode="遍历"`,以 `entity` 参数为起点沿 datatype `"ref"` 的
  属性(关系即属性)双向 BFS(深度 3)+ corrects 因果边,输出从起点可达的
  节点/边子图(节点含 selects 指定的属性列)。
- **on_types 变化驱动**:derive/trigger 规则声明 `on_types` 后只在列出的事件类型
  结算后求值;计数器类 derive 的 target 键可缺省(null→0 起步),冲正类事件
  列入 on_types 即可让派生值随修正回摆(达成率/FTT 均覆盖 ReportReversed)。
- **读侧行过滤(规则决定行)**:视图 `rules` 引用 consumers=read 的 filter 规则,
  终态视图只渲染通过的行——同一份规则定义,写侧过滤候选、读侧决定行(§4.5)。

## P4:适配层 / 异步结算 / 信任分级

- **适配层(三层过滤 + 映射表契约)**:高频机器信号(PLC 原始流)不是事件类型,
  先过 `PlcFilter` 三层过滤——[1] 迟滞状态机(越 `on` 进高态、破 `off` 回低态,
  中间带不动,去抖)→ [2] 驻留(新态须连续稳定 `dwell_ticks` 拍才采信翻转,
  值回落清零重计)→ [3] 聚合去重(同一迁移沿在 `dedup_window_ticks` 窗口内只出
  一次)。迁移沿经 `AdapterGateway` 映射表翻译成变化描述候选(type/目标/键/
  高低态值/actor/锚点/tick 文本),再照常走 `POST /events`——适配器契约:
  输入外部信号、输出变化描述,别无其他。时间用外部注入的逻辑 tick,不读物理时钟。
- **异步结算边界**:事件类型注册 `settlement="async"` 后,四层校验(L0-L3)仍同步
  执行、即时返回拒绝;全部通过才入队,回执 `{"status":"accepted","queue_seq":n}`
  (异步回执同样幂等)。**悬态口径:候选已验未结——在 `drain_async` 之前不进事件
  日志、视图看不到它,它悬在 L0(候选)与 L1(事实)之间的队列里。**`drain_async`
  单写者 FIFO 串行结算,全序保持;settle 内触发的递归候选仍走同步语义。
- **信任分级**:`EventTypeEntry.min_trust`(非负整数,定义层校验)声明类型的最低
  信任级;候选 `trust` 低于它在 **L2** 被拒(`信任级不足: <type> 要求 >=N`)。
  `trust` 是凭证元数据,由入口注入、**不序列化进事件**:HTTP 适配器从
  `X-MSE-Token` 头查 `ApiGateway::set_trust_tokens` 登记表(未携带/未登记 → 0),
  进程内 SDK 调用显式传入(如 trust=3)。种子实例:`PlcEdgeReported`
  (async + min_trust=1,PLC 网关级)与其修正类型 `PlcEdgeVoided`,写
  `工位占用`(enum 空闲/占用)。
- **噪声洪峰验证**(红线:噪声洪峰下事件率有界):100000 个在阈值附近抖动并带
  越阈突刺的含噪采样,经三层过滤只剩 2 个迁移沿(== 真实翻转次数),drain 后
  事件日志恰增 2 条、零污染(demo 第 3.5 节与测试 26 双重钉死)。

## 设计纪律

- **append-only**:事件与定义事件只增不改;纠错 = 携带 `corrects` 的新事件(修正链)。
- **候选-结算**:一切写入(业务事件、定义、AI 建议、规则触发、实体浮现)都先为候选,
  四层校验是唯一结算入口;被拒零事件零补偿零污染。
- **读写同一份规则**:写侧 L3 与读侧按钮可用性调用同一个 `check_filters`——
  结构上一致,不是约定。
- **AI 不碰状态**:knowledge 只返回建议(提取/推理),entity 浮现只翻译为候选;
  无任何写路径。
- **storage 是唯一持久化接口**:事件日志、定义事件、时空持久化(voxelstore)全部
  跑在 `RecordBackend` SPI 上,换后端不动业务。
- **确定性**:核心链路不读物理时钟/随机数;结算逻辑时刻 = `settle_seq`(域内全序);
  派生(推导规则)是 fold 的一部分(投影派生钩子),在线结算与重放同一条路径,
  同输入重放 3 次投影哈希逐比特一致。

## 已知边界与后续

- ~~**异步结算未做**~~(**P4 已实现**:`settlement="async"` 四层校验后入队,
  `drain_async` 单写者串行结算;见上节"异步结算边界")。批量导入的独立通道未做。
- **缺陷计数口径**:`缺陷总数` 是"登记累计"(DefectRegistered +1),不随
  DefectCancelled 回减——冲正史在事件树与流水视图完整可查;若业务要"在册缺陷数",
  增一条 on_types=[DefectCancelled] 的 -1 规则即可(定义热更新,不动内核)。
- **逐级报警为阈值沿语义**:R-ALM-003/010 是 `>=` 谓词,越过阈值后每次同检点故障
  都会再发一条同级预警(规则即数据,要"仅跨沿一次"可改 `==` 谓词,定义热更新)。
- **stmb 动态实例层当前配置下未启用**:SpacetimeTree 以便捷构造
  (`StmbService(4096, 1.0, 1)`,`archiveTimeoutMs=0`)使用时空块服务,
  `reportMoving` 返回 0;本体轨迹由锚点块 payload 反查重建(确定性等价),
  动态层启用后可直接切换。
- **跨域字典未做**:单车间单域一本字典,域内省略前缀;跨域全名(`domain.key`)
  与域际方言翻译层留待 M2。
- **空间索引从简**:锚点匹配为层级路径段 + 末段前缀(车间尺度足够),
  坐标-锚点换算与 R-tree 区域查询未启用;模糊原文只存档不参与锚点切片。
  存储层 `RecordBackend` 的查询算子已扩到 Eq/Ne/Le/Ge/Prefix/IsNull/NotNull
  (Ne/Prefix 为本期新增,锚点路径前缀查询可直接下推到后端)。
- **HTTP 子集**:仅实现本系统需要的 HTTP/1.1 子集(Content-Length、
  Connection: close、JSON 响应);**信任分级已实现**(`X-MSE-Token` → 信任级 →
  min_trust L2 校验,见上节),完整鉴权/限流未做(文档未决项)。
