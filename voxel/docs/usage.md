# stmb 使用方法

> 本文目的与读者对象:面向第一次集成 stmb 的开发者。提供构建方法、
> ServiceConfig 配置项全表、按场景组织的最小代码示例、CTest 使用与
> 大模型接入端到端教程。API 签名细节见 [api.md](api.md)。

## 1. 构建

要求:CMake ≥ 3.16,支持 C++17 的编译器(Linux/WSL GCC、MSVC 均可)。

```bash
wsl -e bash -lc "cd /mnt/d/agent/voxel && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
```

WSL 注意事项:

- Windows 工程目录 `D:/agent/voxel` 在 WSL 中映射为 `/mnt/d/agent/voxel`;
- 构建在 WSL 内完成(GCC),可执行文件为 ELF,须用 WSL 运行;
- 单元测试用 assert 验证,测试源文件顶部 `#undef NDEBUG` 保证 Release
  构建(默认定义 NDEBUG)下断言仍生效——自行编写测试时沿用这一约定。

产物:`build/src/app/stmb_demo`、`build/src/agent/stmb_shell`、
`build/tests/test_*`。

## 2. ServiceConfig 配置项全表

`src/service/stmb_service.h`,聚合初始化按声明顺序传参:

| # | 字段 | 类型 | 默认 | 含义 |
| --- | --- | --- | --- | --- |
| 1 | capacity | std::size_t | 0 | 内存块容量上限(块级 LRU) |
| 2 | cellSize | double | 1.0 | 空间格子边长(单尺度/最细层) |
| 3 | timeSlotMs | TimeStamp | 1 | 时间槽宽度(BlockKey 用,毫秒) |
| 4 | confirmThreshold | double | 1.0 | Pending→Stable / 候选生效的加权确认阈值 |
| 5 | decayHalfLifeMs | std::int64_t | 0 | 置信度半衰期;0 = 不衰减 |
| 6 | dataDir | std::string | 空 | 持久化数据目录;空 = 关闭持久化 |
| 7 | shardCellSize | double | 0.0 | 空间分片边长;0 = 单文件模式 |
| 8 | shardTimeSpanMs | TimeStamp | 0 | 分片时间桶跨度(毫秒) |
| 9 | maxLoadedShards | std::size_t | 8 | 内存驻留分片上限 |
| 10 | levelCellSizes | std::vector\<double\> | 空 | LOD 层级格子尺寸(粗→细);空 = 单尺度 |
| 11 | stationaryTimeoutMs | std::int64_t | 0 | 动态层静止超时(毫秒) |
| 12 | archiveTimeoutMs | std::int64_t | 0 | 动态层归档超时;0 = 禁用动态层 |
| 13 | observationWindowMs | std::int64_t | 0 | 观察窗口;0 = 候选不自动超时 |

常用组合:

```cpp
// 纯内存
stmb::ServiceConfig{1024, 10.0, 1000, 2.0, 0, "", 0.0, 0, 8, {}, 0, 0, 0}
// 单文件持久化
stmb::ServiceConfig{1024, 10.0, 1000, 2.0, 0, "/tmp/stmb_a", 0.0, 0, 8, {}, 0, 0, 0}
// 分片海量
stmb::ServiceConfig{5000, 10.0, 1000, 1.0, 0, "/tmp/stmb_b", 100.0, 10000, 4, {}, 0, 0, 0}
// LOD 三级金字塔
stmb::ServiceConfig{5000, 1.0, 1000, 1.0, 0, "", 0.0, 0, 8, {100.0, 10.0, 1.0}, 0, 0, 0}
// 观测管线(窗口 5s)
stmb::ServiceConfig{100, 10.0, 1000, 2.0, 0, "", 0.0, 0, 8, {}, 0, 0, 5000}
```

## 3. 场景示例

### 3.1 基础读写与时空查询

```cpp
stmb::StmbService svc(stmb::ServiceConfig{1024, 10.0, 1000, 1.0, 0, "",
                                          0.0, 0, 8, {}, 0, 0, 0});
stmb::MemoryBlock b;
b.region = stmb::AABB{{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}};
b.payload = "house";
b.timestamp = 1000;
svc.put(b);                                   // id = 1
auto hits = svc.query(stmb::AABB{{0.0, 0.0, 0.0}, {20.0, 20.0, 20.0}},
                      stmb::TimeRange{500, 1500});   // 命中 id1
```

### 3.2 状态机与版本回溯

```cpp
svc.confirm(1, 1100);                         // -> Stable(阈值 1.0)
svc.reportChange(1, "shop", 0.9, 1500);       // -> Changing(旧数据仍生效)
svc.confirm(1, 1600);                         // -> Stable v2
svc.historyOf(1);                             // [v1 house, v2 shop]
svc.getAt(1, 1000);                           // 回溯:v1 "house"
```

### 3.3 置信度衰减

```cpp
stmb::StmbService svc(stmb::ServiceConfig{16, 10.0, 1000, 1.0, 1000, "",
                                          0.0, 0, 8, {}, 0, 0, 0});
// 半衰期 1000ms:put 时 confidence=0.8,lastUpdate=0
// svc.get(1, 2000)->confidence == 0.2;svc.get(1)->confidence 仍为 0.8
```

### 3.4 分片海量写入与重启恢复

```cpp
const stmb::ServiceConfig cfg{5000, 10.0, 1000, 1.0, 0, "/tmp/stmb_shards",
                              100.0, 10000, 4, {}, 0, 0, 0};
{
    stmb::StmbService svc(cfg);
    for (int i = 0; i < 5000; ++i) { /* 跨分片 put */ }
    svc.checkpoint();                         // flush 分片 + manifest + 截断 WAL
}
stmb::StmbService restored(cfg);              // manifest 恢复,分片延迟加载
// restored.stats().loadedShards == 0;查询触达时自动加载,常驻 <= 4
```

### 3.5 LOD 金字塔与先粗后细

```cpp
stmb::StmbService svc(stmb::ServiceConfig{5000, 1.0, 1000, 1.0, 0, "",
                                          0.0, 0, 8, {100.0, 10.0, 1.0}, 0, 0, 0});
// 写粗骨架(level 0,大区域)与细细节(level 2,小区域)
svc.buildSummaries(1);                        // 细层上卷为摘要块
auto all = svc.queryCoarseToFine(region, range);  // 骨架 + 命中区域下钻细节
bool refined = svc.isRefined(region, 0);      // 该区域是否已有细层数据
```

### 3.6 动态层:轨迹上报与静止沉淀

```cpp
stmb::StmbService svc(stmb::ServiceConfig{100, 10.0, 1000, 1.0, 0, "",
                                          0.0, 0, 8, {}, 1500, 60000, 0});
svc.reportMoving(1, "car", stmb::AABB{{19, 4, 4}, {21, 6, 6}}, {0, 0, 0}, 0, 1);
svc.reportMoving(1, "car", stmb::AABB{{14, 4, 4}, {16, 6, 6}}, {0, 0, 0}, 1000, 1);
// 速度按相邻轨迹点差分估计;trajectoryOf(1) 查看轨迹
svc.updateDynamic(4500);                      // 静止超时 -> Stationary
auto ids = svc.promoteStationary(5000);       // 沉淀为静态层 Pending 块
svc.confirm(ids[0], 6000);                    // -> Stable
```

### 3.7 观测管线:多源仲裁

```cpp
stmb::StmbService svc(stmb::ServiceConfig{100, 10.0, 1000, 2.0, 0, "",
                                          0.0, 0, 8, {}, 0, 0, 5000});
svc.registerSource(1, 0.9);                   // 高可靠
svc.registerSource(2, 0.4);                   // 低可靠
// put "meadow" 并 confirm 到 Stable 后:
auto r1 = svc.submitObservation(1, stmb::Observation{"shop", 0.9, 2, 1000});
// r1.type == Mutation, accepted == false(0.4 < 2.0,挂起进窗口)
auto r2 = svc.submitObservation(1, stmb::Observation{"shop", 0.9, 1, 1500});
// 0.4+0.9=1.3 仍挂起;再来一次高可靠 -> 2.2 >= 2.0,变更生效
svc.sweepWindows(10000);                      // 超时丢弃未达阈值候选
```

### 3.8 持久化恢复(单文件模式)

```cpp
const stmb::ServiceConfig cfg{100, 10.0, 1000, 2.0, 0, "/tmp/stmb_p",
                              0.0, 0, 8, {}, 0, 0, 5000};
{
    stmb::StmbService a(cfg);
    // 写入 / 状态机 / 观测管线操作...
    a.checkpoint();
}                                             // 析构
stmb::StmbService b(cfg);                     // 快照 + WAL 恢复,状态延续
```

### 3.9 来源注册表持久化(v6)与 et_sources 双向桥

**落盘(v6 起,默认行为)**:`registerSource` / 权重变更会追加
RegisterSource WAL 记录;`checkpoint()` 把注册表写进快照(单文件模式)
或 manifest(分片模式)的 SnapSource 小节。重启后快照还原 + WAL 回放,
来源权重完整保留,不再需要重启后重新 `registerSource`;未知来源仍默认
0.5,运行时语义不变。

**双向桥(可选,`STMB_WITH_UNISTORE=ON`)**:把 stmb 来源权重与
entitytree 的 `et_sources` 表(D:\manufacture\storage 第 7 张表)双向
同步——entity 组件反馈闭环(merge +0.02 / split −0.05)写的值,stmb
启动时读回;stmb `registerSource` 写的值,entity 侧可查。

```bash
cmake -S . -B build-bridge -G Ninja -DSTMB_WITH_UNISTORE=ON \
      -DUNISTORE_DIR=D:/manufacture/storage   # 默认即该路径,可覆盖
```

```cpp
#include "unistore_source_bridge.h"
#include "entitytree/backends/sql_store.h"

stmb::StmbService svc(cfg);                          // stmb 服务
entitytree::SqlEntityStore estore("world.db");       // 与 entity 组件同库文件
stmb::UnistoreSourceBridge bridge(svc, estore);      // 构造即水合(表 -> stmb)
                                                     // 并安装变更钩子(stmb -> 表)
svc.registerSource(7, 0.9);   // estore.get_source("7")->reliability == 0.9
// 重启后:表中 entity 侧写过的权重经水合进入 svc.reliabilityOf
```

语义与冲突约定:

- **以表为准**:水合时表中已有的数值 id 以表值覆盖 stmb 内存值
  (entity 反馈闭环是最新证据);
- **id 映射**:stmb `SourceId` 为 u64,`et_sources.source_id` 为字符串,
  桥按十进制互转;表中的非数值 id(如 "cam1")水合时**跳过**
  (该来源在 stmb 侧维持未知默认 0.5)——共享部署请统一用数值来源 id;
- **默认值差异**:stmb 未知来源默认 0.5,entitytree 未注册默认 1.0——
  桥只同步显式注册过的 id,不动默认值;
- **sqlite 文件锁**:两进程共库时避免同时写——实操上让 stmb 与 entity
  组件错开写窗口(单写者原则),或同进程内共用一个实体存储实例;
  高并发场景改用 mysql/pg 后端。

## 4. CTest 使用

```bash
wsl -e bash -lc "cd /mnt/d/agent/voxel && ctest --test-dir build --output-on-failure"
wsl -e bash -lc "cd /mnt/d/agent/voxel && ctest --test-dir build -R test_shard --output-on-failure"   # 只跑某个
wsl -e bash -lc "cd /mnt/d/agent/voxel && ./build/tests/test_e2e"                                      # 直接跑二进制看 [PASS] 明细
```

全部 14 个测试离线运行(不访问网络、不需要 API key);测试数据目录在
`/tmp/stmb_tests/` 下,每个用例自清理。

## 5. 大模型接入教程(端到端)

1. 构建(见第 1 节),确认 `build/src/agent/stmb_shell` 存在;
2. 导出 key(**只走环境变量,不要写进任何文件**):
   `export DEEPSEEK_API_KEY=sk-...`;
3. 冒烟 shell 协议(可选):
   ```bash
   wsl -e bash -lc "cd /mnt/d/agent/voxel && echo '{\"cmd\":\"schemas\"}' | ./build/src/agent/stmb_shell 2>/dev/null | head -c 200"
   ```
4. 启动联调(单轮):
   ```bash
   wsl -e bash -lc "cd /mnt/d/agent/voxel && python3 agent/deepseek_agent.py --query '在区域 [0,0,0,10,10,10] 写入一座房子,时间戳 1000,然后查一下里面有什么'"
   ```
   stderr 可看到 `[tool] stmb_put ...` / `[tool] <= {...}` 的调用过程;
5. 进入 REPL 持续对话(状态在 shell 会话内累积):
   ```bash
   wsl -e bash -lc "cd /mnt/d/agent/voxel && python3 agent/deepseek_agent.py"
   ```
   可连续提问:"写一座房子"→"把它改成商店"→"它现在是什么?历史上改过
   几次?"→"1000 时刻它是什么"(时间回溯);
6. 需要重启不丢数据:加 `--data /tmp/stmb_llm`;需要分片:再加
   `--shard-cell-size 100 --shard-time-span 10000`;
7. 换 OpenAI 兼容端点:`--base-url https://your-endpoint/v1 --model <name>`。

联调排查:shell 日志在 stderr;API HTTP 错误会打印响应体;
`--shell` 可指定其它路径的 stmb_shell。
