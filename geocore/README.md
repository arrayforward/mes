# geocore —— 空间几何内核（参考实现）

面向多尺度嵌套参考系的点目标空间索引内核。本仓库是配套论文与规范
（`docs/original/`）的 C++17 参考实现：

- 《面向多尺度嵌套参考系的空间几何内核》（论文，`docs/original/paper.html`）
- 《空间几何内核：算法与架构说明书 v1.0》（规范，`docs/original/spec.html`）

内核解决的问题：**在层级化（可运动、可非欧）嵌套参考系下，对点目标进行
定位、索引、迁移与半径近邻查询**，尺度动态范围不受浮点限制——同一棵树上
可以同时存在 10¹¹ m 的行星轨道与 10⁻² m 的桌面螺丝。

## 三层架构

```
场景注册表 SceneRegistry  （~10³，准静止）  主表 + 名字哈希 + 标签倒排 + SoA 包围球
        │ rootPath（唯一接口）
路径码锚点树 AnchorTree    （物化 ~10⁶）     路径码纯函数 + 物化哈希表 + 帧级变换缓存
        │ 归属路径（唯一接口）
对象桶 ObjectRecord        （高频增删迁移）   物化节点内扁平数组 + 桶级读写锁
```

核心思想（详见论文）：

1. **锚点与索引的统一**——八叉树节点即匿名锚点，路径码 = 锚点身份 =
   坐标的纯函数（`AnchorOf`，O(d) 不查表）；树拓扑静止、永不退化。
2. **几何差异的策略化封装**——每个场景是一张图卡（CoordinateModel：
   Euclidean3D / SphericalSurface / Orbital / MovingFrame1D），非欧性
   不穿透场景边界（公理 A4），跨场景换算归结为 LCA 路径行走。
3. **静止红利的全面兑现**——拓扑静止换来缓存合法化（FrameCache）、
   零存储细分（虚拟节点规则生成）、构建期烘焙（rootTransform，P8）。

## 构建与测试

依赖：CMake ≥ 3.16，支持 C++17 的编译器（g++ ≥ 9 / clang ≥ 10 / MSVC ≥ 2019）。
无第三方库依赖（线程库除外）。

```bash
# Linux / WSL
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

当前验证环境：WSL Ubuntu 22.04，g++ 11.4，CMake 4.2。

## 测试覆盖（规范 §8 清单的落地）

| 测试 | 内容 | 位置 |
|---|---|---|
| 单元测试 | AnchorPath 四运算/HashKey、四元数与 Transform 封闭性、P1 圈数取模、模型重建一致性、Normalize 主值化、haversine 基准、邻域展开 | `tests/unit_tests.cpp`（61 项断言） |
| T-INV-1 | 10⁵ 次随机增删后物化最小性（无空洞、无冗余空节点） | `tests/e2e_tests.cpp` |
| T-INV-2 | 归属最深性可复算 + Find 结果无重复 id | 同上 |
| T-INV-3 | 非法坐标（lon=181°、s<0、θ 超界）写入即规范化（P4） | 同上 |
| T-INV-5 | 4 迁移线程 + 2 查询线程并发压测：不丢对象、不重复、不死锁 | 同上 |
| T-P1 | 圆轨道 t=10⁹ s 后相位偏差 < 10⁻¹²（圈数取模） | 同上 |
| T-P3/P5 | 往返换算相对误差 < 10⁻¹²；LCA 路径误差 ≤ 绕根路径 | 同上 |
| T-P-尺度 | 10¹¹ m 与 10⁻² m 锚点互换算，误差不随尺度差恶化 | 同上 |
| T-F-邻域 | 跨 cell 边界半径查询与暴力扫描逐点対拍 | 同上 |
| T-F-球面 | 反经线（lon=±180°）邻域展开、大圆距离対拍、高纬度放宽 | 同上 |
| T-F-列车 | MovingFrame1D 越界钳制 + 沿程距离查询 | 同上 |
| T-F-休眠 | Dormant 场景 Convert 可用、Find 返回"场景未加载"不崩溃 | 同上 |
| T-F-速度 | 自转行星表面静止物体 Transfer 含表面线速度项（≈465 m/s 解析対拍） | 同上 |
| 跨场景 | 远亲烘焙路径 vs 行走路径対拍、热对缓存、FindCrossScene | 同上 |

## API 速览（规范 §7）

```cpp
#include "geocore/kernel.hpp"
using namespace geocore;

Kernel k;

// 注册场景（烘焙 rootTransform，P8）；rootPath 互相包含默认拒绝，
// 显式父子场景传 allowNested=true（如行星表面场景挂到运动行星锚点）
SceneId solar = k.CreateScene("solar", /*tags=*/0, AnchorPath{}.Append(1),
                              Euclidean3D{549755813888.0 /* 2^39 m */});

// 语义锚点：行星（圆轨道 + 自转，相位圈数取模 P1，存周期 P2）
AnchorPath planet = k.CreateAnchor(
    AnchorPath{}.Append(1),
    Kinematic::Orbit(/*r=*/1.5e11, /*T=*/31557600.0, /*phase0=*/0.0,
                     Quaternion::Identity(), /*spin=*/86164.0));

// 父子场景：表面球面场景挂载到行星锚点
SceneId surf = k.CreateScene("surface", 0, planet, SphericalSurface{6371000.0},
                             std::nullopt, /*allowNested=*/true);

k.BeginFrame(0.0);                       // 帧时间推进（INV-4），一帧共享 t（P3）

ObjectId o = k.Save(surf, {31.23, 121.47, 12.0}, /*vel=*/{}, /*size=*/1.0);
FindResult r = k.Find(surf, planet, {31.23, 121.47, 0.0}, /*radius=*/5000.0, 0.0);

k.Transfer(o, /*to=*/someAnchor, 0.0);   // 位置速度成对换算（含参考系项）

Vec3d p2 = k.Convert(from, to, p, 0.0);  // 跨锚点/跨场景统一换算（LCA 行走）
k.Remove(o);
k.SetSceneState(surf, SceneState::Dormant);  // 释放物化数据，保留元数据
```

`FindResult.status`：`Ok / SceneNotFound / SceneNotLoaded`（休眠场景查对象
返回错误而非崩溃）。`Find` 结果按距离升序，`topN` 截断为最近 N 个。

## 精度纪律（规范表 3）的实现位置

| 纪律 | 落实 |
|---|---|
| P1 圈数取模 | `Kinematic::OriginAt/RotationAt`：相位存圈数，求值前 `fmod(·,1.0)` |
| P2 存周期不存角速度 | `Kinematic.period/spinPeriod`，`t/T` 一次除法 |
| P3 同一时刻求值 | 全部换算 API 强制传入帧时间 `t`，无无 t 重载 |
| P4 写入即规范化 | `Save`/`Transfer` 过 `ModelNormalize`；读取不隐式修正 |
| P5 LCA 行走不绕根 | `ConvertPoint` 链长 = d_a+d_b−2·lca；烘焙仅在远亲启用 |
| P7 平方域比较 | Find 过滤排序全程 `DistanceSq`，仅结果需要时开方 |
| P8 烘焙用 double 合成 | `CreateScene` 注册时全程 double 连乘，一次落盘 |

（P6 局部 float32 降格为可选优化，本参考实现未启用。）

## 目录结构

```
include/geocore/
  math.hpp              Vec3d / Quaternion / Transform / BoundingSphere
  anchor_path.hpp       路径码：Parent/Prefix/IsPrefixOf/HashKey/LcaDepth
                          + ToString/ParseAnchorPath（字符串序列化/解析）
  kinematic.hpp         Static / CircularOrbit / PathConstrained（P1/P2）
  coordinate_model.hpp  四个内置坐标模型 + variant 静态分派（禁虚函数）
  anchor_tree.hpp       物化哈希表 + 虚拟节点规则生成 + 桶级锁
  scene.hpp             场景记录与注册表（主表/名字/标签/SoA/OwnerOf）
  frame_cache.hpp       帧级缓存（INV-4）
  kernel.hpp            统一门面 API（规范 §7）
src/                    anchor_tree.cpp / scene.cpp / kernel.cpp
tests/                  check.hpp（极简断言框架）/ unit_tests.cpp / e2e_tests.cpp
docs/original/          论文、规范、设计讨论原始文档
```

## 与规范的偏差及已知边界

实现层面的有意取舍（均在代码注释中同步声明）：

- **FrameCache 为内核级单例加锁**：规范为"每线程一份"（免锁优化形态），
  语义与 INV-4 等价；高并发管线可按规范 §6 改造为线程本地缓存。
- **MovingFrame1D 图卡为恒等**：(s,lat,h) 直接作为父系局部坐标；真实
  线路的弧长→曲面投影属外围样条模块。`Normalize` 对 s 越界选"钳制"
  （规范允许的两种行为之一），段间 Transfer 由上层业务触发。
- **图卡雅可比为数值中心差分**：速度跨界换算（算法 S4）在极点/奇异
  邻域精度下降，属有界近似（规范 §4.5 对半径重估的同类声明）。
- **Find 候选收集 = 同层 + 祖先 + 后代子树三段覆盖**：规范 S6 伪码只
  展示同层展开，但 INV-2（对象挂在最深容纳节点）下完整语义必须覆盖
  祖先桶与物化子树（规范 §4.5"祖先重叠由 INV-2 保证不产生重复命中"
  正是此意）；去重针对路径而非对象 id。
- **HashKey 打包路径限深 18 层**：3 bit×18 + 8 bit 深度 ≤ 64 bit；
  更深或含语义段的路径走 FNV-1a 并置最高位隔离，哈希表以完整路径
  做相等比较兜底。
- **Dormant 释放物化数据时一并释放其下对象记录**：对象持久化属外围
  模块职责（规范 §10 边界外）。

规范 §9.2 划定的七类边界外问题（时空历史、拓扑可达、射线求交、全配对、
高维检索、分布式一致、位置不确定性）不在本内核实现范围。

## 许可

MIT License，见 `LICENSE`。
