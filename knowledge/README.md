# knowledge —— 语义图谱树（Semantic Ontology Graph Tree）

《世界模型与世界操作系统架构说明书 v2》§4.1 的语义图谱树：概念/类型/推理规则库
（"是什么？意味着什么？"）。只读消费 [storage](../storage) 事件侧写系统做事件理解与
进一步推理；词汇-联想侧由 **vendored ame 子集**（`ame/`，原样未改）的真实代码与
词典驱动，本体侧（is-a 层级与符号规则推理）为 knowledge 自有——正是 ame 缺失的部分。

设计细节见 [docs/design.md](docs/design.md)。

## 架构：两层

```
knowledge::Engine
├── 本体层（knowledge 自有）        —— 类型结构
│   Ontology：IS_A/PART_OF 闭包、规则库（沿 is-a 继承）、实例、OBSERVED_IN 锚点
│   种子资产：assets/ontology_seed.json、assets/lexicon.json（版本化）
└── 词汇-联想层（vendored ame，ame/）—— 词汇覆盖
    ame::KeywordManager：别名归一、近义词表、RELATED 边（REL_SIMILAR/OPPOSITE）
    ame::Storage + ame::DiffusionEngine：关键词图存储与加权扩散（软通道）
    真实词典：ame/lexicon/lexicon_zh.json（中文词林 7.7 万词）等
```

- vendored 模块：core / storage / time / vector / keyword / diffusion + 各自测试 +
  lexicon 资产，全部 **UNMODIFIED**（与 `D:/agent/memory/ame` 逐文件 byte-identical）。
- MinGW 兼容性不碰 ame 源码：`cmake/ame_mingw_compat.h` 以 `-include` 强制包含进
  ame 目标（`gmtime_r` 包装、`<cstdint>` 传递包含、`M_PI` 补齐）。
- 词典灌库复刻 `ame/bench/src/lexicon_apply.cpp` 的三分流
  （`src/ame_lexicon.cpp`，near 表对称登记是唯一差异），驱动 ame::KeywordManager。
- 归一只作**匹配扩展**：真实大词典同义群较松（勇者→硬汉 之类噪声），摄入/推理遍历
  词形集合（原词/ame 归一/种子归一）做概念匹配，实例命名保留原始 surface。

## 构建与测试

依赖：C++20 工具链（Windows MinGW g++ ≥ 13）、CMake ≥ 3.20、Ninja；
storage 源码在 `D:/manufacture/storage`（以 `add_subdirectory` 引入，
nlohmann/json 与 sqlite3 复用其 third_party，无需下载）。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

产出：静态库 `libknowledge.a` + ame 六个模块库、13 个测试程序
（knowledge 7 + ame 6）、`demo_understand.exe`。

## 快速开始

```cpp
#include "eventstore/backends/sql_store.h"
#include "knowledge/engine.h"

knowledge::Engine eng;
eng.define_ontology("assets/ontology_seed.json");   // 版本化本体资产（运行时只读）
eng.apply_lexicon("assets/lexicon.json");           // 种子词典：本体建边 + 灌入 ame 层
eng.load_ame_lexicon("ame/lexicon/lexicon_zh.json"); // 真实大词典灌入 ame 层（幂等）

eventstore::SqlStore store("demo_story.db");        // storage 的事件侧写库（只读消费）
auto results = eng.ingest_all(store);               // 摄入全部侧写（八步管道）
// results[i].chain       —— 事件分类链：拔出 → 持械 → 战斗动作 → 动作
// results[i].inferences  —— 规则推理：战斗动作 TRIGGERS 警觉（带可解释路径）

knowledge::ReasoningChain c = eng.infer("袭击");    // 种子 → 符号扩展 → 扩散 → 融合
// c.ame_activated        —— ame 关键词图 RELATED 扩散的激活词（软通道）
eng.save_snapshot("snapshot.json");                 // 运行时图快照（可重建）
```

Engine 动词：`define_ontology / apply_lexicon / load_ame_lexicon /
ingest_narrative(ingest_all) / understand(profile_id) / infer(query) /
snapshot + load_snapshot`。

## 演示

```bash
./build/demo_understand.exe            # 默认读 D:/manufacture/storage/demo_story.db
./build/demo_understand.exe path/to.db # 或指定库
```

输出每条侧写的分类链与规则推理（含路径）、真实词典归一示例、ame 软通道激活词，
并验证五条标准：is-a 分类正确、规则推理可解释、OBSERVED_IN 锚点回指、
真实词典别名驱动概念命中（村落→村庄、瞧见→看到）、ame RELATED 扩散激活近义词。

## 目录

```
include/knowledge/   types / conduction / lexicon / ontology / ingest / diffusion / reasoner / engine / ame_lexicon
src/                 对应实现
assets/              ontology_seed.json（种子本体，带版本）、lexicon.json（ame schema 种子词典）
ame/                 vendored ame 子集（core/storage/time/vector/keyword/diffusion + tests + lexicon，UNMODIFIED）
cmake/               ame_mingw_compat.h（MinGW 兼容 shim，-include 强制包含）
tests/               每组件单测 + e2e（SqlStore(":memory:") 与 demo_story.db + 真实词典）
examples/            demo_understand.cpp
docs/design.md       关系分类法、公理、管道与 ame 集成设计
```
