// ============================================================================
// frame_cache.hpp —— 帧级缓存 FrameCache（规范 §3.6，不变式 INV-4）
// ============================================================================
//
// 【本文件的实现思路】
// 公理 A1（拓扑静止）使两类缓存合法化（论文 §5.4"静止红利"）：
//   1. 路径缓存 lastQueryPath：连续查询的时间局部性——下次查询先与缓存
//      求 LCA 再行走，平均深度 O(d) 降至 O(Δ)；
//   2. 变换缓存 toSceneRoot：物化节点 → 场景根的累积变换，路径不变则
//      帧内复用，三角函数只在帧切换时重算。
//
// INV-4（缓存一致性）：失效条件 = 路径变更或帧切换；任何写入路径不得读
// 旧帧缓存。落实方式：
//   - BeginFrame(tNew)：t 变化即整体失效（清空变换缓存与路径缓存）；
//   - 写入路径（Transfer/Save/Remove）一律走直接行走，不读本缓存；
//   - 本参考实现为单例缓存 + 互斥锁。规范 §6 的"每线程一份 FrameCache"
//     是性能优化形态（免锁），语义与 INV-4 等价，多线程管线可据此改造
//     （README 已知边界）。
//
// 与规范的偏差：toSceneRoot 的键用完整 AnchorPath 而非 uint64 哈希值——
// 哈希碰撞会返回错误变换，属正确性问题，此处宁稳勿快。
// ============================================================================
#pragma once

#include <unordered_map>

#include "geocore/anchor_path.hpp"
#include "geocore/math.hpp"

namespace geocore {

// ----------------------------------------------------------------------------
// FrameCache：帧级缓存
// ----------------------------------------------------------------------------
struct FrameCache {
    double     t = 0.0;             // 帧时间戳（P3：一帧内所有换算共享此 t）
    AnchorPath lastQueryPath{};     // 时间局部性：下次查询从此求 LCA
    std::unordered_map<AnchorPath, Transform, PathHash> toSceneRoot;  // 物化节点→场景根

    // BeginFrame：推进帧时间；t 变化即整体失效（INV-4）
    void BeginFrame(double tNew) {
        if (tNew != t) {
            t = tNew;
            toSceneRoot.clear();
            lastQueryPath = AnchorPath{};
        }
    }
};

} // namespace geocore
