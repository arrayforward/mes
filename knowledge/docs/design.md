# 语义图谱树（knowledge）设计文档

对应《世界模型与世界操作系统架构说明书 v2》§4.1 的**语义图谱树（Ontology Tree）**：
世界的概念、类型、属性与推理规则库（"是什么？意味着什么？"）。
它只读消费 storage 事件侧写系统（Profile 五要素 + Link 因果边 + Binding 指代消解），
进行**事件理解**与**进一步推理**。词汇-联想侧由 vendored ame 子集的真实代码与词典
直接驱动（见 §3），本体侧补齐 ame 缺失的能力：真正的 is-a 层级与符号规则推理。

## 1. 关系分类法（RelType）

唯一系数来源是 `conduction.cpp` 的 `conduction_coeff()`（镜像 ame core），
扩散引擎与推理器都从这里取系数，禁止各处自写字面量。

| 分组 | 关系 | 系数 | 说明 |
| --- | --- | --- | --- |
| 本体层级 | IS_A | 0.95 | 概念→概念（子类），传递 |
| | PART_OF | 0.8 | 概念→概念（组成），传递 |
| 语义关系 | CAUSES | 0.9 | 因果 |
| | TRIGGERS | 0.85 | 触发/使能，**规则推理主通道** |
| | REQUIRES | 0.8 | 前置条件 |
| | PREVENTS | 0.4 | 抑制 |
| | SIMILAR | 0.7 | 相似（近义词保留双节点，只建弱边） |
| | OPPOSITE | 0.5 | 对立（最低档，防反义被当强联想） |
| | COOCCUR | 0.6 | 共现（写入即生长） |
| | BEFORE | 0.7 | 时序（同步 storage `Link.before`，镜像 ame NEXT） |
| 桥接层 | INSTANCE_OF | 0.9 | 实例→概念 |
| | OBSERVED_IN | 0.5 | 概念/实例→侧写锚点 `{profile_id, event_id}` |

SIMILAR / OPPOSITE / COOCCUR 为双向边（`rel_undirected()`）；扩散时整张图按
无向加权图处理（联想回忆沿边双向流动），OBSERVED_IN 锚点指向非图节点自然终止。

## 2. 公理层（ame 没有、本体必需）

- **传递闭包**：`is_a(a,b)` / `ancestors()` / `descendants()` / `part_of_ancestors()`，
  沿边 BFS 按需计算（种子本体规模小，不物化闭包表）；`is_a_path()` 重建路径供解释。
  is-a 与 part-of 是两条独立通道，互不泄漏。
- **规则沿 is-a 继承**：实例 `x INSTANCE_OF A`、`A IS_A* B`、规则 `(B, TRIGGERS, C)`
  ⇒ 推出 `x 可能触发 C`，置信度 = 规则置信度 × 绑定置信度。
  `rules_for(concept_id)` = 自身规则 + 全部 is-a 祖先的规则。
- **矛盾检测**：同一 (subject, candidate) 同时被 TRIGGERS/CAUSES 与 PREVENTS 推出时，
  两侧 `Inference.conflict` 标注（供审计，不删除）。
- **静态性**：本体与词典是带 `version` 的 JSON 资产，运行时只读，变更需升版本。

## 3. 词汇层：vendored ame 子集（真实代码与词典直接驱动）

ame 的六个模块（core / storage / time / vector / keyword / diffusion）与其测试、
词典资产以 **UNMODIFIED** 方式 vendored 在 `ame/`（与 `D:/agent/memory/ame` 逐文件
byte-identical；未拷贝的 llm/bench/engine 等子系统与本库无关）。
`knowledge::Engine` 持有一个 `ame::Storage` + `ame::KeywordManager` 实例作为
**词汇-联想层**，与本体层分工：ame 提供词汇覆盖（别名/近义/RELATED 边/扩散），
本体层提供类型结构（IS_A/规则/实例/锚点）。

### 3.1 MinGW 兼容（不修改 ame 源码）

`cmake/ame_mingw_compat.h` 通过 `target_compile_options(ame_* PRIVATE -include ...)`
强制包含进全部 ame 目标，解决三个移植差异：

1. ucrt 无 `gmtime_r`（`ame/time/src/time_service.cpp:74`）——用参数对调的
   `gmtime_s` 包装出 POSIX 语义；
2. 严格 `-std=c++20` 下 `<cstdint>` 不再被传递包含（json.cpp/uid.h 用 uint32_t 等）；
3. 严格标准模式下 `<cmath>` 不定义 `M_PI`（storage.cpp 的 haversine）。

ame 自身的六个测试（test_core/storage/time/vector/keyword/diffusion）从 knowledge
根 CMake 直接注册（ame/tests 无独立 CMakeLists，逐个链接对应模块库）。

### 3.2 词典灌库（复刻 lexicon_apply）

`src/ame_lexicon.cpp` 复刻 `ame/bench/src/lexicon_apply.cpp` 的三分流，驱动
ame::KeywordManager：`syn_strong` → 别名归一表（主词需互为同义组）；
`syn_near` → 近义词表（score>0.6、≤8 条）+ 双方节点存在时 `REL_SIMILAR` 边；
`antonyms` → `REL_OPPOSITE` 边；兼容旧版 `synonyms`/`related` 字段。
与 ame 原版唯一差异：**near 表对称登记**（非主词如"袭击"也能反向降级）。
`Engine::load_ame_lexicon(path)` 幂等、多文件并集；`apply_lexicon` 会把种子词典
一并灌入 ame 层。真实词典：`ame/lexicon/lexicon_zh.json`（中文词林 7.7 万词，
~0.5s 灌入 6.7 万条别名）、`lexicon.json`（2910 条，旧 schema）。

### 3.3 归一即匹配扩展（重要设计决策）

实测中文词林的同义群较松，争议词的别名归一噪声大且与处理顺序相关
（last-wins）：勇者→硬汉、进攻→进击、袭击→袭取、拔→薅。因此：

- **实例命名保留原始 surface**（"勇者"永远是"勇者"）；
- 概念匹配遍历**词形集合** `surface_forms()` = 原词 + ame 归一 + 种子归一（含链式），
  原词优先，归一噪声永远丢不掉原词；
- 近义降级候选 `near_candidates()` 汇集各词形的 ame `near_words` ∪ 种子 `near_of`。

### 3.4 摄入时的 ame 侧生长

摄入每条侧写时，五要素各词形经 `get_or_create` 落 Keyword 节点，近义词补
`REL_SIMILAR` 边并 `wake_keyword` 唤醒（故事词汇作为活跃词汇参与扩散）。

### 3.5 推理的 ame 软通道

`Reasoner::infer` 在本体图扩散之外，追加 **ame 扩散软通道**：查询词各词形的
Keyword 节点全部作种子（别名双向归一会产生多个节点，如 袭击↔袭取），沿
`ame::Storage` 的 RELATED 边跑 `ame::DiffusionEngine`（深度 2、budget 64），
激活词记入 `ReasoningChain::ame_activated`（词名 + 能量，top8）。

## 4. 词典三分流（本体侧，镜像 ame M3 / lexicon_apply）

`assets/lexicon.json` 采用 ame schema `{version, entries:[{word, syn_strong, syn_near, antonyms}]}`：

- `syn_strong` → **别名归一表**（归并为单节点，不建边）；主词别名需互为同义组成员，
  防多义词互指稀释。
- `syn_near`（score>0.6、每词 ≤8 条）→ 保留双节点，双方概念存在才建 **SIMILAR** 弱边；
  近义词表对称化（非主词如"袭击"也能反向降级扩展到主词概念）。
- `antonyms` → **OPPOSITE** 边（最低系数）。

写入侧与查询侧词形对齐统一走 `Lexicon::normalize()`（O(1)）。

## 4. 侧写理解管道（ingest，八步）

`Ingester::ingest_profile(store, profile_id)`（幂等，重复摄入返回缓存）：

1. 读 `Profile` + `effective_binding`（subject/object/place → entity_id、置信度）。
2. 五要素保留原始 surface；词典归一只作匹配扩展（§3.3）——概念匹配遍历
   `surface_forms()` 词形集合，同时五要素各词形在 ame 侧落 Keyword 节点并补
   RELATED 边（§3.4）。
3. surface → `InstanceNode`（已存在则复用），挂 `INSTANCE_OF` 概念
   （各词形按名命中，否则按 `Entity.type` 映射概念 `attrs["entity_type"]`，绑定置信度作边权）；
   事件本身合成事件实例（"勇者拔出圣剑"），`INSTANCE_OF` 动词概念——
   这是规则沿 is-a 继承的统一入口。动词概念解析先各词形精确命中，再按近义词降级（×score）。
4. **事件类型判定**：沿动词概念的 IS_A 链上溯 → 分类链 + 顶类（根的直接子类）。
5. **锚点回写**：事件实例/动词概念/主客体实例挂 `OBSERVED_IN {profile_id, event_id}` 边——
   可审计回指 storage，事实仍由 storage 唯一持有。
6. **自动生长**：同侧写五要素节点两两 `COOCCUR` upsert（weight+1）；
   共现 ≥3 打 `crystallized` 结晶提示（镜像 ame）。
7. `Link.causes/before` 同步为事件实例级 `CAUSES`/`BEFORE` 边
   （单条摄入时目标未摄入则跳过，批量摄入后统一兜底）。
8. **规则前向链**：事件级（动词概念继承的规则，置信度 = 规则 × 动词匹配权重）+
   实例级（实例类型概念继承的规则，置信度 = 规则 × 绑定置信度），
   产出 `Inference{candidate, confidence, path, conflict}`。

## 5. 推理查询管道（reasoner + diffusion，镜像 ame "软→硬→软"）

`Reasoner::infer(query)`：

1. **种子**：查询词的词形集合（`surface_forms()`，原词优先）→ 硬种子
   （概念/实例精确命中，权重 1.0）+ 近义降级扩展（×0.4×score，ame 层 ∪ 种子词典）。
2. **符号扩展（硬）**：is-a 闭包 + 规则前向链（限深 3，结论概念继续触发，
   置信度沿规则衰减）——确定性、可审计。
3. **扩散（软）**：本体图 `diffuse()` 加权逐层 BFS：`trans = 传导系数 × 边权`
   （weight>5 按 `5+log1p(w-5)` 软化），能量按 `(hop+1)/(hop+2)` 衰减并按出度归一，
   budget 限流、<0.01 枯竭剪枝、每节点保留 **top3 传播路径**；
   另加 **ame 软通道**（§3.5）：`ame::DiffusionEngine` 沿关键词图 RELATED 边扩散，
   激活词记入 `ame_activated`。
4. **融合**：规则命中 + 图能量汇成 `ReasoningChain{seeds, inferences, activated,
   ame_activated, confidence}`。

## 6. 持久化与 id 约定

- 本体/词典：`assets/` 带版本的 JSON 资产（运行时只读）。
- 运行时图（实例 + OBSERVED_IN + COOCCUR + BEFORE/CAUSES）：`Engine::snapshot()` /
  `save_snapshot()` JSON 存取；崩溃可从 storage 重新 ingest 重建（事实源在 storage）。
- id 保留 storage 前缀约定（`cp-` 概念、`in-` 实例、`rl-` 规则），但由名字**确定性派生**
  而非随机：同一种子资产多次加载 id 稳定，快照因此可跨进程移植恢复。

## 7. 与 storage 的边界

只读消费：`get_profile / effective_binding / get_entity / query_profiles /
links_from / timeline`。不修改 storage 的任何数据与文件；knowledge 侧的一切
"事实回指"都通过 OBSERVED_IN 锚点的 profile_id/event_id 完成。
