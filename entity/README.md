# entity —— 实体搜索树的算法与变化逻辑层

制造业世界模型系统中"实体从观测流中浮现"的算法组件。
`entity::Resolver`：浮现 → 合并/新建/存疑 → 拆分 → 检索排序，
以及时效衰减（半衰期）、来源可靠性闭环、多分辨率时间桶（rollup）
与轨迹连续性校验（vmax）。

职责划分：本组件 = 算法与变化逻辑；storage（entitytree）= 增删改查；
geocore = 空间几何（锚点路径码邻近）。

**完整文档（单一事实源）在 `D:\manufacture\docs\entity\`：**

- [README.md](../../docs/entity/README.md) — 子系统总览、系统位置、能力清单
- [设计文档.md](../../docs/entity/设计文档.md) — 设计思想、三层模型、算法公式、调参指导
- [使用文档.md](../../docs/entity/使用文档.md) — CMake 集成、API 参考、调用场景、部署

## 构建与运行

依赖同机源码树中的 storage 与 geocore（add_subdirectory 引入，
路径可用 `-DENTITY_STORAGE_DIR=` / `-DENTITY_GEOCORE_DIR=` 覆盖）。

```bash
cmake -B build -G Ninja
cmake --build build

./build/entity_tests.exe   # resolver 一致性套件（memory + sqlite，298 项检查）
./build/demo_entity.exe    # 演示（生成 demo_entity.db）
ctest --test-dir build
```
