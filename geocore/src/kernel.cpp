// ============================================================================
// kernel.cpp —— Kernel 实现：算法 S1–S7 的完整落地（对应 kernel.hpp）
// ============================================================================
//
// 【本文件的实现思路】
// 本文件是规范第 4 章（核心算法）的逐条实现，注释中标注每条代码对应的
// 算法编号与精度纪律。三个贯穿性决策：
//   1. 一切换算走 LCA 行走（P5），烘焙矩阵只在"远亲且双端烘焙有效"时
//      启用，其余情况行走兜底——烘焙是优化，行走是正确性基准；
//   2. 图卡边界（A4）在行走中显式处理：上行情景根先 ToParent、下行进
//      场景根后 FromParent，速度换算另加数值雅可比；
//   3. 所有锁序单向（objects_ → 树 → 桶），Transfer 的原子性靠
//      "写新→删旧→提交"三步在写锁内完成（INV-5）。
// ============================================================================
#include "geocore/kernel.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace geocore {

namespace {

// 场景名义半径（粗判包围球用）：各模型按覆盖范围给出上界
double ModelNominalRadius(const CoordinateModel& m) {
    return std::visit(
        [](const auto& v) -> double {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, Euclidean3D>)
                return v.extent * 0.8660254037844386;  // √3/2·边长
            else if constexpr (std::is_same_v<T, SphericalSurface>)
                return 2.0 * v.radius;
            else if constexpr (std::is_same_v<T, Orbital>)
                return 2.0 * v.rMax;
            else
                return v.length;
        },
        m);
}

// Save 的粒度下限：objSize ≤ 0 时按 1mm 定位，防止无意义的全深度细分
constexpr double kMinEps = 1e-3;

} // namespace

// Kernel 构造：完成锚点树 ↔ 注册表的双向接线
Kernel::Kernel() { tree_.SetRegistry(&registry_); }

// ============================================================================
// AnchorOf（算法 S1）：点定位，纯函数 O(d)
// 伪码（规范 §4.1）：
//   path = sc.rootPath；q = p − RootCenter        // 进入场景根局部坐标
//   while CellSize(d)/2 > eps:                     // 归宿：cell ∈ (eps, 2eps]
//       c = model.CellOf(q)；q -= model.ChildOrigin(c, d)；path ⊕ c
//   return path                                     // 不查任何表
// outLocal 输出残余局部坐标（Save 的存储值，INV-3 原生 + 局部）
// ============================================================================
AnchorPath Kernel::AnchorOf(SceneId sid, Vec3d pSceneRoot, double eps,
                            Vec3d* outLocal) const {
    const Scene* sc = registry_.Get(sid);
    if (!sc) return {};
    if (eps < kMinEps) eps = kMinEps;
    AnchorPath path = sc->rootPath;
    Vec3d q = pSceneRoot - ModelRootCenter(sc->model);
    int d = 0;  // 场景内层深
    while (ModelCellSizeMeters(sc->model, d) * 0.5 > eps &&
           path.depth < kMaxDepth) {
        uint32_t c = ModelCellOf(sc->model, q);
        q -= ModelChildOrigin(sc->model, c, d);  // 进入子区局部坐标
        path.AppendInPlace(c);
        ++d;
    }
    if (outLocal) *outLocal = q;
    return path;
}

// ============================================================================
// CreateScene（规范 §7）：物化挂载点 → 烘焙 rootTransform（P8）→ 注册
// ============================================================================
SceneId Kernel::CreateScene(const std::string& name, uint64_t tagMask,
                            const AnchorPath& rootPath,
                            const CoordinateModel& model,
                            const std::optional<Kinematic>& kinematic,
                            bool allowNested) {
    // 冲突快速预判（注册表 AddScene 还会完整校验）：已被既有场景覆盖的路径
    if (!allowNested && registry_.OwnerOf(rootPath) != kInvalidScene)
        return kInvalidScene;

    // 1. 物化挂载点为语义节点。若路径已物化（父子场景挂载到既有语义
    //    锚点），保留其运动学——运动定义权属于先到的锚点创建者。
    if (!tree_.IsMaterialized(rootPath)) {
        Kinematic kin = kinematic ? *kinematic : tree_.KinematicOf(rootPath);
        tree_.MaterializeSemantic(rootPath, kin);
    }
    Kinematic kin = tree_.KinematicOf(rootPath);

    // 2. 烘焙：从全局根到场景根 double 全程连乘（P8），一次性落盘。
    //    烘焙合法性（论文 §7.3）：链上全程静态 ∧ 不经过非恒等图卡。
    Transform acc = Transform::Identity();
    bool bakedValid = true;
    for (int d = 1; d <= rootPath.depth; ++d) {
        AnchorPath pref = rootPath.Prefix(d);
        if (pref != rootPath && registry_.IsSceneRoot(pref)) {
            const Scene* host = registry_.OwnerScene(pref);
            if (host && !ModelIdentityChart(host->model)) bakedValid = false;
        }
        Kinematic k = tree_.KinematicOf(pref);
        if (!k.IsStatic()) bakedValid = false;  // 运动链按帧重算，不可烘焙
        Transform tr{k.OriginAt(0.0), k.RotationAt(0.0)};
        acc = Transform::Compose(tr, acc);
    }

    // 3. 粗判包围球：运动场景取扫掠包络（圆轨道 = 轨道环面，注册时算一次）
    Scene sc;
    sc.name = name;
    sc.tagMask = tagMask;
    sc.rootPath = rootPath;
    sc.rootTransform = acc;
    sc.invRootTransform = acc.Inverse();
    sc.bakedValid = bakedValid;
    sc.model = model;
    sc.bounds.center = acc.Apply(Vec3d{});
    sc.bounds.radius = ModelNominalRadius(model) +
                       (kin.type == Kinematic::Type::CircularOrbit ? kin.orbitRadius
                                                                   : 0.0);

    SceneId id = registry_.AddScene(std::move(sc), allowNested);
    if (id == kInvalidScene) {
        tree_.ForceErase(rootPath);  // 回滚物化，目录层拒绝不留下痕迹
        return id;
    }
    // 热场景对缓存失效条件 = 注册事件（规范 §4.4）
    std::unique_lock<std::shared_mutex> hk(hotMtx_);
    hotPairs_.clear();
    return id;
}

// SetSceneState：Dormant/Unloaded 释放物化数据（元数据保留，T-F-休眠）
void Kernel::SetSceneState(SceneId id, SceneState st) {
    const Scene* sc = registry_.Get(id);
    if (!sc) return;
    if (st == SceneState::Dormant || st == SceneState::Unloaded) {
        AnchorPath root = sc->rootPath;
        tree_.EraseDescendants(root);
        // 对象记录属物化数据，一并释放（持久化属外围模块职责）
        std::unique_lock<std::shared_mutex> lk(objectsMtx_);
        for (auto it = objects_.begin(); it != objects_.end();) {
            if (it->second.anchor.depth > root.depth &&
                root.IsPrefixOf(it->second.anchor))
                it = objects_.erase(it);
            else
                ++it;
        }
    }
    registry_.SetStateField(id, st);
}

// CreateAnchor：语义锚点创建（低频事件；段码从独立命名空间分配）
AnchorPath Kernel::CreateAnchor(const AnchorPath& parent,
                                const Kinematic& kin) {
    AnchorPath path = parent.Append(nextSemanticCode_++);
    tree_.MaterializeSemantic(path, kin);
    return path;
}

// ============================================================================
// Save（规范 §7）：Normalize（P4）→ AnchorOf（INV-2）→ 惰性物化 → 插桶
// ============================================================================
ObjectId Kernel::Save(SceneId sid, Vec3d posNative, Vec3d vel, double objSize) {
    const Scene* sc = registry_.Get(sid);
    if (!sc || sc->state == SceneState::Dormant ||
        sc->state == SceneState::Unloaded)
        return kInvalidObject;
    Vec3d pos = ModelNormalize(sc->model, posNative);  // P4：写入即规范化
    Vec3d local;
    AnchorPath path = AnchorOf(sid, pos, objSize, &local);  // INV-2：最深容纳
    tree_.MaterializeChain(path);  // 惰性物化父链（规范 S7-Save）
    ObjectId id = nextId_++;
    tree_.BucketInsert(path, id);  // 先插桶：Find 对查不到记录的 id 静默跳过
    std::unique_lock<std::shared_mutex> lk(objectsMtx_);
    objects_[id] = ObjectRecord{id, path, local, vel};  // 残余局部坐标（INV-3）
    return id;
}

// Remove：先删记录后清桶（与 Find 的读路径配合无中间态泄漏）
void Kernel::Remove(ObjectId id) {
    AnchorPath anchor;
    {
        std::unique_lock<std::shared_mutex> lk(objectsMtx_);
        auto it = objects_.find(id);
        if (it == objects_.end()) return;
        anchor = it->second.anchor;
        objects_.erase(it);
    }
    tree_.BucketRemove(anchor, id);
    tree_.PruneFrom(anchor);  // 惰性回收（INV-1，语义节点除外）
}

// ============================================================================
// Transfer（算法 S7）：先写后删的原子迁移（INV-5）
// ============================================================================
void Kernel::Transfer(ObjectId id, const AnchorPath& to, double t) {
    // 锁序：objects_ 写锁全程持有（内部换算取树读锁、迁移取树写锁，
    // 方向单向 objects_ → 树，与 Find 一致，无死锁）
    std::unique_lock<std::shared_mutex> objLk(objectsMtx_);
    auto it = objects_.find(id);
    if (it == objects_.end()) return;
    ObjectRecord& rec = it->second;

    // 位置速度成对换算（规范 §9.3：刻意无"只换坐标"入口）
    Vec3d p = ConvertPoint(rec.anchor, to, rec.pos, t);
    Vec3d v = ConvertVelocity(rec.anchor, to, rec.pos, rec.vel, t);

    // P4 写入即规范化：残余坐标天然在 cell 半宽内，回卷型 Normalize 为
    // 恒等；钳制型（MovingFrame1D 的 s ∈ [0,length]）作用于局部残余会
    // 产生语义错误，此处跳过（钳制语义仅在 Save 的绝对坐标上有意义）。
    const Scene* sc = registry_.OwnerScene(to);
    if (sc && !std::holds_alternative<MovingFrame1D>(sc->model))
        p = ModelNormalize(sc->model, p);

    {
        auto treeLk = tree_.UniqueTreeLock();     // 迁移原子区（INV-5）
        tree_.MaterializeChainLocked(to);
        tree_.BucketInsertLocked(to, id);          // 1. 先写新
        tree_.BucketRemoveLocked(rec.anchor, id);  // 2. 后删旧
        tree_.PruneFromLocked(rec.anchor);         // 惰性回收（INV-1）
    }
    rec.anchor = to;  // 3. 提交归属
    rec.pos = p;
    rec.vel = v;
}

// ============================================================================
// ConvertPoint（算法 S3）：LCA 行走（P5：不绕根）
// ============================================================================
Vec3d Kernel::ConvertPoint(const AnchorPath& from, const AnchorPath& to,
                           Vec3d p, double t) {
    int lca = LcaDepth(from, to);
    // 上行（子系 → 父系）：p ← R·p + o
    AnchorPath cur = from;
    while (cur.depth > lca) {
        // 图卡边界（A4）：场景根原生坐标先欧氏化再越界
        if (registry_.IsSceneRoot(cur)) {
            const Scene* sc = registry_.OwnerScene(cur);
            if (sc) p = ModelToParent(sc->model, p);
        }
        Transform tr = tree_.TransformAt(cur, t);
        p = tr.rotation * p + tr.origin;
        cur = cur.Parent();
    }
    // 下行（父系 → 子系）：p ← R⁻¹·(p − o)
    for (int d = lca + 1; d <= to.depth; ++d) {
        AnchorPath pref = to.Prefix(d);
        Transform tr = tree_.TransformAt(pref, t);
        p = tr.rotation.Conjugate() * (p - tr.origin);
        if (registry_.IsSceneRoot(pref)) {
            const Scene* sc = registry_.OwnerScene(pref);
            if (sc) p = ModelFromParent(sc->model, p);
        }
    }
    return p;
}

// ConvertDirection：无平移，仅逐层旋转（图卡畸变不作用，见头文件声明）
Vec3d Kernel::ConvertDirection(const AnchorPath& from, const AnchorPath& to,
                               Vec3d v, double t) {
    int lca = LcaDepth(from, to);
    AnchorPath cur = from;
    while (cur.depth > lca) {
        v = tree_.TransformAt(cur, t).rotation * v;
        cur = cur.Parent();
    }
    for (int d = lca + 1; d <= to.depth; ++d) {
        v = tree_.TransformAt(to.Prefix(d), t).rotation.Conjugate() * v;
    }
    return v;
}

// ============================================================================
// ConvertVelocity（算法 S4，论文 §7.4）：含参考系牵连项
// 上行：v ← R·v + ω×(R·p) + ȯ(t)；下行取逆（推导见 kernel.hpp 伪码）。
// 与 ConvertPoint 同帧同 t（P3 铁律），成对调用。
// ============================================================================
Vec3d Kernel::ConvertVelocity(const AnchorPath& from, const AnchorPath& to,
                              Vec3d p, Vec3d v, double t) {
    int lca = LcaDepth(from, to);
    AnchorPath cur = from;
    while (cur.depth > lca) {
        if (registry_.IsSceneRoot(cur)) {
            const Scene* sc = registry_.OwnerScene(cur);
            if (sc) {  // 速度过图卡雅可比（数值中心差分，有界近似）
                v = ModelChartVelToParent(sc->model, p, v);
                p = ModelToParent(sc->model, p);
            }
        }
        Transform tr = tree_.TransformAt(cur, t);
        Kinematic kin = tree_.KinematicOf(cur);
        Vec3d rp = tr.rotation * p;
        v = tr.rotation * v + kin.AngularVelAt(t).Cross(rp) + kin.OriginVelAt(t);
        p = rp + tr.origin;
        cur = cur.Parent();
    }
    for (int d = lca + 1; d <= to.depth; ++d) {
        AnchorPath pref = to.Prefix(d);
        Transform tr = tree_.TransformAt(pref, t);
        Kinematic kin = tree_.KinematicOf(pref);
        Quaternion rc = tr.rotation.Conjugate();
        Vec3d pc = rc * (p - tr.origin);              // 子系坐标
        v = rc * (v - kin.OriginVelAt(t)) - (rc * kin.AngularVelAt(t)).Cross(pc);
        p = pc;
        if (registry_.IsSceneRoot(pref)) {
            const Scene* sc = registry_.OwnerScene(pref);
            if (sc) {
                v = ModelChartVelFromParent(sc->model, p, v);
                p = ModelFromParent(sc->model, p);
            }
        }
    }
    return v;
}

// ============================================================================
// LocalToSceneRoot：物化节点 → 场景根的累积变换（FrameCache，INV-4）
// 伪码：
//   if path == sceneRoot: return 单位变换
//   if cache 命中: return 缓存
//   return cache[path] = Compose(LocalToSceneRoot(parent), TransformAt(path))
// 帧内复用；帧切换由 BeginFrame 整体失效。写入路径不读本缓存（INV-4）。
// ============================================================================
Transform Kernel::LocalToSceneRoot(const AnchorPath& path,
                                   const AnchorPath& sceneRoot, double t) {
    if (path == sceneRoot) return Transform::Identity();
    {
        std::lock_guard<std::mutex> lk(cacheMtx_);
        auto it = cache_.toSceneRoot.find(path);
        if (it != cache_.toSceneRoot.end()) return it->second;
    }
    Transform tr = tree_.TransformAt(path, t);
    Transform parent = LocalToSceneRoot(path.Parent(), sceneRoot, t);
    Transform res = Transform::Compose(parent, tr);
    std::lock_guard<std::mutex> lk(cacheMtx_);
    cache_.toSceneRoot.emplace(path, res);
    return res;
}

// HotPair：热场景对合成矩阵缓存（规范 §4.4），跨场景换算降为一次矩阵乘法
Transform Kernel::HotPair(SceneId sa, SceneId sb) {
    uint64_t key = (static_cast<uint64_t>(sa) << 32) | sb;
    {
        std::shared_lock<std::shared_mutex> lk(hotMtx_);
        auto it = hotPairs_.find(key);
        if (it != hotPairs_.end()) return it->second;
    }
    const Scene* a = registry_.Get(sa);
    const Scene* b = registry_.Get(sb);
    Transform m = Transform::Compose(b->invRootTransform, a->rootTransform);
    std::unique_lock<std::shared_mutex> lk(hotMtx_);
    hotPairs_.emplace(key, m);
    return m;
}

// ============================================================================
// Convert（算法 S5）：统一入口，含场景仲裁
// ============================================================================
Vec3d Kernel::Convert(const AnchorPath& from, const AnchorPath& to, Vec3d p,
                      double t) {
    SceneId sa = registry_.OwnerOf(from), sb = registry_.OwnerOf(to);
    if (sa == sb) return ConvertPoint(from, to, p, t);
    int lca = LcaDepth(from, to);
    int mind = from.depth < to.depth ? from.depth : to.depth;
    if (lca >= mind - 2)  // 近亲：行走（LCA 深，精度好，规范 §4.5）
        return ConvertPoint(from, to, p, t);
    const Scene* a = registry_.Get(sa);
    const Scene* b = registry_.Get(sb);
    if (a && b && a->bakedValid && b->bakedValid) {
        // 远亲 + 双端烘焙有效：场景内短行走 → 图卡 → 合成矩阵 → 图卡 → 下行
        Vec3d pa = LocalToSceneRoot(from, a->rootPath, t) * p;
        Vec3d paC = ModelToParent(a->model, pa);
        Vec3d pbC = HotPair(sa, sb) * paC;
        Vec3d pb = ModelFromParent(b->model, pbC);
        for (int d = b->rootPath.depth + 1; d <= to.depth; ++d) {
            Transform tr = tree_.TransformAt(to.Prefix(d), t);
            pb = tr.rotation.Conjugate() * (pb - tr.origin);
        }
        return pb;
    }
    return ConvertPoint(from, to, p, t);  // 兜底：全程行走
}

// ============================================================================
// Find（算法 S6）：两阶段半径近邻查询
// ============================================================================
FindResult Kernel::Find(SceneId sid, const AnchorPath& ctx, Vec3d center,
                        double radius, double t, size_t topN) {
    const Scene* sc = registry_.Get(sid);
    if (!sc) return {FindStatus::SceneNotFound, {}};
    if (sc->state == SceneState::Dormant || sc->state == SceneState::Unloaded)
        return {FindStatus::SceneNotLoaded, {}};  // T-F-休眠：报错而非崩溃

    // 路径缓存：时间局部性提示（论文 §5.4）
    {
        std::lock_guard<std::mutex> lk(cacheMtx_);
        cache_.lastQueryPath = ctx;
    }

    // ---- 阶段一：定位与候选收集（结构层）----
    // base cell ∈ (radius, 2·radius]，邻域展开保证球域在同层全覆盖。
    // 完整性关键（规范 §4.5"祖先重叠由 INV-2 保证不产生重复命中"）：
    // 对象挂在尺度最匹配的最深节点（INV-2），其锚点层级可高于、等于、
    // 低于查询基准层，因此候选 = 三段覆盖——
    //   1. 同层：邻域展开覆盖的 cell 桶；
    //   2. 祖先：覆盖 cell 的全部前缀桶（大尺度对象，按路径去重，
    //      去的是"路径"而非对象 id——INV-2 下对象天然不重复）；
    //   3. 后代：覆盖 cell 的物化子树（小尺度对象，children 链 DFS）。
    // 完备性论证：球域内对象的锚点 cell 必含对象位置，其基准层前缀
    // 必被邻域展开覆盖，故锚点 ∈ {覆盖 cell} ∪ {其祖先} ∪ {其后代}。
    AnchorPath base = AnchorOf(sid, center, radius);
    int level = base.depth - sc->rootPath.depth;
    std::vector<AnchorPath> cells;
    ModelNeighborCells(sc->model, center, level, radius, sc->rootPath, cells);

    std::vector<ObjectId> cand;
    std::unordered_set<AnchorPath, PathHash> visited;  // 祖先路径去重
    for (const AnchorPath& c : cells) {
        tree_.CollectSubtreeBucketIds(c, cand);  // 同层 + 后代子树
        for (int d = sc->rootPath.depth; d < c.depth; ++d) {
            AnchorPath pref = c.Prefix(d);
            if (visited.insert(pref).second) {   // 祖先（含场景根桶）
                std::vector<uint64_t> b = tree_.BucketCopy(pref);
                cand.insert(cand.end(), b.begin(), b.end());
            }
        }
    }

    // ---- 阶段二：度量过滤与排序（数据层，平方域延迟开方，P7）----
    // 对象坐标换算到场景根 q-空间（相对 RootCenter）后统一比较
    Vec3d centerQ = center - ModelRootCenter(sc->model);
    double r2 = radius * radius;
    std::vector<std::pair<double, ObjectId>> scored;
    {
        std::shared_lock<std::shared_mutex> objLk(objectsMtx_);
        for (ObjectId id : cand) {
            auto it = objects_.find(id);
            if (it == objects_.end()) continue;  // Save/Remove 中间态兜底
            const ObjectRecord& o = it->second;
            Vec3d pq = ConvertPoint(o.anchor, sc->rootPath, o.pos, t);
            double dsq = ModelDistanceSq(sc->model, pq, centerQ);
            if (dsq <= r2) scored.emplace_back(dsq, id);
        }
    }
    auto byDist = [](const auto& x, const auto& y) { return x.first < y.first; };
    if (topN == SIZE_MAX || topN >= scored.size()) {
        std::sort(scored.begin(), scored.end(), byDist);
    } else {
        std::partial_sort(scored.begin(), scored.begin() + topN, scored.end(),
                          byDist);  // Top-N：O(m log N)
        scored.resize(topN);
    }
    FindResult out;
    out.status = FindStatus::Ok;
    out.hits.reserve(scored.size());
    for (const auto& s : scored) out.hits.push_back(s.second);
    return out;
}

// ============================================================================
// FindCrossScene（规范 §4.5）：中心过 S5，半径以边界点重估（有界近似）
// 伪码：
//   cRoot = Convert(ctx, target.rootPath, center, t)
//   r' = max over 6 轴向边界点 b: Dist_model(Convert(b), cRoot)
//   return Find(target, cRoot, r', t)
// 祖先重叠由 INV-2 保证不产生重复命中，无需运行期去重集合。
// ============================================================================
FindResult Kernel::FindCrossScene(SceneId target, const AnchorPath& ctx,
                                  Vec3d center, double radius, double t,
                                  size_t topN) {
    const Scene* sc = registry_.Get(target);
    if (!sc) return {FindStatus::SceneNotFound, {}};
    if (sc->state == SceneState::Dormant || sc->state == SceneState::Unloaded)
        return {FindStatus::SceneNotLoaded, {}};

    Vec3d cRoot = Convert(ctx, sc->rootPath, center, t);
    double rEff = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
        for (double sign : {1.0, -1.0}) {
            Vec3d b = center;
            if (axis == 0) b.x += sign * radius;
            if (axis == 1) b.y += sign * radius;
            if (axis == 2) b.z += sign * radius;
            Vec3d bq = Convert(ctx, sc->rootPath, b, t);
            double d2 = ModelDistanceSq(sc->model, bq, cRoot);
            rEff = std::max(rEff, std::sqrt(d2));
        }
    }
    if (rEff <= 0.0) rEff = radius;  // 退化保护
    Vec3d cAbs = cRoot + ModelRootCenter(sc->model);  // q-空间 → 绝对原生坐标
    return Find(target, sc->rootPath, cAbs, rEff, t, topN);
}

// BeginFrame（INV-4）：t 变化即整体失效
void Kernel::BeginFrame(double t) {
    std::lock_guard<std::mutex> lk(cacheMtx_);
    cache_.BeginFrame(t);
}

// 诊断：对象记录只读拷贝
bool Kernel::GetObject(ObjectId id, ObjectRecord& out) const {
    std::shared_lock<std::shared_mutex> lk(objectsMtx_);
    auto it = objects_.find(id);
    if (it == objects_.end()) return false;
    out = it->second;
    return true;
}

size_t Kernel::ObjectCount() const {
    std::shared_lock<std::shared_mutex> lk(objectsMtx_);
    return objects_.size();
}

} // namespace geocore
