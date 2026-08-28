# bench 使用文档

## 1. 构建

依赖：CMake ≥ 3.20、C++20 工具链（MinGW g++ ≥ 13 / MSVC ≥ 2019 / clang ≥ 10）、Ninja。
兄弟模块 storage / geocore / entity / knowledge 以源码树 `add_subdirectory` 引入，
路径可用 `-DBENCH_STORAGE_DIR=` 等覆盖（默认相对仓库根）。

```bash
cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build bench/build -j
```

产出：`bench/build/tick_bench(.exe)`。

## 2. 运行

```bash
tick_bench [--ticks=N] [--warmup=N] [--stations=K]
           [--backend=memory|sqlite] [--knowledge=on|off] [--full-lexicon]
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--ticks=N` | 1000 | 有效计时的拍数 |
| `--warmup=N` | 50 | 预热拍数（不计入统计）：冷缓存、建桶、建表 |
| `--stations=K` | 8 | 模拟工位数（anchor_ref 轮转） |
| `--backend=memory|sqlite` | memory | 存储后端口径：纯算法 / 真实持久化 |
| `--knowledge=on|off` | on | 是否启用第 5 阶段（语义理解） |
| `--full-lexicon` | 关 | 启动时灌入 7.7 万词真实词典（一次性 ~0.9s） |

常用组合：

```bash
# 纯算法基线（无词典）
tick_bench --ticks=2000 --warmup=100 --backend=memory --knowledge=off

# 完整心跳 + 全词典（最贴近生产）
tick_bench --ticks=2000 --warmup=100 --backend=memory --full-lexicon

# 持久化代价（sqlite，拍数调小——单拍 ~100ms）
tick_bench --ticks=300 --warmup=30 --backend=sqlite

# smoke（ctest 注册的就是这条）
ctest --test-dir bench/build
```

sqlite 口径运行前后自动清理 `bench_tick_*.db` 三个临时库文件。

## 3. 输出解读

```
knowledge 装配（一次性，不计入 tick）：      ← 预热成本，只打不统计
  define_ontology      14.7 ms
  apply_lexicon        11.1 ms  (本体建边 2 条)
  load_ame_lexicon    861.0 ms  (真实词典灌入 66628 条)

每阶段耗时（2000 拍有效样本）：
  entity       n=2000   avg= 641.95  p50= 622.00  p95=1157.10  p99=1443.90  max=2291.20  min=96.40 us
  ...

整拍（5 阶段串行合计）：
  tick_total   ...

吞吐：850 ticks/s（串行单线程）
```

- **avg 看成本分布**：哪一阶段占比大，瓶颈就在哪。
- **p95/p99 看节拍稳定性**：心跳要按节拍排产，尾延迟比均值更影响"这一拍
  会不会超时"。p99 ≪ 节拍周期才算稳。
- **max 看离群**：个别拍因实体浮现/桶切换/sqlite 页写放大出现尖峰，
  max 与 p99 差距大说明有低频重路径。
- **吞吐口径**：串行单线程；多工位并行采集另需并发基准（未实现，见
  design.md §5）。

## 4. 扩展新阶段

以"加入 voxel 版本链 append"为例：

1. `tick_bench.cpp` 顶部 `kStages` +1，`kStageNames` 追加名字。
2. 循环内插入取点对 `auto tN = Clock::now();` 夹住新调用。
3. 统计区 `stage[k].add(Us(tN - tN-1).count());`，整拍终点同步后移。
4. 若是可选阶段（如 knowledge 那样），加 `--xxx=on|off` 参数并在
   `parse_args` 注册。

注意保持**感知构造在计时区外**：所有输入数据（Observation / Vec3d /
payload 字符串）在 `tick_begin` 前备好。

## 5. 故障排查

| 现象 | 原因与处理 |
|---|---|
| 配置阶段报目标重复 `unistore` | bench 不能再自行 add_subdirectory(storage)——storage 由 knowledge 带入；检查 BENCH_*_DIR 是否指向同一源码树 |
| `understand` 抛异常 | 多为资产路径错误：assets 目录由编译定义 `KNOWLEDGE_ASSETS_DIR` 注入，改源码树位置后需重新 cmake |
| sqlite 口径首拍极慢 | 正常现象（建表 + 页分配），由 `--warmup` 吸收；不要把 warmup 设为 0 |
| MinGW 下 exe 缺 dll | 构建已带 `-static-libgcc -static-libstdc++`；若自行改 CMakeLists 勿删 MINGW 分支 |
