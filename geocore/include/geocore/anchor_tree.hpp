// ============================================================================
// anchor_tree.hpp —— 路径码锚点树：虚拟/物化统一的层级参考系（规范 §3.5）
// ============================================================================
//
// 【本文件的实现思路】
// 论文 §5.1 的统一：八叉树节点即匿名锚点——层级参考系与层级空间索引是
// 同一棵树。树在数学上无限细分，但只有两个来源的节点占用存储：
//   - 语义节点：手工创建（CreateScene 的根、CreateAnchor 的命名锚点），
//     携带 Kinematic，semantic=true，不可自动回收；
//   - 物化匿名节点：含对象或含物化子节点的路径（INV-1 物化最小性）。
// 其余全部是虚拟节点：路径可算即存在，变换按规则生成，零存储（论文表 2）。
//
// 数据结构（规范 §3.5）：
//   unordered_map<AnchorPath, unique_ptr<NodeData>> nodes_
//   键为完整路径（哈希 = HashKey，全等比较兜底）；值用 unique_ptr 持有，
//   因为 NodeData 内含 shared_mutex 不可移动，且指针稳定性便于桶级锁。
//
// 并发模型（规范 §6）：
//   - mtx_：树结构读写锁（shared_mutex）——查询并发、物化/回收互斥；
//   - NodeData::mtx：桶级读写锁，锁粒度 = 物化节点；
//   - 锁序约定：一律 树锁 → 桶锁，Transfer 全程持树写锁，杜绝死锁；
//   - TransformAt/AnchorOf/LCA/Convert 的纯计算部分无共享状态，
//     锁只出现在 TransformAt 查表与桶扫描两处（规范 §6 末条）。
//
// 与 SceneRegistry 的协作：TransformAt 虚拟分支需要知道"路径属于哪个
// 场景、该场景的细分规则是什么"，通过 SetRegistry 注入（Kernel 装配时
// 双向接线，见 kernel.cpp）；全局公共区域（不属于任何场景的路径）使用
// 内置全局欧氏模型，extent = 2^41 m，覆盖行星系尺度。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "geocore/anchor_path.hpp"
#include "geocore/coordinate_model.hpp"
#include "geocore/kinematic.hpp"
#include "geocore/math.hpp"

namespace geocore {

class SceneRegistry;  // 前向声明：双向接线由 Kernel 完成

// ----------------------------------------------------------------------------
// NodeData：物化节点记录（仅非空路径占用，规范 §3.5）
// ----------------------------------------------------------------------------
struct NodeData {
    AnchorPath              path;       // 节点身份（冗余存储，便于遍历与校验）
    Kinematic               kinematic;  // 语义节点为用户给定；匿名节点为规则生成的 Static
    std::vector<uint64_t>   bucket;     // 对象桶：扁平数组（存 ObjectId）
    std::vector<AnchorPath> children;   // 物化子节点（虚拟子节点不登记）
    bool                    semantic = false;  // 语义节点不可自动回收（INV-1 例外）
    mutable std::shared_mutex mtx;      // 桶级读写锁（锁粒度 = 物化节点）
};

// ----------------------------------------------------------------------------
// AnchorTree：物化哈希表 + 虚拟节点规则生成
// ----------------------------------------------------------------------------
class AnchorTree {
public:
    AnchorTree() = default;

    // 双向接线：TransformAt 虚拟分支与 Prune 需要查询场景归属
    void SetRegistry(const SceneRegistry* r) { registry_ = r; }

    // ------------------------------------------------------------------
    // TransformAt：节点变换解析（算法 S2，虚拟/物化统一入口）
    // 伪码：
    //   if nodes_.find(path): return {kin.OriginAt(t), kin.RotationAt(t)}
    //   // 虚拟节点：规则生成（公理 A3 保证可计算）
    //   return {QuantizedChildOrigin(parent, seg), 单位四元数}
    // ------------------------------------------------------------------
    Transform TransformAt(const AnchorPath& path, double t) const;

    // KinematicOf：返回节点运动学副本（物化查表 / 虚拟返回规则 Static）。
    // 烘焙 rootTransform 时用于判断链是否全程静态（论文 §7.3）。
    Kinematic KinematicOf(const AnchorPath& path) const;

    // IsMaterialized：路径是否在哈希表中有记录（区分虚拟/物化）
    bool IsMaterialized(const AnchorPath& path) const;

    // ------------------------------------------------------------------
    // MaterializeChain：惰性物化父链（Save 的第三步，规范 S7-Save）
    // 伪码：for d = 1..path.depth: 若 prefix(d) 未物化，则以规则生成的
    //   Static Kinematic 插入，并登记进父节点的 children
    // 关键：物化匿名节点的变换必须与虚拟分支的规则生成严格一致，
    //       否则同一路径物化前后变换跳变（INV 之外的隐性一致性）。
    // ------------------------------------------------------------------
    void MaterializeChain(const AnchorPath& path);

    // MaterializeSemantic：物化语义节点（场景根 / CreateAnchor）。
    // 先物化父链，再以用户给定的 Kinematic 覆盖叶节点并标记 semantic。
    void MaterializeSemantic(const AnchorPath& path, const Kinematic& kin);

    // ------------------------------------------------------------------
    // PruneFrom：惰性回收（Remove 后维持 INV-1）
    // 伪码：cur = path
    //   while cur 物化 ∧ !semantic ∧ 桶空 ∧ 无物化子节点 ∧ 非场景根:
    //       从父节点 children 摘除；从哈希表删除；cur = cur.Parent()
    // ------------------------------------------------------------------
    void PruneFrom(const AnchorPath& path);

    // EraseDescendants：删除 rootPath 的全部物化后代（场景 Dormant/Unloaded
    // 释放物化数据；场景根本身的元数据保留，规范 §3.4 注释）。
    void EraseDescendants(const AnchorPath& rootPath);

    // ForceErase：无条件删除节点（含语义节点）并向上回收空父链。
    // 仅用于 CreateScene 冲突回滚等异常路径，正常业务不走这里。
    void ForceErase(const AnchorPath& path);

    // ------------------------------------------------------------------
    // 对象桶操作（桶级锁；调用前节点必须已物化）
    // ------------------------------------------------------------------
    void BucketInsert(const AnchorPath& path, uint64_t id);  // 先写新（INV-5）
    void BucketRemove(const AnchorPath& path, uint64_t id);  // 后删旧（INV-5）
    std::vector<uint64_t> BucketCopy(const AnchorPath& path) const;  // Find 候选收集

    // ------------------------------------------------------------------
    // CollectSubtreeBucketIds：收集 root 子树内全部物化节点的桶内容
    // （含 root 自身）。Find 阶段一的后代覆盖（规范 §4.5）：
    // 对象挂在"尺度恰好容纳它的最深节点"（INV-2），小尺度对象沉在查询
    // 基准层的子树深处，必须沿 children 链下潜收集；INV-1 保证物化子树
    // 规模被实际内容界定，遍历无空转。
    // ------------------------------------------------------------------
    void CollectSubtreeBucketIds(const AnchorPath& root,
                                 std::vector<uint64_t>& out) const;

    // ------------------------------------------------------------------
    // Transfer 原子区（规范 §6：Transfer 先写后删须对读者原子可见）
    // Kernel 的 Transfer 以 UniqueTreeLock 包住 写新桶→删旧桶→回收 全程；
    // 读者（Find）持树读锁扫描，要么全在迁移前、要么全在迁移后。
    // ------------------------------------------------------------------
    std::unique_lock<std::shared_mutex> UniqueTreeLock() {
        return std::unique_lock<std::shared_mutex>(mtx_);
    }
    // 以下 Locked 后缀函数是上述无锁版本的去锁内部实现，仅供持锁方调用
    void MaterializeChainLocked(const AnchorPath& path);
    void BucketInsertLocked(const AnchorPath& path, uint64_t id);
    void BucketRemoveLocked(const AnchorPath& path, uint64_t id);
    void PruneFromLocked(const AnchorPath& path);

    // 诊断与测试：物化节点总数 / 全表只读遍历（T-INV-1 验证用）
    size_t NodeCount() const;
    template <typename F>
    void ForEachNode(F&& f) const {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        for (const auto& kv : nodes_) f(*kv.second);
    }

private:
    // 未加锁的内部查找（unique_ptr 保证引用稳定）
    NodeData* FindUnlocked(const AnchorPath& path);
    const NodeData* FindUnlocked(const AnchorPath& path) const;

    // 规则生成的子区原点：QuantizedChildOrigin(parent, seg)
    // 伪码：scene = registry.OwnerOf(path)（缺省 = 全局欧氏模型）
    //       d = (path.depth − 1) − scene.rootPath.depth
    //       return scene.model.ChildOrigin(path.LastSeg(), d)
    Vec3d QuantizedChildOrigin(const AnchorPath& path) const;

    mutable std::shared_mutex mtx_;  // 树结构读写锁
    std::unordered_map<AnchorPath, std::unique_ptr<NodeData>, PathHash> nodes_;
    const SceneRegistry* registry_ = nullptr;

    // 全局公共区域的内置欧氏模型：extent = 2^41 m（约 14.7 AU，
    // 覆盖行星系尺度；2 的幂保证每层 cell 尺寸在 double 中精确）。
    Euclidean3D globalModel_{2199023255552.0};
};

} // namespace geocore
