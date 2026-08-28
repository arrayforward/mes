# stmb API 参考

> 本文目的与读者对象:面向调用 stmb 的开发者与联调工程师。给出
> StmbService 全部公开 API(签名 + 语义 + 示例)、VQL 语法完整参考、
> Function Calling 工具 schema 与返回结构、stmb_shell 协议与
> deepseek_agent.py 用法。配置项全表见 [usage.md](usage.md)。

## 1. StmbService 公开 API(src/service/stmb_service.h)

命名空间 `stmb`。除标注外,所有方法线程安全(服务层统一互斥锁)。

### 1.1 构造与配置

```cpp
explicit StmbService(const ServiceConfig& config);
StmbService(std::size_t capacity, double cellSize, TimeStamp timeSlotMs);
// 便捷构造:确认阈值 1.0、不衰减、不持久化、不分片、单尺度、不动态层
```

`ServiceConfig` 字段全表见 [usage.md](usage.md#2-serviceconfig-配置项全表)。

### 1.2 写入与读取

```cpp
std::optional<MemoryBlock> put(MemoryBlock block);
```
写入块。调用方填 `region/payload/timestamp`(可选 `confidence/level`);
服务分配 id、生成 BlockKey、置 Pending、归档 v1;`level=-1` 时按 AABB
尺寸自动判定层级。触发 LRU 淘汰时返回被淘汰块。

```cpp
std::optional<MemoryBlock> get(BlockId id);
std::optional<MemoryBlock> get(BlockId id, TimeStamp now);
std::vector<MemoryBlock> query(const AABB& region, const TimeRange& range);
std::vector<MemoryBlock> query(const AABB& region, const TimeRange& range, TimeStamp now);
```
`get`/`query` 命中刷新 LRU;带 `now` 的重载在返回副本上把 `confidence`
替换为该时刻的有效(衰减后)值。`query` 缺省查最细层级。

```cpp
std::vector<MemoryBlock> queryLevel(const AABB& region, const TimeRange& range, int level);
std::vector<MemoryBlock> queryLevel(const AABB& region, const TimeRange& range,
                                    TimeStamp now, int level);
std::vector<MemoryBlock> queryAllLevels(const AABB& region, const TimeRange& range);
std::vector<MemoryBlock> queryCoarseToFine(const AABB& region, const TimeRange& range);
bool isRefined(const AABB& region, int level);
std::size_t buildSummaries(int level);
int levelCount() const;
```
LOD 查询族:指定层级 / 跨层合并 / 先粗后细下钻 / 未细化检测 /
聚合上卷 / 层级数。LOD 模式下命中块的 `hasFinerData` 指示更细层级数据。

### 1.3 状态机与观测管线

```cpp
bool confirm(BlockId id, TimeStamp now);                  // 系统自确认,权重 1.0
bool confirm(BlockId id, SourceId source, TimeStamp now); // 按来源可靠性加权
bool reportChange(BlockId id, std::string newPayload, double newConfidence, TimeStamp now);
```
`confirm`:Pending 加权累计达 `confirmThreshold` 转 Stable;Changing 确认
变更(封存旧版本、候选生效、version+1 归档);其余返回 false。
`reportChange`:仅 Stable 接受,暂存候选转 Changing,生效数据不变。

```cpp
ChangeReport submitObservation(BlockId id, const Observation& obs);
void registerSource(SourceId id, double reliability);  // v6 起随快照/WAL 落盘
double reliabilityOf(SourceId id) const;                 // 查询权重(未知默认 0.5)
void setSourceChangeHook(std::function<void(SourceId,double)> hook);  // 变更钩子(桥接用)
bool registerPattern(BlockId id, const PeriodicPattern& pattern);
std::optional<PeriodicPattern> patternOf(BlockId id) const;
std::size_t sweepWindows(TimeStamp now);
std::vector<std::string> validateSpatial(BlockId id);
void setGroundKeywords(std::vector<std::string> keywords);
```
观测管线族。`submitObservation` 返回
`ChangeReport{ChangeType type, bool accepted, std::string message}`,
分类规则见 [algorithms.md](algorithms.md#6-观测管线分类决策树)。
`Observation{std::string payload; double confidence; SourceId source; TimeStamp t;}`,
payload 空串 = 空地/消失。`sweepWindows` 丢弃超时挂起候选并恢复状态。

### 1.4 版本历史

```cpp
std::vector<BlockVersion> historyOf(BlockId id) const;
std::optional<BlockVersion> getAt(BlockId id, TimeStamp t) const;
void purgeHistory(BlockId id);
```
`BlockVersion{version, payload, confidence, state, validFrom, optional<TimeStamp> validTo}`;
`getAt` 返回 `validFrom <= t < validTo` 的版本。块消亡后历史默认保留。

### 1.5 删除 / 过期 / 统计 / 检查点

```cpp
bool remove(BlockId id);
std::vector<BlockId> expireBefore(TimeStamp ts);   // 清理 timestamp < ts 的块
ServiceStats stats() const;   // {blockCount, capacity, evictCount, loadedShards}
bool checkpoint();            // dataDir 为空返回 false
```

### 1.6 动态实例层

```cpp
InstanceId reportMoving(InstanceId id, const std::string& classLabel, const AABB& bounds,
                        const std::array<double,3>& velocity, TimeStamp now, SourceId source);
InstanceId reportMoving(const std::string& classLabel, const AABB& bounds,
                        const std::array<double,3>& velocity, TimeStamp now, SourceId source);
std::vector<DynamicInstance> queryDynamic(const AABB& region, const TimeRange& range);
std::vector<TrackPoint> trajectoryOf(InstanceId id) const;
void updateDynamic(TimeStamp now);
std::vector<BlockId> promoteStationary(TimeStamp now);
DynamicStats dynamicStats() const;  // {activeCount, stationaryCount, archivedCount}
```
按 id 上报(id=0 自动分配)或跨源自动关联;速度零向量时按相邻轨迹点
差分估计。`promoteStationary` 把 Stationary 实例沉淀为静态层 Pending 块
并归档实例。动态层启用条件:`archiveTimeoutMs > 0`。

### 1.7 调用示例

```cpp
stmb::StmbService svc(stmb::ServiceConfig{1024, 10.0, 1000, 2.0, 0, "",
                                          0.0, 0, 8, {}, 0, 0, 5000});
stmb::MemoryBlock b;
b.region = stmb::AABB{{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}};
b.payload = "house";
b.timestamp = 1000;
svc.put(b);                                  // id = 1,Pending
svc.confirm(1, 1100);                        // 权重 1.0
svc.confirm(1, 1200);                        // 达阈值 -> Stable
svc.registerSource(7, 0.9);
auto r = svc.submitObservation(1, stmb::Observation{"shop", 0.9, 7, 2000});
// r.type == ChangeType::Mutation(0.9 < 2.0,挂起进观察窗口)
auto hits = svc.query(stmb::AABB{{0.0, 0.0, 0.0}, {20.0, 20.0, 20.0}},
                      stmb::TimeRange{0, 5000});
auto past = svc.getAt(1, 1000);              // 时间回溯 v1
```

## 2. VQL 语法参考(src/vql/vql.h)

关键字大小写不敏感;数字含负/小数/指数;字符串用单引号。

```
FIND BLOCKS IN REGION(x1,y1,z1,x2,y2,z2) DURING(t1,t2)
    [LEVEL n] [STATE Pending|Stable|Changing] [PAYLOAD ~ '子串']
    [AT t] [SUMMARY] [LIMIT n]
FIND HISTORY OF <blockId>
FIND DYNAMIC IN REGION(x1,y1,z1,x2,y2,z2) DURING(t1,t2)
FIND TRAJECTORY OF <instanceId>
STATS
```

| 子句 | 语义 |
| --- | --- |
| LEVEL n | 指定 LOD 层级(缺省最细) |
| STATE ... | 对命中结果按状态后过滤 |
| PAYLOAD ~ 's' | 负载子串后过滤 |
| AT t | 时间回溯:命中块改查 t 时刻生效的版本内容(走 getAt) |
| SUMMARY | 走 queryCoarseToFine 先粗后细 |
| LIMIT n | 截断结果数量 |

执行:`VqlEngine engine(service); VqlResult r = engine.execute(sql);`
返回 `{bool ok; std::string error; JsonValue data;}`。错误为位置化文本
(「位置 N: 期望 xxx,得到 yyy」)或「语义错误: ...」(STATE 非法值、
REGION 参数个数错等)。

结果 JSON:块含 id/region[6]/payload/timestamp/state/confidence/level/
version/is_summary/suspect;实例含 id/class/position/velocity/state/
confidence/sources/trajectory_points;版本含 version/payload/confidence/
state/valid_from/valid_to;轨迹点含 t/position/velocity。

## 3. Function Calling(src/vql/functions.h)

```cpp
stmb::FunctionRegistry registry(service);
stmb::JsonValue schemas = registry.toolSchemas();   // OpenAI 风格 tools 数组
stmb::JsonValue result = registry.dispatch(name, args);
```

返回约定:成功 `{ok:true, data:...}`;失败
`{ok:false, error:{code, message}}`,code ∈
`missing_param / invalid_args / unknown_tool / not_found`。

| 工具 | 必需参数 | 可选参数 | 返回 data |
| --- | --- | --- | --- |
| stmb_query | region[6], time_range[2] | level, state, payload_contains | 块数组 |
| stmb_query_at | block_id, timestamp | — | 版本对象(时间回溯) |
| stmb_history | block_id | — | 版本数组 |
| stmb_put | region[6], payload, timestamp | confidence, level | `{block_id}` |
| stmb_observe | block_id, payload, confidence, source_id, timestamp | — | `{type, accepted, message}` |
| stmb_dynamic_query | region[6], time_range[2] | — | 实例数组 |
| stmb_stats | — | — | 统计对象 |
| stmb_checkpoint | — | — | `{success}` |

## 4. stmb_shell 协议(src/agent/shell_main.cpp)

启动:`stmb_shell [--data DIR] [--capacity N]
[--shard-cell-size X --shard-time-span MS]`。启动日志(含就绪行)走
stderr;stdout 只出协议 JSON(每行一个响应)。

| 请求(每行一个 JSON) | 响应 |
| --- | --- |
| `{"cmd":"schemas"}` | `{"ok":true,"data":<tools 数组>}` |
| `{"cmd":"vql","query":"FIND ..."}` | `{"ok":true,"data":...}` 或 `{"ok":false,"error":"位置 N: ..."}` |
| `{"cmd":"call","name":"stmb_query","arguments":{...}}` | dispatch 原样 `{ok, data|error}` |
| 坏 JSON / 缺 cmd / 未知 cmd | `{"ok":false,"error":{code,message}}` |

任何错误进程不退出,继续读下一行;stdin EOF 正常退出。一个服务实例贯穿
会话,工具调用之间状态持续累积。

## 5. deepseek_agent.py 用法(agent/deepseek_agent.py)

```bash
export DEEPSEEK_API_KEY=sk-...   # 必需,只从环境变量读
python3 agent/deepseek_agent.py --query "在区域 [0,0,0,10,10,10] 写入一座房子,时间戳 1000,然后查一下"
python3 agent/deepseek_agent.py                    # REPL,输入 quit 退出
python3 agent/deepseek_agent.py --model deepseek-v4-pro --data /tmp/stmb_llm
python3 agent/deepseek_agent.py --base-url https://your-openai-compatible/v1
```

参数:`--query`(单轮)/ `--model`(默认 deepseek-v4-flash)/
`--base-url`(默认 https://api.deepseek.com/v1)/ `--shell`(stmb_shell
路径)/ `--data` / `--capacity` / `--shard-cell-size` / `--shard-time-span`
(后四项透传给 stmb_shell)。工具调用过程(调用名 + 参数 + 结果摘要)
打印到 stderr;缺 key 时打印明确提示并以退出码 2 退出。
