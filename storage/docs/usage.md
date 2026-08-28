# storage 使用文档 —— 上层模块调用指南

> 本文面向**调用方**：世界模型 / 世界操作系统等上层系统的模块，
> 把 storage 作为子模块集成后，如何使用事件树、时空块、实体搜索树三套
> 领域接口，以及如何接入新的领域模型。设计原理见 [design.md](design.md)。

## 1. 集成

### 1.1 作为 CMake 子模块

```bash
# 在上层项目里以 submodule/vendor 方式引入
git submodule add <repo-url> third_party/storage
```

```cmake
# 上层 CMakeLists.txt
add_subdirectory(third_party/storage)
target_link_libraries(your_target PRIVATE unistore)
```

`unistore` 静态库自动携带：统一层 + 三个领域模型 + vendored sqlite3 +
nlohmann/json 头文件路径。Windows 下自动链接 ws2_32（redis 后端）。
entitytree 的算法层（entity::Resolver）在独立的 entity 组件
（`D:\manufacture\entity`），按需引入。

### 1.2 可选依赖（装了才编译对应后端）

```bash
# Linux
sudo apt install libmysqlclient-dev   # 或 libmariadb-dev
sudo apt install libpq-dev
```

CMake 选项：`-DUNISTORE_WITH_MYSQL=OFF` / `-DUNISTORE_WITH_PG=OFF` 可显式关闭。

### 1.3 Windows 运行说明

测试/demo 程序已静态链接 MinGW 运行库（`-static-libgcc -static-libstdc++`），
上层自己的可执行文件如需脱离 mingw64/bin 运行，请加上同样的链接选项。

## 2. 事件树（eventstore）快速开始

```cpp
#include "eventstore/backends/sql_store.h"   // 换成其他后端只需换头文件与类名
using namespace eventstore;

SqlStore store("world.db");                  // ":memory:" 为纯内存库

// 叙事 → 事件 → 侧写
auto story = Narrative::create("story", "勇者斗恶龙");
store.append_narrative(story);

auto ev = Event::create(story.narrative_id, "勇者拔剑");
store.append_event(ev);

// 侧写：时间/地点/谁/动词/对什么 + 载荷 + 修饰语
auto p = Profile::create(ev.event_id, "active",
                         TimeRef::virtual_time("从前"),       // 虚时间；真实时间用 TimeRef::real(iso)
                         "村口老树下", "勇者", "拔出", "圣剑",
                         {{"mood", "决绝"}},
                         {Modifier{"verb", "manner", "奋力"},
                          Modifier{"object", "quantity", "一把"},
                          Modifier{"object", "quality", "锋利无比"}});
store.append_profile(p);

// 先后关系（虚时间无全序，顺序靠 Link 表达）
store.append_link(Link{p.profile_id, "pf-另一个侧写", "before"});

// 指代消解：surface "张伟" → 具体实体（重名不同人互不干扰）
auto zw = Entity::create("张伟", "person", json::array(), {{"village", "东村"}});
store.append_entity(zw);
store.append_binding(Binding::create(p.profile_id, "subject", zw.entity_id));

// 查询
auto line = store.timeline(story.narrative_id);          // 按 Link 拓扑排序的时间线
auto hits = store.query_profiles("勇者", "拔出");         // 五条件任意组合
auto mods = store.find_profiles_by_modifier("奋力");      // 修饰语查询
auto involved = store.profiles_of_entity(zw.entity_id);   // 实体→侧写反查
```

## 3. EventStore 接口参考

### 3.1 写入（全部 append-only，无 update/delete）

| 方法 | 说明 |
|---|---|
| `append_narrative(n)` | 追加叙事。重复 id 抛 `DuplicateError` |
| `append_event(e)` | 追加事件。父叙事不存在抛 `NotFoundError` |
| `append_profile(p) → seq` | 追加侧写，返回全局自增 seq（物理追加序） |
| `append_link(l)` | 追加侧写间边：`before / causes / refines / retracts` |
| `append_entity(e)` | 追加实体（name 可重复，id 定身份） |
| `append_binding(b) → seq` | 追加指代消解记录（profile_id, slot, entity_id, confidence, note） |

### 3.2 读取

| 方法 | 说明 |
|---|---|
| `get_narrative / get_event / get_profile / get_entity(id)` | 按 id 取，不存在抛 `NotFoundError` |
| `events_of(narrative_id)` | 叙事下全部事件（插入序） |
| `profiles_of(event_id)` | 事件下全部侧写（seq 序）——同一事件的多个视角 |
| `links_from / links_to(profile_id)` | DAG 出边 / 入边 |
| `bindings_of(profile_id)` / `bindings_for(profile_id, slot)` | 消解全历史（seq 序） |
| `effective_binding(profile_id, slot) → optional` | 当前有效指代（latest wins） |

### 3.3 查询

| 方法 | 说明 |
|---|---|
| `query_profiles(subject?, verb?, perspective?, place?, object?)` | 五条件任意组合，seq 升序 |
| `find_profiles_by_modifier(text?, kind?, target?)` | 修饰语查询；条件落在**同一修饰语**上 |
| `find_entities_by_name(name)` | 按规范名/别名检索，重名全部返回 |
| `profiles_of_entity(entity_id, slot?)` | 实体→侧写反查（**有效绑定**语义：被重消歧纠正走的不再命中） |
| `timeline(narrative_id)` | 叙事时间线：before/causes 边拓扑排序（seq 兜底），有环抛 `EventStoreError` |

### 3.4 数据类工厂

```cpp
Narrative::create(kind /*story|fact|scenario*/, title, meta = {})
Event::create(narrative_id, summary, meta = {})
Profile::create(event_id, perspective, TimeRef, place, subject, verb, object,
                payload = {}, modifiers = {})
TimeRef::real(iso_timestamp)
TimeRef::virtual_time(label, ordinal = nullopt)
Entity::create(name, type /*person|place|org|thing|...*/, aliases = {}, attributes = {})
Binding::create(profile_id, slot /*subject|object|place*/, entity_id,
                confidence = 1.0, note = "")
```

错误类型（`eventstore/errors.h`）：`EventStoreError`（基类）、
`NotFoundError`、`DuplicateError`。

## 4. VoxelStore 接口参考（时空记忆块）

```cpp
#include "voxelstore/backends/sql_voxel_store.h"
using namespace voxelstore;

SqlVoxelStore store("voxel.db");

VoxelBlock b;
b.id = 1;
b.region = Aabb{0, 0, 0, 10, 10, 10};   // min_x,min_y,min_z,max_x,max_y,max_z
b.payload = "一座房子";
b.timestamp = 1000;
b.version = 1;
b.state = "Stable";                      // Pending | Stable | Changing
store.put_block(b);                      // upsert：当前态可变

// 版本链：变更时先 seal 旧版本，再归档新版本
store.append_version(VoxelVersion{1, 1, "一座房子", 0.95, "Stable", 1000, std::nullopt});
store.seal_version(1, 2000);
store.append_version(VoxelVersion{1, 2, "一座两层房子", 0.97, "Stable", 2000, std::nullopt});

store.query_blocks(Aabb{0,0,0,15,15,15}, 0, 2500, std::nullopt);  // 时空联合
store.version_at(1, 1500);               // 回溯："t=1500 时这里是什么"
```

| 方法 | 说明 |
|---|---|
| `put_block(b)` / `get_block(id)` / `remove_block(id)` / `all_blocks()` | 块当前态（upsert；删除不影响版本历史） |
| `query_blocks(region?, time_from?, time_to?, level?)` | AABB 相交 × 时间闭区间 × LOD 层级 |
| `append_version(v)` / `seal_version(id, valid_to)` / `versions_of(id)` | 版本链（append-only + 关闭区间） |
| `version_at(id, t) → optional` | 时间回溯（`valid_from <= t < valid_to`），基类通用实现 |
| `put_instance(i)` / `get_instance(id)` / `all_instances()` | 动态实例（类别/包围盒/轨迹/来源/状态机） |
| `set_meta(k, v)` / `get_meta(k, default)` | id 发生器与全局参数 |

`VoxelBlock` 字段与 stmb `MemoryBlock` 落盘字段一一对应
（含观测管线窗口 `window_*`、周期模式 `pattern`、LOD `level/is_summary/
source_level`、空间一致性标记 `suspect`）。

## 5. EntityStore 接口参考（实体搜索树，纯存储层）

实体从属性观测流中"浮现"：storage 只管观测/侧写/实体/绑定/来源五类记录的
落地与检索原语；浮现、合并、拆分、检索排序、时效衰减、来源可靠性闭环等
算法全部在 **entity 组件**（`D:\manufacture\entity`，`entity::Resolver`，
参数与用法见其 README）。

```cpp
#include "entitytree/backends/sql_store.h"
using namespace entitytree;

SqlEntityStore store("entity.db");       // 与 voxelstore 指同一文件即共享 et_sources

// 观测（append-only）→ 侧写（upsert）→ 实体（upsert）→ 绑定（append-only）
store.append_observation(Observation{"ob-1", "plate_number", "沪A12345",
                                     0.95, "cam1", "gate-1", 100, "", 0});
AttrProfile p;  p.profile_id = "ap-1";  p.anchor_ref = "gate-1";
p.attributes["plate_number"] = json::array(
    {json{{"value", "沪A12345"}, {"confidence", 0.95}, {"source_id", "cam1"},
          {"obs_seq", 1}, {"ts", 100}}});
store.upsert_profile(p);
EntityNode e;  e.entity_id = "en-1";  // credibility 为基准值，updated_at 为基准时间
store.upsert_entity(e);
store.append_binding(EntityBinding{"bd-1", "ap-1", "en-1", 1.0, "merge", "", 0});
store.upsert_source(SourceRecord{"cam1", 0.9, 100, json::object()});
```

| 方法 | 说明 |
|---|---|
| `append_observation(o) → seq` / `get_observation(id)` | 观测（append-only，重复 id 抛 `DuplicateError`） |
| `observations_of(anchor_ref)` / `query_observations(key?, source?, anchor?)` | 观测索引查询（seq 升序） |
| `upsert_profile(p)` / `get_profile(id)` | 侧写当前态（镜像永久保留；不存在抛 `NotFoundError`） |
| `query_profiles(anchor?, bucket_from?, bucket_to?, status?, level?)` | 时空窗口 × 状态 × 层级（时间桶分辨率，0=最细）组合查询 |
| `find_profiles_by_attribute(key, value)` | 侧写属性召回（走 et_profile_attrs 等值查询） |
| `upsert_entity(e)` / `get_entity(id)` / `find_entities_by_attribute(key, value)` | 实体当前态与属性召回；`credibility` 存基准值、`updated_at` 存基准时间（衰减在查询时由 entity 组件计算，存储不改写） |
| `append_binding(b) → seq` | 绑定（append-only；引用的侧写/实体不存在抛 `NotFoundError`） |
| `bindings_of(profile_id)` / `effective_binding(profile_id)` | 绑定全历史 / 有效绑定（latest wins） |
| `profiles_of_entity(entity_id)` | 实体→侧写反查（**有效绑定**语义，被 split 纠正走的不再命中） |
| `upsert_source(s)` / `get_source(id) → optional` / `list_sources()` | 来源可靠性（reliability 0~1 + updated_at + meta json；未注册返回 nullopt） |

**et_sources 共享部署（制造业单库场景）**：entitytree 与 voxelstore 指向
**同一个数据库文件**（如同一个 `world.db` 或同一个 MySQL 库），et_sources
表即被 stmb 观测管线仲裁与 entity 组件天然共用——一份来源可靠性数据，
两个系统各自读写（stmb 侧 `SourceRegistry` 同形态：source_id → 0~1）。

## 6. 后端选择与构造

三个领域模型的后端类命名规律一致（以事件树为例）：

| 后端 | 类 | 构造 |
|---|---|---|
| 内存 | `MemoryStore` | `MemoryStore()` |
| SQLite | `SqlStore` | `SqlStore("file.db")` / `SqlStore(":memory:")` |
| 文件目录 | `FileStore` | `FileStore("data_dir")` |
| Redis | `RedisStore` | `RedisStore("host:6379", key_prefix = "ust")` |
| MySQL* | `MySqlStore` | `MySqlStore("mysql://user:pass@host:3306/dbname")` |
| PostgreSQL* | `PgStore` | `PgStore("host=... user=... dbname=...")` |

voxel 对应：`MemoryVoxelStore / SqlVoxelStore / FileVoxelStore /
RedisVoxelStore / MySqlVoxelStore / PgVoxelStore`；
entitytree 对应：`MemoryEntityStore / SqlEntityStore / FileEntityStore /
RedisEntityStore / MySqlEntityStore / PgEntityStore`。

\* 仅当 CMake 探测到原生库时可用（`UNISTORE_WITH_MYSQL / UNISTORE_WITH_PG`）。

选择建议：

- **嵌入/单机/边缘**：sqlite（单文件、零运维）或 file（可人读、易调试）；
- **服务化/共享**：mysql / postgres；
- **缓存层/短生命周期/高并发读**：redis（注意其为全量 hash+set 布局，
  适合中小数据量）；
- **测试**：memory / sqlite(":memory:")。

## 7. 接入新的领域模型

新模型（例如未来的"规则系统""意图系统"）接入统一层的步骤：

1. **定义类型**：`include/yourmodel/types.h`（POD 风格结构 + 必要的
   to_json/from_json）。
2. **声明表结构**：在实现文件里写出 `TableSchema`——哪些字段、谁是主键、
   是否 auto_seq、哪些维度需要二级索引。**可检索的语义不要藏进 JSON 列，
   拆独立索引表**。
3. **写 Record 映射**：`to_record(领域结构)` / `xxx_from(Record)`。
4. **实现领域接口**：`RecordYourStore : public YourStore`，构造时
   `create_table`，方法翻译成 SPI 调用（参照 `RecordEventStore` /
   `RecordVoxelStore`，各约 400 行）。
5. **写后端薄封装**：每个后端一个一行注入的子类
   （`MemoryYourStore / SqlYourStore / ...`）。
6. **写一致性套件**：接收 `YourStore&` 的领域级测试，注册到所有后端上跑。

## 8. 注意事项

- **线程模型**：当前实现为单线程设计（sqlite 以 `SQLITE_THREADSAFE=0`
  编译）。多线程调用方请在外层加互斥锁（与 voxel 服务层"统一互斥锁"
  模型一致）。
- **schema 演进**：建表均为幂等（`CREATE TABLE IF NOT EXISTS`），
  但目前不做列级迁移；字段变更请换代数据目录（与 voxel 的 formatVersion
  策略一致）。
- **大数据量**：redis 后端的非索引查询是全表扫描（SMEMBERS + 逐个
  HGETALL），适合中小数据量；大表请用 SQL 系后端。
- **测试环境变量**：`UNISTORE_MYSQL_DSN` / `UNISTORE_PG_CONNINFO` /
  `UNISTORE_REDIS_ADDR` 设置后，测试程序会自动追加对应后端的
  一致性测试（redis 每次运行使用随机 key 前缀，不污染既有数据）。
