# bench 测试文档

## 1. 测试环境

| 项 | 值 |
|---|---|
| CPU | Intel Core Ultra 5 125H |
| 内存 | 16 GB |
| OS | Windows（win32），MinGW-W64 g++ 15.2.0 (ucrt-posix-seh) |
| CMake | 4.2.3，Ninja 生成器，`-DCMAKE_BUILD_TYPE=Release` |
| 被测代码 | mechanical-heart master（storage/geocore/entity/knowledge 源码树引入） |

## 2. 测试方法

- 每配置先跑 `--warmup` 预热拍（不计入），再跑 `--ticks` 有效拍。
- 计时：`steady_clock` 微秒级，每阶段独立取点；统计 min/avg/p50/p95/p99/max。
- knowledge 装配（ontology/种子词典/全量词典）在跑拍前完成，单独报告、不计入 tick。
- sqlite 口径：三个独立临时库文件，跑后自动删除。
- 全部测试离线，无外部服务。

## 3. 结果

### 3.1 memory 后端 + 种子词典（knowledge on）

命令：`tick_bench --ticks=2000 --warmup=100 --backend=memory`

| 阶段 | avg | p50 | p95 | p99 | max | min |
|---|---|---|---|---|---|---|
| entity | 483.70 | 471.40 | 856.60 | 968.90 | 1113.40 | 87.80 |
| geocore | 4.00 | 3.70 | 5.10 | 7.60 | 147.20 | 2.30 |
| eventstore | 44.25 | 41.80 | 57.80 | 72.80 | 148.10 | 33.20 |
| voxelstore | 13.57 | 13.30 | 19.30 | 23.90 | 97.20 | 7.90 |
| knowledge | 298.97 | 278.70 | 568.30 | 664.70 | 1572.90 | 67.50 |
| **整拍** | **844.55** | **823.50** | **1488.80** | **1722.00** | **2544.40** | **205.00** |

**吞吐：1184 ticks/s。** 装配：define_ontology 13.0ms + apply_lexicon 10.6ms。

### 3.2 memory 后端 + 全量词典（--full-lexicon，66,628 条）

命令：`tick_bench --ticks=2000 --warmup=100 --backend=memory --full-lexicon`

| 阶段 | avg | p50 | p95 | p99 | max | min |
|---|---|---|---|---|---|---|
| entity | 641.95 | 622.00 | 1157.10 | 1443.90 | 2291.20 | 96.40 |
| geocore | 6.40 | 5.60 | 10.80 | 18.00 | 189.50 | 3.00 |
| eventstore | 54.70 | 51.60 | 76.90 | 101.20 | 665.60 | 36.40 |
| voxelstore | 16.18 | 15.00 | 25.40 | 37.30 | 125.50 | 8.40 |
| knowledge | 456.78 | 443.70 | 860.20 | 1115.90 | 4227.60 | 77.60 |
| **整拍** | **1176.07** | **1140.20** | **2089.10** | **2720.30** | **5826.30** | **231.60** |

**吞吐：850 ticks/s。** 装配：14.7 + 11.1 + **861.0ms**（灌入 66,628 条）。

### 3.3 sqlite 后端 + 种子词典

命令：`tick_bench --ticks=500 --warmup=50 --backend=sqlite`

| 阶段 | avg | p50 | p99 | max |
|---|---|---|---|---|
| entity | 69537.19 | 70247.00 | 77170.60 | 81199.60 |
| geocore | 16.16 | 15.70 | 33.20 | 114.20 |
| eventstore | 18138.20 | 18059.60 | 21048.20 | 22843.60 |
| voxelstore | 8469.19 | 8240.50 | 10247.10 | 11018.10 |
| knowledge | 748.52 | 737.80 | 1143.50 | 1223.90 |
| **整拍** | **96909.32** | **97797.90** | **106798.40** | **109772.10** |

**吞吐：10 ticks/s。**

### 3.4 sqlite 后端 + 全量词典

命令：`tick_bench --ticks=300 --warmup=30 --backend=sqlite --full-lexicon`

| 阶段 | avg | p50 | p99 | max |
|---|---|---|---|---|
| entity | 70694.67 | 69380.40 | 82329.10 | 176344.80 |
| geocore | 18.57 | 17.20 | 40.30 | 93.50 |
| eventstore | 18337.03 | 17973.20 | 22289.90 | 25705.70 |
| voxelstore | 8480.18 | 8099.40 | 10741.20 | 10912.10 |
| knowledge | 820.36 | 809.70 | 1066.70 | 1113.40 |
| **整拍** | **98350.87** | **96944.00** | **115956.00** | **205952.80** |

**吞吐：10 ticks/s。** 装配：0.6 + 0.4 + 836.4ms。

## 4. 诊断

### 4.1 loop 结构本身：无性能问题

整拍 = 五阶段之和（残差 < 2%，为取点与分支开销）。循环、参数解析、
统计累计的成本在测量噪声以下。**性能问题不在 loop 结构，在阶段实现。**

### 4.2 sqlite 后端：缺 PRAGMA，每写一次 fsync

`storage/src/storage/sqlite_backend.cpp` 全文无 `PRAGMA journal_mode` /
`synchronous` 设置：每条写是独立自动提交事务，默认 `synchronous=FULL`，
每条语句一次 fsync。entity 阶段一拍多条写（观测 append + 侧写 upsert +
绑定 append + 实体 upsert），代价叠乘 → 69ms/拍。

对照关系：memory→sqlite，entity ×144、eventstore ×410、voxelstore ×624，
而 geocore（纯内存计算，×4）与 knowledge（内存图操作，×2.5）几乎不变——
证实瓶颈在存储落盘路径而非算法。

修复方向（sqlite 标准三件套，预计回到亚毫秒级）：
`PRAGMA journal_mode=WAL` + `synchronous=NORMAL` + 一拍内多写合并为单事务。

### 4.3 词典规模：scaling 良好

| 口径 | knowledge avg | 整拍 avg | 吞吐 |
|---|---|---|---|
| 种子词典 | 298.97 µs | 844.55 µs | 1184 ticks/s |
| 全量 66,628 条 | 456.78 µs | 1176.07 µs | 850 ticks/s |

词典条目增 4 个数量级，单拍推理仅 +53%：归一（哈希）与扩散（邻接表）
的索引结构对词典规模近 O(1)。一次性灌库 861ms 属启动预热，可接受。

附带效应：entity 阶段 484→642µs（+33%）为词典常驻内存后的缓存压力，
非算法退化（entity 不读词典）。

### 4.4 尾延迟形态

memory 口径 p99/avg ≈ 2~2.4：尖峰来自实体浮现拍（resolve 全路径：
blocking 召回 + 打分 + 合并 + rebuild）与 sqlite 页分配，均为低频重路径。
对 10ms~1s 的制造业典型节拍，p99 ≈ 2.7ms（全词典）余量充足。

## 5. 结论

1. **心跳 loop 可行**：完整五阶段（含 knowledge 按拍推理 + 全量词典）
   整拍 1.18ms / 850 ticks/s，距典型制造节拍（≥10ms）有一个数量级余量。
2. **knowledge 应按拍跑**：`understand()` 300~460µs，是"每拍一条侧写"
   设计的正确粒度；批量 `ingest_all` 留给补录/回填场景。
3. **生产部署必须先修 sqlite**：当前 10 ticks/s 不可用；WAL + NORMAL +
   批量事务是 P0 优化（修改点集中在 sqlite_backend.cpp 的连接初始化与
   写入路径）。
4. **下一阶段基准**：并发管线（感知/持久化并行、多工位并行采集）、
   mysql/pg 外部后端、以及 sqlite 修复后的复测。

## 6. 复现

```bash
cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build bench/build -j
./bench/build/tick_bench --ticks=2000 --warmup=100 --backend=memory --full-lexicon
```
