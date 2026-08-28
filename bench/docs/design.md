# bench 设计文档

> **定位**: 根 README §1 心跳模型的可执行基准。不是单元测试，不是端到端业务演示，
> 而是回答一个工程问题——"把五棵数据结构底座的按拍调用串成一个 loop，
> 一拍的成本是多少、瓶颈在哪"。

---

## 1. 测量目标

| 问题 | 对应设计 |
|---|---|
| loop 结构本身有性能问题吗 | 循环体只有顺序调用，计时粒度到阶段，loop 开销自然暴露（实测可忽略） |
| 一拍多久 | 五阶段串行合计 + p50/p95/p99/max 分位数 |
| 瓶颈在哪个模块 | 每阶段独立计时，直接定位 |
| 持久化代价多大 | memory / sqlite 双后端对照 |
| 语义推理能不能按拍跑 | knowledge 预加载 + 每拍 `understand()` 单独计时 |
| 词典规模影响多大 | 种子词典 vs `--full-lexicon`（7.7 万词）对照 |

## 2. 阶段定义与依据

```
感知（合成观测，不计入计时）
  │  entitytree::Observation{attribute_key, value, confidence, source_id, anchor_ref, timestamp}
  ▼
stage 1  entity    Resolver.ingest(obs)
                   覆盖：观测 append → 时间桶侧写聚合 → info_score → 达阈值浮现 → resolve（合并/新建）
  ▼
stage 2  geocore   Kernel.BeginFrame(t) + Kernel.Save(scene, pos, vel, size)
                   覆盖：帧推进 + 写入即规范化（P4）+ AnchorOf 最深容纳 + 惰性物化 + 插桶
  ▼
stage 3  eventstore Event::create + append_event + Profile::create + append_profile
                   覆盖：因果存证主路径（事件锚点 + 原子侧写）
  ▼
stage 4  voxelstore put_block(upsert 当前态)
                   覆盖：时空块当前态写入（版本链 append 属变更路径，非每拍，见 §5 取舍）
  ▼
stage 5  knowledge Engine.understand(store, profile_id)
                   覆盖：摄入幂等 + 八步管道（归一 → 概念匹配 → 分类链 → 规则推理 → OBSERVED_IN 锚点）
```

阶段选择依据：

- **entity 按拍**：观测流是心跳的天然输入，浮现算法必须逐拍运行（设计文档
  `docs/entity/设计文档.md` 的 Resolver 职责）。
- **geocore 按拍**：`BeginFrame` 是帧纪律（INV-4 一帧共享 t），工件落位 `Save`
  是每拍的空间登记。
- **eventstore 按拍**：append-only 因果存证是每拍的审计落点。
- **voxelstore 按拍**：时空块当前态（upsert）随观测演化；版本链 append 只在
  状态变更时发生，不作为每拍基线。
- **knowledge 按拍**：`understand()` 的设计就是"理解一条侧写"（engine.h:
  摄入幂等 + 分类链 + 规则推理 + 锚点），天然是按拍粒度的 API；
  `ingest_all` 才是批量入口，不进入心跳。

## 3. 计时方法学

- 时钟：`std::chrono::steady_clock`，单调、无 NTP 回拨。
- 粒度：每阶段首尾取点，差值即该阶段耗时（µs 浮点）；整拍 = 感知后第一个取点
  到末阶段取点。
- 预热：`--warmup` 拍不计入统计（消除冷缓存 / 首个时间桶建桶 / sqlite 建表
  的一次性成本）。
- 统计：有效样本全量保存，报告 min/avg/p50/p95/p99/max —— 心跳场景关心
  尾延迟（p99 决定节拍稳定性），不只看均值。
- 吞吐：1e6/avg(µs)，串行单线程口径。

## 4. 后端抽象与双口径

```
tick_bench
  └─ Stores{ entitytree::EntityStore, eventstore::EventStore, voxelstore::VoxelStore }
       ├─ memory 口径：MemoryEntityStore / MemoryStore / MemoryVoxelStore
       │              → 纯算法 + 内存索引成本（统一层 RecordBackend 内存参考实现）
       └─ sqlite 口径：SqlEntityStore / SqlStore / SqlVoxelStore
                      → 真实持久化成本（vendored sqlite3，SQLITE_THREADSAFE=0）
```

sqlite 口径**分三个文件**（bench_tick_entity/events/voxel.db）而非单库三连接：
避免多连接同库写锁相互干扰计时。这牺牲"单库部署一份数据"的真实性换测量隔离，
是基准的有意选择，不是部署建议。

## 5. 有意取舍（不测量什么）

| 不含 | 理由 |
|---|---|
| 感知层真实采集 | 传感器/SCADA 属业务侧，基准用合成观测（构造 Observation 不计时） |
| 并发 | 首版回答"单拍串行成本"；并发管线（geocore 桶级锁 / storage 后端连接池）另立基准 |
| voxel 版本链 append | 变更路径（seal + append_version）非每拍基线，避免高估 |
| entity rollup | 维护任务（每小时/每天），由上层调度器触发，不在心跳内 |
| knowledge 装配 | `define_ontology / apply_lexicon / load_ame_lexicon` 一次性预热，单独报告不计入 tick |
| knowledge `infer(query)` | 查询侧推理，由业务按需触发，非每拍 |
| mysql/pg/redis 后端 | 需外部服务；sqlite 已足够暴露持久化瓶颈模式（fsync） |

## 6. 构建集成的两个关键决策

1. **entity 不走 add_subdirectory**：`entity/CMakeLists.txt` 会自行
   `add_subdirectory(storage)` 与 `add_subdirectory(geocore)`；bench 直接以
   同款定义编译 `entity_algo`（`src/resolver.cpp` 单文件静态库），避免目标重复。
2. **storage 由 knowledge 带入**：`knowledge/CMakeLists.txt` 内部
   `add_subdirectory(storage)`（强制 UNISTORE_BUILD_TESTS=OFF），
   bench 先加 knowledge 即获得 `unistore` 目标，不再重复添加。

依赖方向（无环，与仓库整体一致）：

```
tick_bench → entity_algo → unistore ← knowledge(带入 storage)
           ↘              ↗                ↘
            geocore ──────┘            ame_keyword / ame_diffusion
           → knowledge ────────────────┘
```

## 7. 模拟负载的制造语义

- `--stations=K`（默认 8）：K 个工位轮转，anchor_ref = `station-{i}`，
  模拟多工位观测流。
- 每拍 1 条观测、推进 1 秒：attribute 在 {station_state, torque, temperature}
  三键轮转，同工位观测聚入同一时间桶（bucket_size=3600），实体随拍浮现——
  覆盖"新建实体 / 合并到既有实体"两条路径。
- geocore 场景：1km Euclidean3D 厂房，工件尺寸 0.5m，位置随工位分布。
- knowledge：本体 + 种子词典预载；合成侧写词汇不命中种子概念（分类链为空
  是合法结果），测量的是八步管道的**运行成本**而非业务命中率——命中越多
  规则推理越深，成本上界随本体规模变，不在本基准固定负载内。
