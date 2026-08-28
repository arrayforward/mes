# storage 设计文档 —— 统一数据存储层

> 版本：v1.0
> 定位：世界模型与世界操作系统（见《世界模型与世界操作系统架构说明书 v2》）的
> **统一数据存储子模块**。上层系统的任何数据结构都以"领域模型"身份接入本层，
> 自动获得全部存储后端，不为每个模型重复写数据库适配。

## 1. 设计目标

1. **统一**：所有领域模型共享一套存储抽象（RecordBackend SPI），
   MySQL / PostgreSQL / SQLite / Redis / 文件目录 / 内存各实现一次，
   新模型接入即可在全部后端上运行。
2. **可审计**：对"事实类"数据（事件树）坚持 append-only——只有追加，
   没有 update/delete，历史完整可回溯。
3. **索引丰富**：append-only 意味着索引只增不改，可以"一次写入、多次索引"，
   写入时同步维护任意多个索引，查询永远走索引而非扫描。
4. **零重依赖**：除 vendored 的 nlohmann/json 与 sqlite3 外无强制第三方依赖；
   MySQL/PostgreSQL 用原生连接库（不用 ODBC），CMake 探测到才编译；
   Redis 为自研 RESP2 客户端。Linux / Windows 均可构建。

## 2. 总体架构

```
┌─ 领域层 ─────────────────────────────────────────────────────────┐
│  eventstore（事件树）            voxelstore（时空记忆块）          │
│  Narrative/Event/Profile/Link/  VoxelBlock/VoxelVersion/          │
│  Entity/Binding/Modifier        VoxelInstance/TrackPoint          │
│  EventStore 接口                VoxelStore 接口                    │
│  RecordEventStore（模型→表映射） RecordVoxelStore（模型→表映射）   │
│  ──────────────────────────────────────────────────────────────  │
│  entitytree（实体搜索树，纯存储层）：Observation/AttrProfile/       │
│  EntityNode/EntityBinding/SourceRecord；RecordEntityStore         │
│  （模型→表映射）。浮现/合并/拆分/检索算法在 entity 组件             │
└──────────────────────────┬───────────────────────────────────────┘
                           │ 只依赖 RecordBackend SPI
┌─ 统一层 ──────────────────▼───────────────────────────────────────┐
│  Record（字段名→Value）/ TableSchema / Condition / Ordering       │
│  RecordBackend：create_table / put / append / get / remove /      │
│                 query / update_where                              │
└──┬────────┬──────────┬──────────┬──────────┬──────────┬──────────┘
   memory   sqlite     mysql      postgres   redis      file
 (参考实现) (穿刺验证) (原生库)   (原生库)  (自研RESP) (WAL+JSON)
```

依赖方向自上而下，无环。领域层不知道后端是谁；后端不知道领域语义。

## 3. 统一层设计（`include/storage/`）

### 3.1 记录模型

```cpp
using Value  = std::variant<std::nullptr_t, int64_t, double, std::string>;
using Record = std::map<std::string, Value>;

struct FieldDef { std::string name; FieldType type; };  // kInt / kReal / kText
struct TableSchema {
    std::string name;
    std::vector<FieldDef> fields;
    std::string pk;                        // 主键字段
    bool auto_seq;                         // true = append-only 自增主键表
    std::vector<std::vector<std::string>> indexes;  // 二级索引（可复合）
};
struct Condition { std::string field; Op op; Value value; };  // Eq/Le/Ge/IsNull/NotNull
struct Ordering  { std::string field; bool desc; };
```

设计取舍：

- **Value 只有四种类型**——bool 用 int64 0/1，JSON 子文档（图、轨迹、别名等）
  一律 dump 成 text。换取的是：任何后端（包括纯字符串的 Redis）都能无损表达。
- **两类表**对应两种数据性质：
  - `auto_seq = true`：append-only 表（侧写、绑定、版本……），
    只许 `append`，主键由后端分配（物理追加序）；
  - `auto_seq = false`：upsert 表（实体、块当前态、meta……），
    `put` 按主键覆盖，表达"可变当前态"。
- **`update_where` 是受限更新**，只为"关闭生效区间"（版本链 seal）这类
  操作存在，不是通用 UPDATE——这是对 append-only 原则的刻意收口。

### 3.2 SPI 语义（`record_backend.h`）

| 方法 | 语义 |
|---|---|
| `create_table` | 幂等建表（含声明的二级索引） |
| `put` | 按主键 upsert（upsert 表） |
| `append` | 自增主键表追加，返回分配的 seq（append-only 表） |
| `get` | 按主键取一条 |
| `remove` | 按主键删除（领域层决定哪些表允许删；事件树不删） |
| `query` | 条件 AND + 多级排序 + limit |
| `update_where` | 受限更新：命中行套用 patch（不得改主键） |

### 3.3 索引策略：一次写入、多次索引

- 索引在 `TableSchema.indexes` 中**声明**，后端各自落实：
  memory 用哈希桶（写时维护），SQL 用 `CREATE INDEX`，
  redis 用 `idx:{table}:{field}:{vals}` 集合桶，file 跟随内存索引。
- **可检索的语义不进 JSON 子文档**：实体别名拆 `entity_names` 表、
  修饰语拆 `profile_modifiers` 表。这样名称/修饰语查询在任何后端上都是
  普通等值查询，不依赖某个数据库特有的 JSON 函数——这是统一层能真正
  "统一"的关键决策。

## 4. 领域模型一：事件树 eventstore

对应架构说明书 L0 最底层的 append-only 事件树：存故事事件、真实事件、
说明书场景。

### 4.1 概念模型

```
Narrative（叙事来源：story|fact|scenario）
  └─ Event（事件：分组锚点，内容全在侧写里）
       └─ Profile（侧写，原子单元）
            五要素：time(TimeRef) / place / subject / verb / object
            perspective：视角（active/passive/observer/... 自由字符串）
            modifiers：修饰语列表（见 4.3）
            graph：五要素+修饰语组装的图（保真）
            seq：全局自增（物理追加序）
Link（侧写间 DAG 边）：before | causes | refines | retracts
Entity（实体）：name 可重复、entity_id 定身份、aliases 支持同人异名
Binding（指代消解，append-only）：(profile_id, slot) → entity_id，
  latest wins，全历史可审计，confidence 承接概率消歧
```

### 4.2 关键设计决策

- **侧写是原子单元，事件只是锚点**。同一事件从主动者/被动者/旁观者等
  视角各存一条侧写，互不覆盖。
- **虚拟时间与 Link**。时间可以是"从前""第二天"这类虚时间（`TimeRef.kind
  = virtual`），虚时间没有全序，先后关系由显式 Link（before/causes）表达；
  `timeline()` 在基类用 Kahn 拓扑排序实现一次（refines/retracts 不参与排序，
  环检测抛错），所有后端行为一致。
- **人名问题的专门设计**：侧写只存 surface 字符串且永不修改；
  "这个名字指谁"是独立的 append-only Binding 记录。重名不同人
  （两个 entity 同名不同 id）、同人异名（aliases）、未消歧（无 Binding）、
  重消歧（新 Binding 覆盖语义、旧记录保留）都被自然表达。
- **事实与解释分离**：Profile 是事实层（不可变），Binding 是解释层
  （可追加纠正）。纠正消歧不会污染原始记录。
- **append-only 铁律**：接口只有 `append_*`。纠正 = 追加 `refines`/`retracts`
  关系的新侧写；重消歧 = 追加新 Binding。

### 4.3 修饰语（Modifier）

五要素是骨架，"奋力地拔""一把锋利的剑"中的方式/程度/量词/性质不能在
抽取时丢失：

```cpp
struct Modifier { std::string target;  // subject|verb|object|place|time
                  std::string kind;    // quality|quantity|degree|manner
                  std::string text; }; // 原文
```

保序（同槽位多个修饰语先后有意义）、进图（修饰节点+修饰边）、可查询
（`find_profiles_by_modifier`，条件必须落在同一修饰语上）。

### 4.4 表映射（8 张表）

`narratives` / `events` / `profiles` / `links` / `entities` /
`entity_names`（别名索引）/ `bindings` / `profile_modifiers`（修饰语索引）。
profiles 的 `seq` 与 bindings 的 `seq` 为自增主键；五要素各列均建索引。

## 5. 领域模型二：时空记忆块 voxelstore

支撑 `D:\manufacture\voxel`（stmb 时空记忆块服务）的数据存储，
镜像其 `MemoryBlock / BlockVersion / DynamicInstance` 落盘字段。

### 5.1 与事件树的本质区别

事件树存"历史事实"（append-only）；voxel 存"空间状态"——块的当前态
随观测演化（状态机、确认计数、LOD），因此：

- **块当前态可变**（upsert 表 `voxel_blocks`）；
- **版本链 append-only**（`voxel_versions`）：`seal_version` 关闭
  `[valid_from, valid_to)` 区间是唯一允许的"修改"，
  `version_at(t)` 回答"这里以前是什么"；
- **动态实例可变**（`voxel_instances`：最新包围盒 + 轨迹 + 状态机）。

### 5.2 时空联合查询

`query_blocks(region?, time_from?, time_to?, level?)`：
AABB 相交翻译为 6 个范围条件（`min_x <= q.max_x AND max_x >= q.min_x` 逐轴），
时间为闭区间，level 等值。条件语义在统一层表达，全后端一致。

### 5.3 表映射（4 张表）

`voxel_blocks`（upsert）/ `voxel_versions`（append-only + seal）/
`voxel_instances`（upsert）/ `voxel_meta`（id 发生器与全局参数）。

## 6. 领域模型三：实体搜索树 entitytree

### 6.1 设计思想

实体不是预先声明的对象，而是从属性观测流中"浮现"的推论。
三层模型：

```
Observation（属性观测：最小证据单元，append-only，永不修改）
    attribute_key/value/confidence/source_id/anchor_ref/timestamp/event_ref
    → AttrProfile（侧写：anchor_ref × time_bucket 时空窗口内的属性包聚合，
       镜像永久保留；status：pool 积累中 → emerged 达阈值待解析 → linked 已挂实体）
    → EntityNode（实体：侧写的归并视图 merged view，当前态可变的缓存，
       带 view_version 与实体级 credibility；status：candidate/confirmed/disputed）
EntityBinding（侧写→实体绑定，append-only，latest wins）：
  合并 = 追加 merge 绑定；拆分 = 追加指向新实体的 split 绑定；全历史可审计
```

与前两个模型的关系：观测与绑定延续事件树的 append-only 铁律（只有追加）；
侧写与实体延续 voxel 的"可变当前态"（upsert）——实体 merged view 只是
缓存，真相永远在观测与绑定里。

### 6.2 职责边界：storage 只做增删改查

**entitytree 在 storage 内是纯存储层**：七张表的落地、CRUD、索引查询、
append-only 约束（观测/绑定只追加）、倒排索引维护（随 upsert 整组替换）、
有效绑定（latest wins）与实体→侧写反查语义。
浮现/合并/拆分/检索排序/时效衰减/来源可靠性闭环等**算法与变化逻辑全部在
entity 组件**（`D:\manufacture\entity`，`entity::Resolver`）——它只依赖
EntityStore 接口，任意后端行为一致；锚点层级邻近用的 geocore 路径码
纯函数也由 entity 组件直接链接，storage 不感知 geocore。

存储层为算法层提供的两处支撑：

- `EntityNode.updated_at`：credibility 的**基准时间**（秒）。
  stmb 纪律——查询返回有效值、存储基准值不改写：衰减（半衰期指数）
  只在查询时由 entity 组件计算，本层只保证基准值与基准时间持久化。
- `et_sources` 来源可靠性表：与 stmb（voxelstore 观测管线仲裁）**共享**。
  制造业单库部署时 entitytree 与 voxelstore 指向同一个数据库文件，
  来源画像一份数据两方共用（stmb 侧 `SourceRegistry` 同形态：
  source_id → reliability 0~1）。

### 6.3 表映射（7 张表）

`et_observations`（append-only，索引 attribute_key/anchor_ref/source_id）/
`et_profiles`（upsert，索引 anchor_ref/time_bucket/status/level；
level = 时间桶分辨率层级，0=最细，>0 为 entity 组件 rollup 的粗桶镜像）/
`et_profile_attrs`（侧写属性倒排索引，随 upsert 整组替换）/
`et_entities`（upsert，含 credibility 基准值 + updated_at 基准时间）/
`et_entity_attrs`（实体属性倒排索引，随 merged view 重建整组替换）/
`et_bindings`（append-only，索引 profile_id/entity_id）/
`et_sources`（upsert，source_id 主键，entitytree ↔ stmb 共享）。
属性召回在任何后端上都是普通等值查询，不依赖特定数据库的 JSON 函数。

## 7. 后端设计要点

| 后端 | 要点 |
|---|---|
| **memory** | 参考实现。每表主键映射 + 声明索引的哈希桶，写时维护；Eq 选"字段数最多且被全覆盖"的索引桶，其余条件过滤 |
| **sqlite** | SQL 参照实现。TableSchema 直译 `CREATE TABLE/INDEX`；`INTEGER PRIMARY KEY AUTOINCREMENT`；`INSERT OR REPLACE`；vendored amalgamation，`SQLITE_THREADSAFE=0` |
| **mysql** | libmysqlclient 原生库。`BIGINT AUTO_INCREMENT`；`REPLACE INTO`；TEXT 不能做键 → 主键/索引字段 `VARCHAR(191)`；值经 `mysql_real_escape_string` 转义 |
| **postgres** | libpq 原生库。`BIGSERIAL`；`ON CONFLICT DO UPDATE`；全参数化 `PQexecParams`（不拼字符串）；`INSERT ... RETURNING` 取 seq |
| **redis** | 自研 RESP2 客户端（Winsock2/POSIX）。`sch:/pks:/row:/idx:/seq:` key 布局；Eq 命中索引桶否则全表扫描，过滤排序在 C++ 侧（与 memory 同一套 record_codec 工具）；`key_prefix` 多租户隔离 |
| **file** | 目录存储：每表 `schema.json` + `wal.jsonl`（只有 put/remove 两种操作；update_where 先查后改、整行落日志）；打开时回放 WAL 重建内存索引 |

## 8. 测试策略

- **一致性套件**：同一套领域级测试跑在所有后端上（`tests/conformance.cpp`
  282 项检查、`tests/voxel_conformance.cpp` 203 项检查、
  `tests/entity_conformance.cpp` 260 项检查——entitytree 为纯存储套件，
  算法场景在 entity 组件的 resolver 套件，298 项检查），
  保证"换后端不换行为"。
- **分层验证**：
  - memory / sqlite / file：每轮全量本地回归；
  - redis：真实服务联测（环境变量 `UNISTORE_REDIS_ADDR` 开启）；
  - mysql / pg：原生库实现 + stub 头文件语法检查，
    真实联测由 `UNISTORE_MYSQL_DSN` / `UNISTORE_PG_CONNINFO` 开启。
- 持久化专项：sqlite / file 均有"写入→关闭→重开→校验"测试。

## 9. 演进方向

- [x] 实体搜索树 entitytree（观测 → 侧写 → 实体三层模型 + Resolver
      解析引擎）已跑在统一层上；
- mysql / pg 在真实数据库上的联测与 CI；
- redis 批量流水线（pipeline）降低 round-trip；
- file 后端 WAL 压缩与快照；
- 新领域模型接入（只需：定义类型 → 声明表结构 → Record 映射 →
  RecordXxxStore → 后端薄封装，见 usage.md 第 7 节）。
