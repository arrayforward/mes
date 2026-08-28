# storage — 统一数据存储层

世界模型与世界操作系统的**统一数据存储子模块**：领域模型（事件树、时空块、
以及未来的任何数据结构）映射到同一套"记录"抽象上；数据库 / Redis /
文件目录各只实现一次后端，全部领域模型即自动获得全部后端。

```
领域层    EventStore（事件树：叙事/侧写图）   VoxelStore（时空记忆块）   EntityStore（实体搜索树）
             └───────────────────────────────┬───────────────────────────────┘
统一层    RecordBackend SPI（Record + 类型化字段 + 条件查询 + 受限更新）
             ┌────────┬────────┬────────┬────────┬───────┬──────┐
后端       memory   sqlite   mysql    postgres redis   file
          （参考） （穿刺） （原生库）（原生库）(自研RESP)(WAL+JSON)
```

## 文档

| 文档 | 内容 |
|---|---|
| [docs/design.md](docs/design.md) | 完整设计文档：架构、统一层语义、三个领域模型、各后端设计要点、索引策略、测试策略 |
| [docs/usage.md](docs/usage.md) | 使用文档：子模块集成、EventStore/VoxelStore/EntityStore 接口参考、后端选择、新领域模型接入指南 |

## 快速开始

```bash
cmake -B build -G Ninja
cmake --build build

./build/eventstore_tests.exe   # 事件树一致性套件（memory + sqlite + file）
./build/voxel_tests.exe        # 时空块一致性套件（memory + sqlite + file）
./build/entity_tests.exe       # 实体搜索树存储一致性套件（memory + sqlite + file）
./build/demo_story.exe         # 事件树演示（生成 demo_story.db）
./build/demo_voxel.exe         # 时空块演示（生成 demo_voxel.db）
ctest --test-dir build
```

外部服务联测（设置环境变量后自动追加对应后端的一致性测试）：

```bash
export UNISTORE_MYSQL_DSN="mysql://user:pass@127.0.0.1:3306/world"
export UNISTORE_PG_CONNINFO="host=127.0.0.1 user=postgres password=xxx dbname=world"
export UNISTORE_REDIS_ADDR="127.0.0.1:6379"
```

30 秒上手（事件树，换后端只需换头文件与类名）：

```cpp
#include "eventstore/backends/sql_store.h"
using namespace eventstore;

SqlStore store("world.db");
auto story = Narrative::create("story", "勇者斗恶龙");
store.append_narrative(story);
auto ev = Event::create(story.narrative_id, "勇者拔剑");
store.append_event(ev);
auto p = Profile::create(ev.event_id, "active", TimeRef::virtual_time("从前"),
                         "村口老树下", "勇者", "拔出", "圣剑", {},
                         {Modifier{"verb", "manner", "奋力"}});
store.append_profile(p);
store.timeline(story.narrative_id);
```

## 核心设计（详见 design.md）

- **统一层**：`Value/Record/TableSchema` + `RecordBackend` SPI；
  upsert 表（可变当前态）与 auto_seq 表（append-only）区分两类数据性质；
  可检索的语义拆独立索引表，不依赖特定数据库的 JSON 函数。
- **事件树 append-only**：侧写是原子单元（五要素 + 修饰语 + 图）；
  虚拟时间靠显式 Link 拓扑排序；人名问题用"实体 + 绑定"消解
  （重名不同人 / 同人异名 / latest-wins 全历史可审计）。
- **时空块**：块当前态可变（upsert），版本链 append-only
  （seal 关闭生效区间，`version_at` 回溯），时空联合查询全后端语义一致。
- **实体搜索树**：实体从属性观测流中"浮现"——观测（append-only）→
  时空窗口侧写（镜像永久保留）→ 实体归并视图（带版本号的缓存）；
  合并/拆分 = append-only 绑定（latest wins，全历史可审计）；
  et_sources 来源可靠性表与 stmb 共享（单库部署一份数据两方共用）。
  本层只做增删改查；浮现/合并/拆分/检索排序算法在 entity 组件。
- **索引哲学**：一次写入、多次索引——append-only 下索引只增不改，
  写入时同步维护。
