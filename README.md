# mechanical-heart — 制造业中心心跳模块

> **定位**: 面向单个制造单位的**中心心跳模块**,按统一节拍驱动观测采集、状态推进、语义理解与时空记忆,
> 让一条业务从"原料入场"走到"成品出库"的每一步都可记录、可回溯、可仲裁。
>
> **架构底本**: 《世界模型与世界操作系统架构说明书 v2》(详见 `docs/`)。本仓库是其在
> 制造业场景下的落地实现 —— 把"四棵数据结构底座 + 三大业务逻辑"收敛为一套 C++ 静态库
> 集群与一组演示程序,作为制造单元级业务推进的运行时心脏。

---

## 1. 心跳模型

`mechanical-heart` 的"心跳"不是单纯定时器,而是一拍一拍推进业务的循环:

```
一拍 tick = 0..N ms
  ├─ 感知: 采集观测(传感器 / SCADA / 上游工位信号)
  ├─ 实体化: entity::Resolver 把观测归并到当前制造对象
  ├─ 空间化: geocore 把对象落入坐标系锚点(AABB / 工位 / 产线)
  ├─ 语义化: knowledge::Engine 推断"这是什么工序 / 处于什么阶段 / 下一步应做什么"
  ├─ 存证: storage::EventStore append 一条不可改因果事件;voxel 写入时空记忆块
  └─ 仲裁: 变更走观测管线(变化分类 + 多源加权),冲突不污染下游
```

每一拍产出的事件/块/实体/概念绑定,都进入统一的存储层,作为下一拍的输入与历史审计
依据。整条产线由同一个心跳源驱动,确保"同一时刻看到的同一物体"在所有数据结构中是
一致的。

---

## 2. 子模块清单(本仓库统一管理)

```
mechanical-heart/
├── docs/                  架构说明书与 entity 子系统文档
├── entity/                实体搜索树算法层(从观测流"浮现"实体)
├── geocore/               空间几何内核(多尺度嵌套参考系 + 锚点路径码)
├── knowledge/             语义图谱树(本体 / 推理 / 词汇-联想)
├── storage/               统一数据存储层(EventStore / VoxelStore / EntityStore)
├── voxel/                 时空记忆块服务(stmb:4D 可回溯结构化记忆)
└── bench/                 心跳 tick 基准测试(五阶段循环的可执行版本)
```

| 子模块 | 在心跳中的角色 | 核心产出 | 子系统 README |
|---|---|---|---|
| **entity** | 浮现实体 | `entity::Resolver`: 浮现→合并/新建/存疑→拆分→检索排序,带半衰期衰减、轨迹连续性校验(vmax) | [entity/README.md](entity/README.md) |
| **geocore** | 空间锚定 | `Kernel`: 多尺度嵌套坐标系 + 路径码纯函数 + 跨场景换算 + 半径近邻 | [geocore/README.md](geocore/README.md) |
| **knowledge** | 语义理解 | `Engine`: 本体推理 + 符号扩展 + 关键词图扩散 + 八步摄入管道 | [knowledge/README.md](knowledge/README.md) |
| **storage** | 持久化底座 | `RecordBackend` SPI + EventStore / VoxelStore / EntityStore 三个领域模型 | [storage/README.md](storage/README.md) |
| **voxel** | 时空记忆 | `StmbService`: 块状态机 + 置信度衰减 + 版本回溯 + LOD + 动静分离 + 仲裁管线 | [voxel/README.md](voxel/README.md) |
| **bench** | 心跳基准 | `tick_bench`: 五阶段串行计时 + 分位数统计 + memory/sqlite 双口径 | [bench/README.md](bench/README.md) |

依赖方向(无环):

```
entity ─┬─→ storage
        └─→ geocore

knowledge ──→ storage            (只读消费事件侧写)

voxel    ──→ storage             (桥接 et_sources)

外部应用 / 大模型 ──→ 各模块公开 API
```

---

## 3. 与世界模型架构的映射

| 架构说明书 v2 § | 对应子模块 |
|---|---|
| §4.1 语义图谱树(Ontology Tree) | `knowledge` |
| §4.2 实体搜索树(Entity Tree) | `entity`(算法) + `storage/entitytree`(存储) |
| §4.3 时空索引树(Spatiotemporal Tree) | `geocore`(内核) + `voxel`(4D 记忆) |
| §4.4 事件因果树(Causal Event Tree) | `storage/eventstore`(append-only 侧写) |
| §5 业务逻辑(规则/意图/价值) | 留作业务侧扩展,本仓库提供算法 + 存储底座 |

完整架构请阅读 [`docs/世界模型与世界操作系统架构说明书_v2.md`](docs/世界模型与世界操作系统架构说明书_v2.md)。

---

## 4. 构建

通用依赖:CMake ≥ 3.20、C++20 工具链(MSVC ≥ 2019 / MinGW g++ ≥ 13 / clang ≥ 10 / g++ ≥ 9)、Ninja。
无第三方在线依赖(nlohmann/json 与 sqlite3 已在 `storage/third_party/` 内置)。

```bash
# 在仓库根目录
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

各子模块也可独立构建(子模块 README 给出了具体命令与依赖),路径可用
`-DENTITY_STORAGE_DIR=` / `-DENTITY_GEOCORE_DIR=` 等覆盖。

Windows MinGW 用户:`geocore` 与 `voxel` 已在 MinGW g++ 13 + Ninja 下验证全绿
(`build-mingw/`);`knowledge` 通过 `cmake/ame_mingw_compat.h` 解决 ame vendored
代码的 MinGW 兼容问题。

---

## 5. 心跳使用样例

把一帧观测推进到所有数据结构,只需依次调用五个子模块的入口:

```cpp
#include "entity/resolver.h"
#include "geocore/kernel.hpp"
#include "knowledge/engine.h"
#include "eventstore/backends/sql_store.h"
#include "voxel/voxel_store.h"

// 1) 存储初始化(同一库,四个领域模型共享)
eventstore::SqlStore evt_store("plant.db");
voxelstore::SqlStore vox_store("plant.db");

// 2) 浮现实体
entity::Resolver resolver;
auto entity_view = resolver.resolve(observation);   // 合并/新建/存疑

// 3) 空间锚定
geocore::Kernel geo;
auto anchor = geo.Save(scene, {x, y, z}, {}, 1.0);
geo.Transfer(entity_view->object, scene_anchor, t);

// 4) 语义推断
knowledge::Engine eng;
eng.define_ontology("assets/ontology_seed.json");
auto chain = eng.understand(profile_id);             // 工序分类 + 规则推理

// 5) 时空存证
vox_store.upsert_block(anchor, block_data);
evt_store.append_event(case_event, "CNC_START", entity_view, t);

// ── 下一拍 tick 开始 ──
```

更多 API 参考与场景示例见各子模块 README 与 `docs/`。

---

## 6. 性能基准

`bench/tick_bench` 是 §1 心跳模型的可执行基准(五阶段串行 + 分位数统计 +
memory/sqlite 双口径):

```bash
cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build bench/build -j
./bench/build/tick_bench --ticks=2000 --warmup=100 --backend=memory --full-lexicon
```

头部结论(Intel Ultra 5 125H / MinGW g++ 15.2 / Release,2000 拍):

| 配置 | 整拍 avg | p99 | 吞吐 |
|---|---|---|---|
| memory + 种子词典 | 845 µs | 1722 µs | 1184 ticks/s |
| memory + 全量词典(66,628 条) | 1176 µs | 2720 µs | 850 ticks/s |
| sqlite(任意词典) | ~97 ms | — | 10 ticks/s |

- knowledge 按拍推理 `understand()` ≈ 300~460 µs;全量词典灌库 861 ms 一次性预热。
- sqlite 后端瓶颈已定位: `sqlite_backend.cpp` 缺 PRAGMA(每条写一次 fsync),P0 待修。
- 完整数据、诊断与结论见 [bench/docs/testing.md](bench/docs/testing.md)。

---

## 7. 仓库约定

- 全部子模块已统一在主仓库管理(已移除原内嵌 git 信息与子模块拆分)。
- 代码与文档以中文为主,API 命名遵循现有各模块风格(英文)。
- 新增业务模块应挂在 `entity` / `knowledge` / `voxel` / `storage` 四个底座之上,不绕过
  `storage` 直接写自有持久化,以保留"四棵树一份数据"的可审计性。

---

## 8. 许可

各子模块遵循各自许可(详见子模块根目录 `LICENSE`)。架构说明书采用项目内文档许可。