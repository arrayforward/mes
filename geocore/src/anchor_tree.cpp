// ============================================================================
// anchor_tree.cpp —— AnchorTree 实现（对应 anchor_tree.hpp 的设计说明）
// ============================================================================
//
// 【本文件的实现思路】
// 实现规范 §3.5 的物化哈希表与算法 S2 的虚拟/物化统一入口。要点：
//   1. 所有公开查询函数持树读锁（shared_lock），物化/回收持写锁；
//   2. 物化匿名节点的 Kinematic 由规则生成，与虚拟分支严格一致——
//      同一路径物化前后 TransformAt 输出不变（否则对象局部坐标语义
//      会在物化瞬间跳变）；
//   3. 虚拟分支的量化原点依赖场景归属（OwnerOf），全局公共区域回落到
//      内置全局欧氏模型。
// ============================================================================
#include "geocore/anchor_tree.hpp"

#include "geocore/scene.hpp"

namespace geocore {

// ----------------------------------------------------------------------------
// 内部查找（无锁，调用方持锁）；unordered_map 节点桶保证引用稳定
// ----------------------------------------------------------------------------
NodeData* AnchorTree::FindUnlocked(const AnchorPath& path) {
    auto it = nodes_.find(path);
    return it == nodes_.end() ? nullptr : it->second.get();
}
const NodeData* AnchorTree::FindUnlocked(const AnchorPath& path) const {
    auto it = nodes_.find(path);
    return it == nodes_.end() ? nullptr : it->second.get();
}

// ----------------------------------------------------------------------------
// QuantizedChildOrigin：虚拟节点的规则生成原点（公理 A3 的可计算性核心）
// 伪码：
//   sc   = registry.OwnerOf(path)          // 最长前缀匹配；含自身场景
//   d    = (path.depth − 1) − sc.rootPath.depth   // 父节点在场景内的层深
//   return sc.model.ChildOrigin(path.LastSeg(), d)
// ----------------------------------------------------------------------------
Vec3d AnchorTree::QuantizedChildOrigin(const AnchorPath& path) const {
    const Scene* sc = registry_ ? registry_->OwnerScene(path) : nullptr;
    if (!sc) {
        // 全局公共区域：内置欧氏细分
        int d = path.depth - 1;
        return globalModel_.ChildOrigin(path.LastSeg(), d < 0 ? 0 : d);
    }
    int d = (path.depth - 1) - sc->rootPath.depth;
    return ModelChildOrigin(sc->model, path.LastSeg(), d < 0 ? 0 : d);
}

// ----------------------------------------------------------------------------
// TransformAt（算法 S2）：虚拟/物化统一入口
// ----------------------------------------------------------------------------
Transform AnchorTree::TransformAt(const AnchorPath& path, double t) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (const NodeData* nd = FindUnlocked(path)) {
        return {nd->kinematic.OriginAt(t), nd->kinematic.RotationAt(t)};
    }
    // 虚拟节点：规则生成（零存储细分，规范 §9.3：虚拟节点落库是错误）
    return {QuantizedChildOrigin(path), Quaternion::Identity()};
}

// KinematicOf：烘焙链路静态性判定用；虚拟节点等价于规则生成的 Static
Kinematic AnchorTree::KinematicOf(const AnchorPath& path) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (const NodeData* nd = FindUnlocked(path)) return nd->kinematic;
    return Kinematic::Static(QuantizedChildOrigin(path));
}

bool AnchorTree::IsMaterialized(const AnchorPath& path) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    return FindUnlocked(path) != nullptr;
}

// ----------------------------------------------------------------------------
// MaterializeChainLocked：惰性物化父链（调用方须持树写锁）
// 伪码：
//   for d = 1..path.depth:
//       pref = path.Prefix(d)
//       if pref 未物化:
//           nodes_[pref] = NodeData{ path=pref,
//               kinematic=Static(QuantizedChildOrigin(pref)), semantic=false }
//           parent.children.push_back(pref)
// ----------------------------------------------------------------------------
void AnchorTree::MaterializeChainLocked(const AnchorPath& path) {
    for (int d = 1; d <= path.depth; ++d) {
        AnchorPath pref = path.Prefix(d);
        if (FindUnlocked(pref)) continue;
        auto nd = std::make_unique<NodeData>();
        nd->path = pref;
        // 规则生成的 Static Kinematic：与虚拟分支输出严格一致
        nd->kinematic = Kinematic::Static(QuantizedChildOrigin(pref));
        NodeData* raw = nd.get();
        nodes_.emplace(pref, std::move(nd));
        // 登记进父节点的 children（虚拟子节点不登记，规范 §3.5）
        if (NodeData* parent = FindUnlocked(pref.Parent()))
            parent->children.push_back(pref);
        (void)raw;
    }
}

void AnchorTree::MaterializeChain(const AnchorPath& path) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    MaterializeChainLocked(path);
}

// MaterializeSemantic：物化语义节点（场景根 / 命名锚点）
void AnchorTree::MaterializeSemantic(const AnchorPath& path,
                                     const Kinematic& kin) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    MaterializeChainLocked(path);
    NodeData* nd = FindUnlocked(path);
    nd->kinematic = kin;     // 以用户运动学覆盖规则生成值
    nd->semantic = true;     // 语义节点不可自动回收（INV-1 例外）
}

// ----------------------------------------------------------------------------
// PruneFromLocked：惰性回收空物化节点（维持 INV-1 物化最小性）
// 终止条件（任一）：未物化 / semantic / 桶非空 / 有物化子节点 / 场景根
// ----------------------------------------------------------------------------
void AnchorTree::PruneFromLocked(const AnchorPath& path) {
    AnchorPath cur = path;
    while (cur.depth > 0) {
        NodeData* nd = FindUnlocked(cur);
        if (!nd || nd->semantic || !nd->bucket.empty() || !nd->children.empty())
            return;
        if (registry_ && registry_->IsSceneRoot(cur)) return;
        // 从父节点 children 摘除后删除
        if (NodeData* parent = FindUnlocked(cur.Parent())) {
            auto& ch = parent->children;
            for (auto it = ch.begin(); it != ch.end(); ++it) {
                if (*it == cur) { ch.erase(it); break; }
            }
        }
        nodes_.erase(cur);
        cur = cur.Parent();
    }
}

void AnchorTree::PruneFrom(const AnchorPath& path) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    PruneFromLocked(path);
}

// EraseDescendants：场景 Dormant/Unloaded 时释放物化数据（元数据保留）
void AnchorTree::EraseDescendants(const AnchorPath& rootPath) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        const AnchorPath& p = it->first;
        if (p.depth > rootPath.depth && rootPath.IsPrefixOf(p)) {
            // 从父节点 children 摘除（父在删除集外时才需要）
            if (NodeData* parent = FindUnlocked(p.Parent())) {
                auto& ch = parent->children;
                for (auto cit = ch.begin(); cit != ch.end(); ++cit) {
                    if (*cit == p) { ch.erase(cit); break; }
                }
            }
            it = nodes_.erase(it);
        } else {
            ++it;
        }
    }
}

// ----------------------------------------------------------------------------
// 对象桶操作（桶级锁 + 树读锁；锁序：树锁 → 桶锁）
// ----------------------------------------------------------------------------
void AnchorTree::ForceErase(const AnchorPath& path) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    if (FindUnlocked(path)) {
        // 从父节点 children 摘除（语义节点不走 PruneFrom 的常规条件）
        if (NodeData* parent = FindUnlocked(path.Parent())) {
            auto& ch = parent->children;
            for (auto it = ch.begin(); it != ch.end(); ++it) {
                if (*it == path) { ch.erase(it); break; }
            }
        }
        nodes_.erase(path);
    }
    PruneFromLocked(path.Parent());  // 向上回收可能变空的父链
}
void AnchorTree::BucketInsertLocked(const AnchorPath& path, uint64_t id) {
    if (NodeData* nd = FindUnlocked(path)) nd->bucket.push_back(id);
}

void AnchorTree::BucketRemoveLocked(const AnchorPath& path, uint64_t id) {
    if (NodeData* nd = FindUnlocked(path)) {
        auto& b = nd->bucket;
        for (auto it = b.begin(); it != b.end(); ++it) {
            if (*it == id) { b.erase(it); return; }
        }
    }
}

void AnchorTree::BucketInsert(const AnchorPath& path, uint64_t id) {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (NodeData* nd = FindUnlocked(path)) {
        std::unique_lock<std::shared_mutex> nl(nd->mtx);
        nd->bucket.push_back(id);
    }
}

void AnchorTree::BucketRemove(const AnchorPath& path, uint64_t id) {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (NodeData* nd = FindUnlocked(path)) {
        std::unique_lock<std::shared_mutex> nl(nd->mtx);
        auto& b = nd->bucket;
        for (auto it = b.begin(); it != b.end(); ++it) {
            if (*it == id) { b.erase(it); return; }
        }
    }
}

// BucketCopy：Find 阶段一的候选收集；虚拟节点零成本跳过（算法 S6 注释）
std::vector<uint64_t> AnchorTree::BucketCopy(const AnchorPath& path) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (const NodeData* nd = FindUnlocked(path)) {
        std::shared_lock<std::shared_mutex> nl(nd->mtx);
        return nd->bucket;
    }
    return {};
}

// CollectSubtreeBucketIds：DFS 物化子树（children 链），收集全部桶内容
void AnchorTree::CollectSubtreeBucketIds(const AnchorPath& root,
                                         std::vector<uint64_t>& out) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    std::vector<AnchorPath> stack{root};
    while (!stack.empty()) {
        AnchorPath p = stack.back();
        stack.pop_back();
        if (const NodeData* nd = FindUnlocked(p)) {
            std::shared_lock<std::shared_mutex> nl(nd->mtx);
            out.insert(out.end(), nd->bucket.begin(), nd->bucket.end());
            for (const AnchorPath& c : nd->children) stack.push_back(c);
        }
    }
}

size_t AnchorTree::NodeCount() const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    return nodes_.size();
}

} // namespace geocore
