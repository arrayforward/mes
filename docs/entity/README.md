# 实体搜索树子系统（entitytree / entity）

世界模型与世界操作系统（见《世界模型与世界操作系统架构说明书 v2》）
四棵数据结构底座（硬骨架）之一——**实体搜索树**的实现子系统。

核心命题：**实体不是预先声明的对象，而是从属性观测流中"浮现"的推论**。
摄像头、LLM、人工录入持续产生属性观测；子系统把观测按时空窗口聚成侧写，
侧写积累到足够证据量后浮现，再决定它属于哪个实体（合并 / 新建 / 存疑）。
一切结论都可被新证据纠正，且全部历史可审计。

## 系统位置

```
┌─ entity 组件（D:\manufacture\entity）──────────────────────────┐
│  entity::Resolver —— 算法与变化逻辑层                            │
│  浮现(θ_form) → 合并/新建/存疑(θ_merge/θ_split) → 拆分 → 检索    │
│  半衰期时效衰减（查询时有效值） / 来源可靠性反馈闭环               │
└──────────────┬───────────────────────────────┬────────────────┘
               │ 只依赖 EntityStore 接口         │ 只用路径码纯函数
┌──────────────▼─────────────────┐  ┌───────────▼───────────────┐
│ storage / entitytree            │  │ geocore                    │
│ （D:\manufacture\storage）      │  │ （D:\manufacture\geocore） │
│ 7 张表的增删改查与检索原语，     │  │ 锚点 AnchorPath 路径语义：  │
│ 跑在 RecordBackend 统一层       │  │ 层级邻近（祖先/后代/LCA）   │
│ （memory/sqlite/mysql/pg/       │  │                            │
│  redis/file 六后端）            │  │                            │
└──────────────┬─────────────────┘  └────────────────────────────┘
               │ 同一个 RecordBackend 统一层上的兄弟模型
        ┌──────┴──────┐
   eventstore      voxelstore / stmb
   （事件树：       （时空记忆块：
    观测的          锚点状态与版本链；
    event_ref       et_sources 来源可靠性
    溯源去向）       与其仲裁管线共享）
```

协作关系：

- **eventstore（事件树）**：Observation.event_ref 可回指事件树中的溯源事件，
  "这条属性证据来自哪段叙事/哪次感知批次"由事件树回答。
- **voxelstore / stmb（时空记忆块）**：`et_sources` 来源可靠性表与 stmb 观测
  管线仲裁共享——制造业单库部署时 entitytree 与 voxelstore 指向同一数据库
  文件，一份来源画像两个系统共用；锚点语义同源（geocore AnchorPath）。
- **geocore**：锚点路径码纯函数（ParseAnchorPath / IsPrefixOf / LcaDepth），
  支撑"房间 → 楼 → 街区"式的层级邻近召回。

## 能力清单

- 观测摄入与时空窗口聚合（append-only 证据链，镜像永久保留）
- 信息量浮现（多源互证加分、同源重复边际递减）
- 实体解析：属性倒排 + 锚点层级邻近双路召回，三区间决策（合并/新建/存疑）
- 误合并纠正（拆分），合并/拆分全历史 append-only 可审计
- 实体检索：多路属性召回、α·匹配度 + β·可信度排序、topk、可信度过滤、
  逐属性 explanation 可解释输出
- 时效语义：实体可信度与属性命中均支持半衰期衰减（基准值不改写）
- 来源可靠性：有效置信度折算 + merge/split 反馈闭环自学习，与 stmb 共享
- 多分辨率时间桶：bucket_sizes 分辨率梯子 + rollup 粗桶镜像，
  观测稀疏时常驻实体经粗桶回退仍可召回
- 轨迹连续性校验：vmax 物理可达（veto 否决 / penalty 压分），
  距离由 DistanceProvider 注入，geocore 适配器开箱可用
- 换后端不换行为：memory / sqlite / mysql / postgres / redis / file

## 快速开始

```bash
# entity 组件（自动引入兄弟工程 storage 与 geocore）
cd D:\manufacture\entity
cmake -B build -G Ninja
cmake --build build

./build/entity_tests.exe   # resolver 一致性套件（memory + sqlite，298 项检查）
./build/demo_entity.exe    # 演示：浮现→合并→拆分→检索（生成 demo_entity.db）
ctest --test-dir build

# storage 侧存储层一致性套件（可选）
cd D:\manufacture\storage
cmake -B build -G Ninja && cmake --build build
./build/entity_tests.exe   # 纯存储套件（260 项检查）
```

## 文档索引

| 文档 | 内容 |
|---|---|
| [设计文档.md](设计文档.md) | 设计思想、三层模型、职责边界、核心算法公式、7 表映射、调参指导 |
| [使用文档.md](使用文档.md) | CMake 集成、Resolver API 参考、上层调用场景流程、EntityStore 速查、ResolverConfig 全参数表、部署说明 |
