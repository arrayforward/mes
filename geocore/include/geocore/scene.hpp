// ============================================================================
// scene.hpp —— 场景 Scene 与注册表 SceneRegistry：语义目录层（规范 §3.4）
// ============================================================================
//
// 【本文件的实现思路】
// 场景是锚点子树的"语义命名与生命周期管理单元"（论文定义 3.1），也是整个
// 内核的策略注入点：坐标模型（CoordinateModel）与运动学（Kinematic）都在
// 场景层注入，非欧几何被封装为图卡，不穿透场景边界（公理 A4）。
//
// 三层架构中的目录层（论文 §4，表 1）：规模千级、准静止、按名/按标签/
// 按空间粗判访问。数据结构选择直接对应访问模式：
//   - 主表 vector<Scene>，SceneId 即下标；
//   - 名字哈希 byName_；
//   - 标签倒排 byTag_[64]（标签 ≤64 种时位掩码）；
//   - 空间入口：包围球抽 SoA（cx/cy/cz/r 四数组），千级直接线性扫，
//     不建索引（规范 §9.3 错误预警：对 1000 个场景建空间索引是过度设计）；
//   - rootPath 排序数组：OwnerOf 最长前缀匹配 = 逐前缀二分（规范 §3.4）。
//
// 烘焙（纪律 P8）：rootTransform = 场景根到全局根的累积变换（含逆），
// 注册时以 double 全程连乘一次落盘。合法性条件（论文 §7.3）：
//   1. 链上所有节点 Kinematic 静态（运动链按帧重算，不可烘焙）；
//   2. 链上不经过非恒等图卡（球面/轨道的转移映射非线性，无法折叠进
//      单个刚体 Transform）。两条件任一不满足则 bakedValid=false，
//      跨场景远亲换算（算法 S5）自动回退为 LCA 行走。
//
// 生命周期（SetSceneState）：Resident/Loaded/Dormant/Unloaded。
// Dormant 释放物化数据但保留元数据（rootPath、rootTransform、语义锚点的
// Kinematic），因此休眠场景仍参与坐标换算，只是查对象报错（T-F-休眠）。
// ============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "geocore/anchor_path.hpp"
#include "geocore/coordinate_model.hpp"
#include "geocore/math.hpp"

namespace geocore {

// SceneId：主表下标；kInvalidScene 表示"全局公共区域"（规范 §7 OwnerOf）
using SceneId = uint32_t;
inline constexpr SceneId kInvalidScene = ~0u;

// 场景生命周期状态（规范 §7 SetSceneState）
enum class SceneState : uint8_t { Resident, Loaded, Dormant, Unloaded };

// ----------------------------------------------------------------------------
// Scene：场景记录（规范 §3.4）
// ----------------------------------------------------------------------------
struct Scene {
    SceneId         id = kInvalidScene;
    std::string     name;
    uint64_t        tagMask = 0;       // 标签位掩码（≤64 种）
    AnchorPath      rootPath;          // 挂载点：与锚点树的唯一接口
    Transform       rootTransform;     // 烘焙：到全局根的累积变换（P8）
    Transform       invRootTransform;  // 烘焙逆变换（算法 S5 远亲路径）
    bool            bakedValid = false; // 烘焙合法性（静态链 ∧ 恒等图卡链）
    BoundingSphere  bounds;            // 粗判包围球；运动场景取扫掠包络
    SceneState      state = SceneState::Resident;
    CoordinateModel model;             // 注入的坐标策略
};

// ----------------------------------------------------------------------------
// SceneRegistry：场景注册表
// ----------------------------------------------------------------------------
class SceneRegistry {
public:
    SceneRegistry() = default;

    // ------------------------------------------------------------------
    // AddScene：注册场景（Kernel::CreateScene 完成物化与烘焙后调用）
    // 契约（规范 §7 CreateScene）：rootPath 不得与既有场景互相包含，
    // 除非显式声明父子场景（allowNested=true，如"行星表面场景挂载到
    // 既有行星锚点"）；重复 name 拒绝。返回分配的 SceneId；冲突返回
    // kInvalidScene。
    // ------------------------------------------------------------------
    SceneId AddScene(Scene sc, bool allowNested);

    // 状态切换由 Kernel::SetSceneState 编排（释放物化数据需锚点树配合），
    // 本函数只更新目录层状态字段。
    void SetStateField(SceneId id, SceneState st);

    const Scene* Get(SceneId id) const;
    Scene* Get(SceneId id);
    size_t Size() const { return scenes_.size(); }

    SceneId FindByName(const std::string& name) const;
    std::vector<SceneId> FindByTag(int tagBit) const;

    // ------------------------------------------------------------------
    // OwnerOf：路径的场景归属 = rootPath 最长前缀匹配（规范 §3.4）
    // 伪码：for d = path.depth downto 0:
    //           if 二分(sortedRoots_, path.Prefix(d)) 命中: return id
    //       return kInvalidScene        // 全局公共区域
    // ------------------------------------------------------------------
    SceneId OwnerOf(const AnchorPath& path) const;
    const Scene* OwnerScene(const AnchorPath& path) const;

    // IsSceneRoot：路径是否恰为某场景的挂载点（图卡边界判定，算法 S3 在
    // 跨界时插入 ToParent/FromParent 的依据）
    bool IsSceneRoot(const AnchorPath& path) const;

    // ------------------------------------------------------------------
    // 空间粗判：SoA 包围球线性扫（千级规模下优于任何索引，规范 §3.4）
    // 返回包围球与查询球相交的场景集合。
    // ------------------------------------------------------------------
    std::vector<SceneId> ScenesNearSphere(const Vec3d& center, double r) const;

    // 目录层读写锁：注册/注销走写锁（规范 §6 的 RCU 窗口简化为短写锁），
    // 查询热路径持读锁。
    std::shared_mutex& Mutex() const { return mtx_; }

private:
    // sortedRoots_ 的重排维护（AddScene 后保持有序，供 OwnerOf 二分）
    void ResortRoots();

    mutable std::shared_mutex mtx_;
    std::vector<Scene> scenes_;                              // 主表，id 即下标
    std::unordered_map<std::string, SceneId> byName_;        // 名字哈希
    std::array<std::vector<SceneId>, 64> byTag_;             // 标签倒排
    std::vector<std::pair<AnchorPath, SceneId>> sortedRoots_;// rootPath 排序数组
    std::unordered_set<AnchorPath, PathHash> rootSet_;       // 图卡边界集合
    // SoA 包围球数组（与主表同序）
    std::vector<double> bcx_, bcy_, bcz_, br_;
};

} // namespace geocore
