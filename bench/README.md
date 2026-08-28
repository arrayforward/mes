# bench —— 心跳 tick 基准测试

根 README §1 心跳模型的**可执行版本**：把"一拍"串成 entity → geocore →
eventstore → voxelstore → knowledge 五阶段调用链，逐阶段计时，回答
"这个 loop 有没有性能问题、一拍要多久"。

```
感知（合成观测，不计时）→
  entity    : Resolver.ingest        观测归并 / 实体浮现
  geocore   : BeginFrame + Save      空间锚定
  eventstore: append_event+profile   因果存证
  voxelstore: put_block              时空记忆块
  knowledge : Engine.understand      语义理解（分类链 + 规则推理）
```

## 快速开始

```bash
cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build bench/build -j

./bench/build/tick_bench --ticks=2000 --warmup=100 --backend=memory --full-lexicon
ctest --test-dir bench/build            # smoke（200 拍）
```

## 头部结论（Intel Ultra 5 125H / MinGW g++ 15.2 / Release）

| 配置 | 整拍 avg | 吞吐 |
|---|---|---|
| memory 后端 + 种子词典 | 845 µs | 1184 ticks/s |
| memory 后端 + 全词典（6.7 万条） | 1176 µs | 850 ticks/s |
| sqlite 后端（任意词典） | ~97 ms | 10 ticks/s |

knowledge 按拍推理 `understand()` ≈ 300~460 µs，词典灌库 861 ms 一次性。
sqlite 后端瓶颈已定位为 `sqlite_backend.cpp` 缺 PRAGMA（每条写一次 fsync），
与循环结构无关。完整数据见 [docs/testing.md](docs/testing.md)。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/design.md](docs/design.md) | 设计文档：测量目标、阶段定义、计时方法学、后端抽象、取舍 |
| [docs/usage.md](docs/usage.md) | 使用文档：构建、CLI 参数、输出解读、扩展新阶段 |
| [docs/testing.md](docs/testing.md) | 测试文档：环境、方法、四组全量结果、瓶颈诊断与结论 |
