// ============================================================================
// scene.cpp —— SceneRegistry 实现（对应 scene.hpp 的设计说明）
// ============================================================================
//
// 【本文件的实现思路】
// 目录层的四个索引（主表/名字/标签/SoA 包围体）+ rootPath 排序数组。
// 注册是低频事件，因此 AddScene 不惜 O(n) 重排 sortedRoots_ 与重建 SoA
// 数组，换取查询热路径的极简（论文 §2.3"构建期重投入换查询期轻量"）。
// ============================================================================
#include "geocore/scene.hpp"

#include <algorithm>

namespace geocore {

// ----------------------------------------------------------------------------
// AddScene：注册场景并维护全部索引
// 伪码：
//   if byName_[name] 已存在: return kInvalidScene
//   if ∃ 既有 rootPath 与新 rootPath 互相包含: return kInvalidScene  // 契约
//   id = scenes_.size()；scenes_.push_back(sc)；维护名字/标签/排序/SoA
// ----------------------------------------------------------------------------
SceneId SceneRegistry::AddScene(Scene sc, bool allowNested) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    if (byName_.count(sc.name)) return kInvalidScene;
    if (!allowNested) {
        for (const auto& rp : sortedRoots_) {
            // rootPath 互相包含会破坏 OwnerOf 与图卡边界的唯一性（规范 §7 契约）
            if (rp.first.IsPrefixOf(sc.rootPath) || sc.rootPath.IsPrefixOf(rp.first))
                return kInvalidScene;
        }
    }
    SceneId id = static_cast<SceneId>(scenes_.size());
    sc.id = id;
    bcx_.push_back(sc.bounds.center.x);
    bcy_.push_back(sc.bounds.center.y);
    bcz_.push_back(sc.bounds.center.z);
    br_.push_back(sc.bounds.radius);
    byName_[sc.name] = id;
    for (int b = 0; b < 64; ++b)
        if (sc.tagMask & (1ull << b)) byTag_[b].push_back(id);
    sortedRoots_.emplace_back(sc.rootPath, id);
    rootSet_.insert(sc.rootPath);
    ResortRoots();
    scenes_.push_back(std::move(sc));
    return id;
}

void SceneRegistry::ResortRoots() {
    std::sort(sortedRoots_.begin(), sortedRoots_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

void SceneRegistry::SetStateField(SceneId id, SceneState st) {
    std::unique_lock<std::shared_mutex> lk(mtx_);
    if (id < scenes_.size()) scenes_[id].state = st;
}

const Scene* SceneRegistry::Get(SceneId id) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    return id < scenes_.size() ? &scenes_[id] : nullptr;
}

Scene* SceneRegistry::Get(SceneId id) {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    return id < scenes_.size() ? &scenes_[id] : nullptr;
}

SceneId SceneRegistry::FindByName(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    auto it = byName_.find(name);
    return it == byName_.end() ? kInvalidScene : it->second;
}

std::vector<SceneId> SceneRegistry::FindByTag(int tagBit) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    if (tagBit < 0 || tagBit >= 64) return {};
    return byTag_[tagBit];
}

// ----------------------------------------------------------------------------
// OwnerOf：rootPath 最长前缀匹配（排序 + 二分，规范 §3.4）
// 伪码：
//   for d = path.depth downto 0:
//       pref = path.Prefix(d)
//       if binary_search(sortedRoots_, pref): return 对应 SceneId
//   return kInvalidScene   // 全局公共区域
// 从长到短枚举前缀，首个命中即最长匹配；深度 ≤ 32，千级条目二分 ≈ 10 次比较。
// ----------------------------------------------------------------------------
SceneId SceneRegistry::OwnerOf(const AnchorPath& path) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    for (int d = path.depth; d >= 0; --d) {
        AnchorPath pref = path.Prefix(d);
        auto it = std::lower_bound(
            sortedRoots_.begin(), sortedRoots_.end(), pref,
            [](const auto& entry, const AnchorPath& v) { return entry.first < v; });
        if (it != sortedRoots_.end() && it->first == pref) return it->second;
    }
    return kInvalidScene;
}

const Scene* SceneRegistry::OwnerScene(const AnchorPath& path) const {
    return Get(OwnerOf(path));
}

bool SceneRegistry::IsSceneRoot(const AnchorPath& path) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    return rootSet_.count(path) != 0;
}

// ----------------------------------------------------------------------------
// ScenesNearSphere：SoA 包围球线性扫
// 千级场景不建索引（规范 §9.3 错误预警第 5 条）；循环体与 SIMD 友好的
// 纯数组访问，真到万级以上再考虑扁平 BVH（属外围优化）。
// ----------------------------------------------------------------------------
std::vector<SceneId> SceneRegistry::ScenesNearSphere(const Vec3d& c,
                                                     double r) const {
    std::shared_lock<std::shared_mutex> lk(mtx_);
    std::vector<SceneId> out;
    for (size_t i = 0; i < bcx_.size(); ++i) {
        double dx = bcx_[i] - c.x, dy = bcy_[i] - c.y, dz = bcz_[i] - c.z;
        double rr = br_[i] + r;
        if (dx * dx + dy * dy + dz * dz <= rr * rr)
            out.push_back(static_cast<SceneId>(i));
    }
    return out;
}

} // namespace geocore
