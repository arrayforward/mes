# stmb —— 时空记忆块服务(Spatio-Temporal Memory Block Service)

给大模型与智能体提供**可查询、可回溯的 4D 时空结构化外部记忆**:每个记忆块
绑定一个三维空间区域(AABB)和一个时间点,服务支持时空联合查询、版本历史
回溯、多尺度金字塔、动静分离跟踪、众源观测仲裁,并通过 VQL 查询语言与
Function Calling 接口直接接入大模型。

## 项目意义与用途

大模型擅长语言推理,但缺乏三种能力:**空间一致性**(物体的位置、遮挡、
支撑关系)、**精确几何**(坐标、距离、区域包含)、**可验证的记忆**(事实
何时被谁观测、经历过几次变更)。把传感器与众源观测交给纯文本上下文,既
易丢失精度,也无法回答"这里以前是什么"。

stmb 把观测数据变成**结构化、可回溯、可计算**的时空知识库:

- 空间上,每个事实是带 AABB 的体素块,可粗筛精查、可多尺度聚合;
- 时间上,每个事实有时间点、版本链与生效区间,任意时刻可回溯;
- 来源上,每次变更走"变化分类 + 周期模式 + 多源加权仲裁"管线,
  冲突与瞬态噪声不会污染知识库;
- 接口上,大模型通过 tools(Function Calling)或 VQL 直接读写,
  把"记忆"外包给可验证的系统,而不是塞进上下文窗口。

典型用途:

- **具身智能空间记忆**:机器人/智能体把环境观测沉淀为可查询的空间先验;
- **众源城市感知**:多终端上报同一区域,仲裁后形成一致的城市动态图层;
- **机器人导航先验**:静态骨架 + 按需细化的局部细节 + 动态障碍物轨迹;
- 任何需要**「空间 + 时间」双维索引与版本回溯**的场景。

## 特性清单

- **时空联合索引**:空间格子哈希粗筛 + AABB 精过滤 + 时间有序索引范围过滤
- **容量与淘汰**:块级 LRU(经典 list + unordered_map,O(1))
- **块状态机**:Pending → Stable → Changing → Stable,变更走候选确认
- **置信度衰减**:半衰期指数衰减,查询返回有效值、存储基准值不改写
- **版本历史**:生效区间 `[validFrom, validTo)`,任意时刻 `getAt` 回溯
- **本地持久化**:快照 + WAL(自实现二进制帧 + CRC32,原子写、crash 容错;
  v6 起来源注册表同步落盘,重启不丢)
- **分片存储**:按空间+时间分片落盘,manifest 路由,延迟加载、冷分片
  LRU 换出、pin/unpin 防抖动,支持海量对象
- **LOD 金字塔**:多层级格子、自动层级判定、聚合上卷摘要块、先粗后细查询、
  未细化检测
- **动静分离**:动态实例轨迹层(跨源关联、Active/Stationary/Archived、
  静止沉淀为静态块、临时占用标记)
- **观测管线**:六种变化分类(Emergence/Vanishing/Mutation/Seasonal/
  Transient/Conflict)、周期模式、来源加权仲裁、观察窗口、空间一致性约束
- **VQL 查询语言**:类 SQL 文本查询,含时间回溯与先粗后细
- **Function Calling**:8 个工具的 OpenAI 风格 schema + dispatch,迷你 JSON
  自实现,零第三方依赖
- **大模型联调**:stmb_shell(JSON Lines 协议)+ DeepSeek Python 代理

## 架构总览

```
┌──────────────────────────────────────────────────────────────────┐
│ 接口层   stmb_demo(演示)  stmb_shell(JSON Lines)                 │
│          stmb_vql:迷你 JSON · VQL 引擎 · Function Calling        │
├──────────────────────────────────────────────────────────────────┤
│ 服务层   stmb_service:StmbService(门面,统一互斥锁)              │
│          状态机 · 置信度衰减 · 观测管线 · LOD · 动静分离          │
├───────────────┬───────────────┬───────────────┬──────────────────┤
│ 数据/索引层   │ stmb_store    │ stmb_index    │ stmb_history     │
│               │ 块存储+LRU    │ 空间/时间索引 │ 版本历史         │
├───────────────┴───────────────┴───────────────┴──────────────────┤
│ 存储调度   stmb_shard(分片 LRU 缓存)                            │
│            stmb_persistence(快照/WAL/分片文件/manifest,CRC32)   │
│            stmb_dynamic(动态实例轨迹层)                         │
├──────────────────────────────────────────────────────────────────┤
│ 基础层   stmb_core:TimeStamp/TimeRange/AABB/BlockKey/ShardKey/    │
│          MemoryBlock/ChangeType/Observation/PeriodicPattern       │
└──────────────────────────────────────────────────────────────────┘
联调:DeepSeek ⇄ agent/deepseek_agent.py(urllib)⇄ JSON Lines ⇄ stmb_shell
```

模块依赖方向:上层依赖下层,无环。vql 依赖 service;shard 依赖 persistence;
persistence 依赖 core/history/dynamic;service 依赖全部数据与存储调度模块。

## 快速开始(WSL)

```bash
# 构建 + 测试 + 演示
wsl -e bash -lc "cd /mnt/d/agent/voxel && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
wsl -e bash -lc "cd /mnt/d/agent/voxel && ctest --test-dir build --output-on-failure"
wsl -e bash -lc "cd /mnt/d/agent/voxel && ./build/src/app/stmb_demo"
```

Windows 原生亦可构建(MinGW g++ + CMake + Ninja,13 个 ctest 全绿;
test_shell 依赖 fork/wait 仅 POSIX 注册):

```bash
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw -j
ctest --test-dir build-mingw --output-on-failure
```

大模型联调(DeepSeek,API key 只走环境变量,严禁写入任何源文件):

```bash
export DEEPSEEK_API_KEY=sk-...   # 你自己的 key
wsl -e bash -lc "cd /mnt/d/agent/voxel && python3 agent/deepseek_agent.py --query '在区域 [0,0,0,10,10,10] 写入一座房子,时间戳 1000,然后查一下'"
```

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [docs/design.md](docs/design.md) | 整体思路与设计:问题定义、设计目标、参考讨论到工程的映射、取舍汇总 |
| [docs/architecture.md](docs/architecture.md) | 架构:模块职责与依赖图、分层视图、持久化架构、线程模型 |
| [docs/api.md](docs/api.md) | API 参考:StmbService 全部公开 API、VQL 语法、8 个工具、shell 协议、agent 用法 |
| [docs/algorithms.md](docs/algorithms.md) | 关键算法:索引、LRU、分片防抖动、衰减、管线决策树、仲裁、LOD、CRC32 容错 |
| [docs/usage.md](docs/usage.md) | 使用方法:构建、ServiceConfig 全表、场景示例、CTest、大模型接入教程 |

## 测试清单(14 个 CTest,全部离线;Windows/MinGW 下 13 个,test_shell 仅 POSIX)

开启 `-DSTMB_WITH_UNISTORE=ON`(entitytree et_sources 双向桥,见
[docs/usage.md](docs/usage.md))时追加 `test_source_bridge`。

`test_types`(core 几何/键/衰减)、`test_spatial_grid`、`test_temporal_index`
(index)、`test_block_store`(LRU)、`test_version_log`(历史)、
`test_persistence`(快照/WAL/crash 容错/恢复)、`test_shard`(归属/懒加载/
换出/重启/海量 smoke)、`test_lod`(层级/分层查询/上卷/未细化/分片组合)、
`test_dynamic`(轨迹/关联/状态推进/沉淀/占用/持久化)、`test_arbitration`
(六种分类/周期模式/加权仲裁/窗口/空间约束/v5)、`test_vql`(JSON/VQL/
Function Calling)、`test_shell`(stmb_shell 协议)、`test_stmb_service`
(服务单测)、`test_e2e`(端到端场景集)。

## 持久化格式版本

二进制格式自实现(小端、magic 'STMB'、帧 `{type, len, payload, crc32}`):

| formatVersion | 引入内容 | 兼容性 |
| --- | --- | --- |
| 1 | 快照 + WAL(单文件模式) | — |
| 2 | 分片存储(manifest + 分片文件) | 不兼容 v1 |
| 3 | LOD 层级字段、ShardKey 加 level 维 | 不兼容 v2 |
| 4 | 动态实例层(SnapInstance/dynamic.stmb) | 不兼容 v3 |
| 5 | 观测管线(加权 confirmations、窗口、周期模式、suspect) | 不兼容 v4 |
| 6 | 来源注册表落盘(SnapSource 小节 + RegisterSource WAL) | 不兼容 v5,当前版本 |

本工程无线上数据,版本升级直接换代,旧数据目录需清空重建。
