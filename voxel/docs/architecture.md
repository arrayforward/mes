# stmb 架构文档

> 本文目的与读者对象:面向需要改动或扩展 stmb 的开发者。描述九个库的
> 职责与依赖关系、分层视图、持久化架构(文件布局与恢复流程)与线程模型。
> 设计动机见 [design.md](design.md),调用级细节见 [api.md](api.md)。

## 1. 模块清单与职责

| 模块(库) | 目录 | 职责 | 依赖 |
| --- | --- | --- | --- |
| stmb_core | src/core | 基础类型:TimeStamp/TimeRange/AABB/CellCoord/BlockKey/ShardKey/BlockState/ChangeType/Observation/PeriodicPattern/MemoryBlock;`makeBlockKey`/`makeShardKey`/`decayedConfidence`/`toString` | 无 |
| stmb_index | src/index | `SpatialGridIndex`(格子倒排粗筛)、`TemporalIndex`(时间有序索引/范围/过期) | core |
| stmb_store | src/store | `BlockStore`:块唯一数据存放点,容量上限 + 经典 LRU,自带互斥锁 | core |
| stmb_history | src/history | `VersionLog`:按块的版本链(archive/seal/historyOf/getAt/drop) | core |
| stmb_dynamic | src/dynamic | `DynamicLayer`:动态实例轨迹、跨源关联、Active/Stationary/Archived 推进 | core |
| stmb_persistence | src/persistence | `PersistenceManager`:二进制帧 + CRC32、快照/WAL、分片文件、manifest、dynamic.stmb | core/history/dynamic |
| stmb_shard | src/shard | `ShardManager`:分片清单与路由目录、驻留 LRU、pin/unpin、脏 flush、换出 | core/history/persistence |
| stmb_service | src/service | `StmbService` 门面:一致性、状态机、衰减、LOD、观测管线、动静分离接入、持久化接入 | 以上全部 |
| stmb_vql | src/vql | 迷你 JSON、`VqlEngine`、`FunctionRegistry`(toolSchemas + dispatch) | service |
| (可执行) | src/app | `stmb_demo`:15 段全特性演示 | service, vql |
| (可执行) | src/agent | `stmb_shell`:JSON Lines 协议外壳 | vql |
| (脚本) | agent/ | `deepseek_agent.py`:DeepSeek 工具调用代理(纯 stdlib) | stmb_shell |

## 2. 依赖图

```
stmb_core ◄── stmb_index ──────────────┐
stmb_core ◄── stmb_store ──────────────┤
stmb_core ◄── stmb_history ──┐         │
stmb_core ◄── stmb_dynamic ──┤         │
           (history,dynamic) ▼         │
              stmb_persistence ◄── stmb_shard
                        │              │
                        ▼              ▼
                    stmb_service ◄── stmb_vql
                        ▲              ▲
                  stmb_demo      stmb_shell
```

依赖图无环;core 不依赖任何 stmb 模块。构建系统(CMake)按
core → index/store/history/dynamic → persistence → shard → service →
vql → app/agent 的顺序 add_subdirectory。

## 3. 分层视图

### 3.1 基础层(core)

纯数据类型与纯函数,不含任何状态。关键类型:

- `TimeStamp`(int64 毫秒)、`TimeRange{start,end}`(闭区间);
- `AABB{min[3],max[3]}`(闭区域,边界相接算相交);
- `BlockKey{cellX,cellY,cellZ,timeSlot}`、`ShardKey{sx,sy,sz,tBucket,level}`;
- `MemoryBlock`:id/key/region/payload/timestamp/version/lastAccess +
  状态机字段(state/confidence/confirmations/lastUpdate/pending*) +
  LOD 字段(level/isSummary/sourceLevel/hasFinerData) +
  管线字段(windowStart/windowWeight/windowSources/pattern/suspect) +
  瞬态字段(temporarilyOccupiedBy,不落盘)。

### 3.2 数据/索引层(store / index / history / dynamic)

- **BlockStore**:`std::list<BlockId>`(前=最近)+
  `unordered_map<BlockId, Entry{block, list迭代器}>` 的经典 LRU,自带一把
  互斥锁,可独立线程安全;`put` 满员时返回被淘汰块的完整数据(服务层需要
  它同步清理索引/下沉)。
- **SpatialGridIndex**:`unordered_map<CellCoord, unordered_set<BlockId>>`
  倒排,按 AABB 覆盖的所有格子增删,queryRegion 返回候选并集(粗筛)。
- **TemporalIndex**:`std::map<TimeStamp, unordered_set<BlockId>>`,有序
  支持 lower_bound 范围扫描与前缀截断(expireBefore)。
- **VersionLog**:`unordered_map<BlockId, vector<BlockVersion>>`,版本链按
  归档顺序升序;`seal` 填当前版本 validTo;`getAt` 按
  `validFrom<=t<validTo` 回溯。
- **DynamicLayer**:活跃集 + 归档集两张哈希表;轨迹 deque 上限 64 点。

以上部件除 BlockStore 外自身不加锁,由服务层统一互斥锁保护。

### 3.3 存储调度层(persistence / shard)

见第 4 节。

### 3.4 服务层(service)

`StmbService` 是唯一业务门面,持有全部部件与配置。每个变更操作拆成
「公开方法 = 内部 apply(不写日志)+ 追加 WAL」两层,WAL 重放与正常运行
共用 apply 路径。它还负责:

- 状态机迁移(confirm/reportChange)与加权确认(SourceRegistry);
- 观测管线(submitObservation 决策树,见 [algorithms.md](algorithms.md));
- LOD:每层级一套 SpatialGridIndex,单尺度时回退到兼容路径 spatial_;
- 分片模式下的 ensureLoaded/pin/unpin/换出编排骨架(经 ShardManager 回调
  onLoad/onMerge/onUnload 与自身解耦)。

### 3.5 接口层(vql / app / agent)

- `VqlEngine`:词法(数字/单引号字符串/标识符/括号逗号)+ 递归下降语法
  + 执行,错误位置化;
- `FunctionRegistry`:8 个工具的 schema 与 dispatch,统一
  `{ok:true,data}` / `{ok:false,error:{code,message}}`;
- `stmb_shell`:把二者封装为 stdin/stdout JSON Lines 协议,一个服务实例
  贯穿会话;
- `deepseek_agent.py`:urllib 调 DeepSeek chat/completions,工具调用循环
  驱动 shell。

## 4. 持久化架构

### 4.1 单文件模式(shardCellSize = 0)

```
dataDir/
├── snapshot.stmb   文件头 + SnapBlock 帧*(全量块)+ SnapVersion 帧*
│                   + SnapInstance 帧*(动态实例)+ SnapSource 帧*(来源注册表,v6)
│                   + SnapshotEnd{nextId, instanceNextId}
└── wal.log         文件头 + 操作帧*(PutBlock/Confirm/Observe/RegisterSource/
                                       ReportChange/Remove/ExpireBefore)
```

### 4.2 分片模式(shardCellSize > 0)

```
dataDir/
├── manifest.stmb   ManifestMeta(全局参数+nextId+maxBlockRadius+levelCount)
│                   + ManifestShard*(ShardKey+块 id 列表+版本数)
│                   + SnapSource 帧*(来源注册表,v6)+ 结束帧
├── shards/<level>_<sx>_<sy>_<sz>_<tb>.stmb
│                   每分片自包含:SnapBlock* + SnapVersion* + 结束帧
├── dynamic.stmb    动态层(实例位置漂移,不归属固定分片)
└── wal.log         操作帧携带 ShardKey,重放时按需加载对应分片
```

二进制格式:文件头 `magic u32('STMB') + formatVersion u32`;帧
`{type u8, len u32, payload, crc32 u32}`(CRC 覆盖 type+len+payload);
整数小端、字符串 `len+bytes`、double 按位写、optional 1 字节标志。
formatVersion 当前为 **6**(各代不兼容,见 README 版本表)。

### 4.3 写入与 checkpoint

- 变更操作成功后追加 WAL(尽力而为,失败仅忽略);
- `checkpoint()`:单文件模式落全量快照并截断 WAL;分片模式 flushAll
  (每个脏分片「内存现状 ∪ 盘上旧数据(下沉块)」归并原子写盘)→ 重写
  manifest → 写 dynamic.stmb → 截断 WAL。

### 4.4 恢复流程

1. 单文件模式:loadSnapshot 重建 store/索引/历史/动态层与 nextId →
   replayWal 按序重放增量操作;
2. 分片模式:loadManifest 重建分片清单与 id→分片路由目录(校验分片参数
   与 levelCount,不一致抛异常)→ replayWal 按记录携带的 ShardKey 先
   ensureLoaded 再重放;
3. 容错:magic/version 错误抛 `std::runtime_error`;CRC 错误/截断尾部
   丢弃、保留已验证前缀。

### 4.5 换出一致性不变式

- 块在 store ⇒ 其分片已加载;分片换出 ⇒ 其全部块从 store/索引移除、
  历史从内存清除(已随归并落盘);
- 下沉块(id 在 sunk 集合)⇒ 分片为脏;干净分片 ⇒ 盘上文件即最新;
- 查询触达的分片在查询期间被 pin,换出器跳过被 pin 分片,查询结束 unpin
  后 trimToLimit 收缩回上限。

## 5. 线程模型

- **服务层一把 `std::mutex`** 串行化所有公开方法(简单优先);
- BlockStore 内部另有一把锁,使其可脱离服务独立安全使用;两层锁不会
  死锁(服务层持锁期间不回调外部代码);
- 其余部件(index/history/shard/dynamic)自身无锁,均由服务层锁保护;
- stmb_shell 单线程按行处理;deepseek_agent.py 与 shell 之间是管道上的
  同步请求-响应,无并发。
