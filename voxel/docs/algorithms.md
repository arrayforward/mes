# stmb 关键算法

> 本文目的与读者对象:面向想理解 stmb 内部机制的开发者与评审者。按主题
> 给出核心算法的流程与复杂度要点,引用真实实现位置。架构全貌见
> [architecture.md](architecture.md)。

## 1. 空间哈希网格:粗筛 + 精过滤

实现:`src/index/spatial_grid.cpp`。

- 结构:`unordered_map<CellCoord, unordered_set<BlockId>, CellCoordHash>`。
- 插入/删除:对 AABB 的每个轴计算 `floor(min/cellSize)..floor(max/cellSize)`,
  三重循环枚举覆盖格子,逐格增删 id;空格子销毁。复杂度 O(覆盖格子数)。
- 查询:同样离散化查询区域,取覆盖格子 id 集合的并集(粗筛);服务层再
  `AABB::intersects` 精确过滤。格子边界上的块可能多进相邻格子——粗筛
  只保证不漏。
- 格子/时间槽编号用 `std::floor` 除法,负坐标正确分桶。

## 2. 时间有序索引

实现:`src/index/temporal_index.cpp`。

- 结构:`std::map<TimeStamp, unordered_set<BlockId>>`(按时间有序)。
- `queryRange`:从 `lower_bound(start)` 顺序扫描到 `end`,沿途拼接,O(log n + k)。
- `expireBefore(ts)`:`lower_bound(ts)` 之前的前缀整体收集 id 后一次性
  erase,O(过期桶数),返回被清理 id 供服务层同步清理。

## 3. 两级 LRU

### 3.1 块级 LRU(src/store/block_store.cpp)

`std::list<BlockId>`(前端=最近使用)+ `unordered_map<BlockId, Entry{块,
链表迭代器}>`。get/put 命中用 `splice` 提到前端(O(1) 且不失效其他迭代
器);满员淘汰链表尾部。`put` 返回被淘汰块完整数据(服务层清理索引或
下沉需要)。

### 3.2 分片级 LRU(src/shard/shard_manager.cpp)

已加载分片同样用「链表 + 哈希表」管理;超过 `maxLoadedShards` 换出最冷
的**未钉住**分片:脏分片先归并写盘(内存现状 ∪ 盘上旧数据的下沉块),
再经回调从内存组件移除;干净分片直接释放。

## 4. 分片 pin/unpin 防抖动

问题:一次查询触达的分片数可能超过驻留上限,若边加载边换出,先加载的
分片被挤出去,查询漏数据(e2e 实测捕获过)。

方案:查询前 `loadTouchedShards` 对每个触达分片 `ensureLoaded` 后立即
`pin`(计数 +1);`evictColdest` 从 LRU 尾部向前找第一个 `pins==0` 的分片,
全部被钉住时允许临时超过上限;查询结束统一 `unpin` 并 `trimToLimit()`
收缩回上限。

## 5. 置信度半衰期衰减

实现:`src/core/types.cpp::decayedConfidence`。

```
effective = base * 0.5 ^ ((now - lastUpdate) / halfLifeMs)
```

`elapsed <= 0` 或 `halfLifeMs <= 0` 时原样返回 base。衰减只作用于查询/
读取的返回副本,存储基准值不改写;带 `now` 的 API 才触发计算。

## 6. 观测管线分类决策树

实现:`src/service/stmb_service.cpp::submitObservationLocked`。判定顺序
(先命中先生效):

```
块不存在?
  ├─ 观测为空      -> Vanishing(不接受,无操作)
  └─ 观测非空      -> Emergence(不接受:需先 put 空地占位块)
块有注册周期模式,且观测 == 当前相位预测?
  └─ 是            -> Seasonal(按加权确认处理,不进 Changing,不刷版本)
观测 == 当前生效 payload(无挂起候选)?
  └─ 是            -> Transient(计入加权确认;Pending 块达阈值转 Stable)
块处于 Changing 且候选挂起中?
  ├─ 观测 == 当前生效 -> Transient(反向推翻,关闭窗口,不产生版本)
  ├─ 观测 == 候选     -> 累加权重;达阈值则候选生效(Emergence/Vanishing/
  │                      Mutation),否则继续挂起
  └─ 第三种值         -> Conflict(保持挂起,等待仲裁)
块非 Stable         -> Conflict(未稳定即收到矛盾观测)
块 Stable(新候选):
  ├─ 单源权重 >= 阈值 -> 立即生效(Emergence/Vanishing/Mutation)
  └─ 否则             -> 挂起进观察窗口(windowStart/windowWeight/windowSources)
```

分类规则:观测为空 + 当前非空 → Vanishing;当前为空 + 观测非空 →
Emergence;双方非空互异 → Mutation。`sweepWindows(now)` 丢弃
`now - windowStart > observationWindowMs` 的候选并恢复 Stable。

## 7. 加权仲裁与观察窗口

- 来源权重:`SourceRegistry::reliabilityOf`,注册 0~1,未知默认 0.5;
  两参 `confirm` = 系统自确认权重 1.0。
- Pending 转 Stable 与候选生效都看**加权和**是否 >= `confirmThreshold`
  (double);挂起候选的权重记录在块上(windowWeight/windowSources),
  多源同候选观测逐次累加。
- WAL 中 Confirm 记录直接携带权重(重放精确还原);Observe 记录重放复跑
  管线。
- **来源注册表落盘(v6)**:`registerSource`/权重变更追加 RegisterSource
  WAL 记录;快照与 manifest 携带 SnapSource 来源小节;重启后快照还原 +
  WAL 回放,来源权重完整保留(此前注册表不落盘,需重新 registerSource)。
  此外服务暴露 `setSourceChangeHook` 变更钩子,供外部系统(如 entitytree
  et_sources 桥)同步来源画像,见 [usage.md](usage.md) 桥接章节。

## 8. 周期模式相位判定

实现:`submitObservationLocked` 内的模式分支。

```
phase = ((t % periodMs) + periodMs) % periodMs   // 负时间戳也安全
命中:phase ∈ [offsetMs, offsetMs + durationMs) 且 Phase.payload == 观测 payload
```

命中即 Seasonal(不刷版本,避免昼夜/季节反复产生版本);与模式冲突的新
稳定值仍走正常 Mutation,模式本身可演化。模式随块持久化(v5)。

## 9. LOD 层级判定与上卷聚合

### 9.1 自动层级判定(detectLevel)

取块 AABB 最大轴长度 extent,选 `|levelCellSizes[level] - extent|` 最小的
层级;并列取更细(`<=` 更新)。显式 level 越界则夹取。

### 9.2 聚合上卷(buildSummaries)

1. 清除该层旧摘要(applyRemove + 清历史 + 分片 untrack);
2. 收集 level+1 层的**非摘要**块(分片模式先加载该层全部分片);
3. 按本层格子(中心 / 本层 cellSize)分组;
4. 每组生成摘要块:区域 = 粗格子包围盒;payload =
   `"N 个子块, 主要语义 X, 平均置信度 Y"`(X = 众数负载);confidence =
   均值;state = 多数(并列取枚举序小者);timestamp = 子块最大;
   `isSummary=true, sourceLevel=level+1`;走正常 put 路径(持久化/状态机/
   版本链一致)。

### 9.3 先粗后细(queryCoarseToFine)

先查 level 0,对每个命中块求「其区域 ∩ 查询区域」,在该子区域逐级下钻
更细层级,合并返回。`isRefined`/`hasFinerData` 按「更细层级是否存在相交
数据」判定(分片模式先按分片包围盒粗判再加载)。

## 10. 跨源实例关联

实现:`src/dynamic/dynamic_layer.cpp::reportAuto`。

同一物体匹配条件(全部满足):类别一致 + bounds 相交 +
`|now - lastSeen| <= assocWindowMs(默认 2000)`;匹配到即追加进该实例
(来源入 sources 集合),否则新建。速度估计:上报零向量且有历史点时,
`v = (pos - prevPos) / (dt 秒)`;`dt <= 0` 保持零向量。

状态推进:`update(now)`:Active 且近零速度(模长 < 1e-6)持续超过
`stationaryTimeoutMs` → Stationary;任何活跃实例 `now - lastSeen >=
archiveTimeoutMs` → Archived(移出活跃集,轨迹保留);Archived 被同 id
上报时复活为 Active。

## 11. 二进制帧格式与 CRC32 容错重放

实现:`src/persistence/persistence.cpp`。

- 帧:`{type u8, len u32, payload, crc32 u32}`,CRC 覆盖 type+len+payload;
  表驱动 CRC32(多项式 0xEDB88320,编译期建表)。
- 快照/分片/dynamic.stmb/manifest 用「临时文件 + rename」原子写,
  读到的文件要么完整要么不存在;
- 重放解析:文件头 magic/formatVersion 错误 → 判定损坏(返回错误);
  帧 CRC 不匹配或剩余字节不足一帧 → 停止解析,**保留已验证前缀**(模拟
  crash 写一半);WAL 接受前缀,快照类文件要求完整(否则判损坏)。
