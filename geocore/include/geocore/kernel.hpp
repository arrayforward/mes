// ============================================================================
// kernel.hpp —— Kernel：空间几何内核的统一门面（规范 §7 API 参考）
// ============================================================================
//
// 【本文件的实现思路】
// Kernel 装配三层架构（场景注册表 / 路径码锚点树 / 对象桶）并实现规范
// §7 的全部公开 API。核心算法与规范的对应关系：
//   AnchorOf        —— 算法 S1：点定位，纯函数 O(d)，逐层量化不查表；
//   TransformAt     —— 算法 S2：虚拟/物化统一入口（委托 AnchorTree）；
//   ConvertPoint    —— 算法 S3：LCA 行走（纪律 P5：不绕根）；
//   ConvertVelocity —— 算法 S4：含参考系牵连项 ω×(R·p) + ȯ(t)（论文 §7.4）；
//   Convert         —— 算法 S5：统一入口，场景仲裁 + 近亲行走 / 远亲烘焙 /
//                        热场景对合成矩阵缓存；
//   Find            —— 算法 S6：两阶段（结构层收集 + 数据层平方域过滤）；
//   Transfer / Save / Remove —— 算法 S7：先写后删（INV-5）、写入即规范化
//                        （P4）、惰性物化与回收（INV-1）。
//
// 并发纪律（规范 §6）：
//   - 全局锁序：objects_ 锁 → 锚点树锁 → 桶锁，所有路径单向，无死锁；
//   - Transfer 全程持 objects_ 写锁 + 锚点树写锁：读者（Find 持
//     objects_ 读锁与树读锁）要么看到迁移前、要么看到迁移后（INV-5）；
//   - Find 对桶中 id 查不到对象记录的情况静默跳过：Save（先插桶后登记）
//     与 Remove（先删记录后清桶）的中间态由此兜底，无需额外同步。
//
// 与规范的偏差声明（README 同步声明）：
//   - FrameCache 为内核级单例加锁（规范为每线程一份，语义等价）；
//   - MovingFrame1D 的图卡为恒等（线路样条投影属外围模块）；
//   - 对象几何尺寸不显式存储（规范 §3.5），归属深度隐式表达。
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "geocore/anchor_path.hpp"
#include "geocore/anchor_tree.hpp"
#include "geocore/coordinate_model.hpp"
#include "geocore/frame_cache.hpp"
#include "geocore/kinematic.hpp"
#include "geocore/math.hpp"
#include "geocore/scene.hpp"

namespace geocore {

using ObjectId = uint64_t;
inline constexpr ObjectId kInvalidObject = 0;

// ObjectRecord：对象记录（规范 §3.5）。pos/vel 为归属锚点局部坐标，
// 以所属场景的 CoordinateModel 原生形式存储且已 Normalize（INV-3）。
struct ObjectRecord {
    ObjectId   id = kInvalidObject;
    AnchorPath anchor;   // 归属路径（INV-2：尺度恰好容纳的最深节点）
    Vec3d      pos;
    Vec3d      vel;
};

// Find 的状态码：休眠场景查对象返回错误而非崩溃（T-F-休眠，规范 §8.3）
enum class FindStatus : uint8_t { Ok, SceneNotFound, SceneNotLoaded };

struct FindResult {
    FindStatus            status = FindStatus::Ok;
    std::vector<ObjectId> hits;   // 按距离升序（算法 S6 阶段二）
};

// ----------------------------------------------------------------------------
// Kernel：内核门面
// ----------------------------------------------------------------------------
class Kernel {
public:
    Kernel();

    // ------------------------------------------------------------------
    // CreateScene（规范 §7）：注册场景并烘焙 rootTransform（P8）
    // rootPath 不得与既有场景互相包含；显式声明父子场景时传
    // allowNested=true（规范 §7"除非显式声明父子场景"）——此时 rootPath
    // 可以是已物化的语义锚点（如把行星表面场景挂到运动行星锚点上），
    // 挂载不覆盖该锚点既有运动学。冲突返回 kInvalidScene。
    // kinematic 缺省时挂载点保持规则生成的静态变换。
    // ------------------------------------------------------------------
    SceneId CreateScene(const std::string& name, uint64_t tagMask,
                        const AnchorPath& rootPath, const CoordinateModel& model,
                        const std::optional<Kinematic>& kinematic = std::nullopt,
                        bool allowNested = false);

    // SetSceneState：Resident/Loaded/Dormant/Unloaded；Dormant 保留元数据、
    // 释放物化数据（含其下对象记录，规范 §3.4 注释）。
    void SetSceneState(SceneId id, SceneState st);

    // CreateAnchor：在 parent 下创建语义锚点（命名、可携带运动学；
    // 公理 A3——只有语义锚点可动）。返回新锚点路径。
    AnchorPath CreateAnchor(const AnchorPath& parent, const Kinematic& kin);

    // ------------------------------------------------------------------
    // Save（规范 §7）：Normalize（P4）→ AnchorOf（INV-2 最深容纳）→
    // 惰性物化父链 → 插桶。objSize ≤ 0 时按默认最小粒度定位。
    // 场景未加载返回 kInvalidObject。
    // ------------------------------------------------------------------
    ObjectId Save(SceneId sid, Vec3d posNative, Vec3d vel, double objSize);

    // Remove：删桶项；桶空后惰性回收物化节点（语义节点除外，INV-1）。
    void Remove(ObjectId id);

    // ------------------------------------------------------------------
    // Transfer（算法 S7）：对象跨锚点迁移
    // 伪码：
    //   p' = ConvertPoint   (o.anchor, to, o.pos, t)
    //   v' = ConvertVelocity(o.anchor, to, o.pos, o.vel, t)  // 含参考系项
    //   p' = SceneOf(to).model.Normalize(p')                // P4
    //   BucketAt(to).Insert(o.id)      // 1. 先写新
    //   BucketAt(o.anchor).Remove(o.id)// 2. 后删旧
    //   o.anchor = to; o.pos = p'; o.vel = v'               // 3. 提交归属
    // 位置速度成对换算，刻意不提供"只换坐标"入口（规范 §9.3 错误预警）。
    // ------------------------------------------------------------------
    void Transfer(ObjectId id, const AnchorPath& to, double t);

    // ------------------------------------------------------------------
    // Find（算法 S6）：场景内半径近邻查询
    //   center 为场景根原生坐标（半径语义归场景）；ctx 为查询上下文锚点，
    //   用于帧缓存的路径局部性（FrameCache.lastQueryPath）。
    //   topN < SIZE_MAX 时 partial_sort 只取前 N 个最近者。
    //   候选收集为"同层 + 祖先 + 后代子树"三段覆盖（完备性见 kernel.cpp
    //   阶段一注释）；INV-2 保证对象不重复命中，无运行期去重集合。
    // ------------------------------------------------------------------
    FindResult Find(SceneId sid, const AnchorPath& ctx, Vec3d center,
                    double radius, double t, size_t topN = SIZE_MAX);

    // FindCrossScene：中心先过 Convert（S5）到目标场景，半径以边界点
    // 重估（有界近似，规范 §4.5 声明）；祖先重叠由 INV-2 保证无重复命中。
    FindResult FindCrossScene(SceneId target, const AnchorPath& ctx,
                              Vec3d center, double radius, double t,
                              size_t topN = SIZE_MAX);

    // ------------------------------------------------------------------
    // Convert（算法 S5）：跨锚点/跨场景统一换算入口
    // 伪码：
    //   sa = OwnerOf(from); sb = OwnerOf(to)
    //   if sa == sb: return ConvertPoint(from, to, p, t)
    //   if lca ≥ min(d_a, d_b) − 2: return ConvertPoint(...)  // 近亲：行走
    //   if 双端烘焙有效: return M_hot(sa,sb) · 场景内短行走   // 远亲：O(1)
    //   return ConvertPoint(...)                              // 兜底：行走
    // ------------------------------------------------------------------
    Vec3d Convert(const AnchorPath& from, const AnchorPath& to, Vec3d p, double t);

    // ConvertPoint（算法 S3）：LCA 行走
    // 伪码：
    //   lca = LcaDepth(from, to)
    //   上行（子→父）：p ← R·p + o；过场景根先施加图卡 ToParent（A4）
    //   下行（父→子）：p ← R⁻¹·(p − o)；入场景根后施加图卡 FromParent
    Vec3d ConvertPoint(const AnchorPath& from, const AnchorPath& to,
                       Vec3d p, double t);

    // ConvertDirection（算法 S3 变体）：方向向量无平移，仅逐层旋转。
    // 已知边界：图卡边界的方向畸变（球面度→米）未作用，方向换算请以
    // 笛卡尔坐标为准（README 声明）。
    Vec3d ConvertDirection(const AnchorPath& from, const AnchorPath& to,
                           Vec3d v, double t);

    // ------------------------------------------------------------------
    // ConvertVelocity（算法 S4，论文 §7.4）：含参考系牵连项
    // 伪码：
    //   上行每步：v ← R·v + ω×(R·p) + ȯ(t)；p ← R·p + o
    //   下行每步：v ← R⁻¹·(v − ȯ) − (R⁻¹ω)×p_child；p ← p_child
    //   图卡边界：速度过雅可比 J（数值中心差分，有界近似）
    // 铁律（P3）：与 ConvertPoint 同帧同 t，成对调用。
    // ------------------------------------------------------------------
    Vec3d ConvertVelocity(const AnchorPath& from, const AnchorPath& to,
                          Vec3d p, Vec3d v, double t);

    // BeginFrame（规范 §7）：推进帧缓存（INV-4）；一帧内所有换算共享此 t（P3）
    void BeginFrame(double t);

    // OwnerOf（规范 §7）：rootPath 最长前缀匹配；kInvalidScene = 全局公共区域
    SceneId OwnerOf(const AnchorPath& path) const { return registry_.OwnerOf(path); }

    // ------------------------------------------------------------------
    // 诊断与测试入口（不属于规范 §7，供单元/端到端测试与外围模块使用）
    // ------------------------------------------------------------------
    AnchorPath AnchorOf(SceneId sid, Vec3d pSceneRoot, double eps,
                        Vec3d* outLocal = nullptr) const;  // 算法 S1
    Transform TransformAt(const AnchorPath& path, double t) const {
        return tree_.TransformAt(path, t);
    }
    bool GetObject(ObjectId id, ObjectRecord& out) const;
    const Scene* GetScene(SceneId id) const { return registry_.Get(id); }
    AnchorTree& Tree() { return tree_; }
    const AnchorTree& Tree() const { return tree_; }
    SceneRegistry& Registry() { return registry_; }
    const SceneRegistry& Registry() const { return registry_; }
    size_t ObjectCount() const;

private:
    // 场景内短行走：from → 场景根的累积变换（带 FrameCache，INV-4）
    Transform LocalToSceneRoot(const AnchorPath& path,
                               const AnchorPath& sceneRoot, double t);

    // 热场景对合成矩阵 M(sb,sa) = invRoot(sb) ∘ root(sa)（规范 §4.4）；
    // 失效条件 = 任一方 rootTransform 变更（注册事件驱动 → CreateScene 清空）
    Transform HotPair(SceneId sa, SceneId sb);

    SceneRegistry registry_;
    AnchorTree    tree_;

    FrameCache        cache_;                 // 帧缓存（INV-4），见 frame_cache.hpp
    mutable std::mutex cacheMtx_;

    std::unordered_map<ObjectId, ObjectRecord> objects_;  // 对象主表
    mutable std::shared_mutex objectsMtx_;

    // 热场景对缓存：key = (sa << 32) | sb
    std::unordered_map<uint64_t, Transform> hotPairs_;
    mutable std::shared_mutex hotMtx_;

    std::atomic<ObjectId> nextId_{1};
    std::atomic<uint32_t> nextSemanticCode_{kSemanticCodeBase};
};

} // namespace geocore
