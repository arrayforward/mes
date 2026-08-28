#pragma once

// entity 组件的 geocore 距离适配器：把 geocore::Kernel 包装成
// Resolver::DistanceProvider，供轨迹连续性校验（vmax 物理可达）使用。

#include <optional>
#include <string>

#include <geocore/anchor_path.hpp>
#include <geocore/kernel.hpp>

#include "entity/resolver.h"

namespace entity {

/// 构造 DistanceProvider：把两锚点（entitytree anchor_ref，AnchorPath 路径码
/// 字符串）的原点在帧时间 frame_time 经 Kernel.Convert 换算到对方参考系，
/// 取直线距离（米）。Convert 自带场景仲裁（近亲行走 / 远亲烘焙），
/// 无需调用方指定场景。解析失败、跨树无法换算或内核异常时返回
/// std::nullopt（轨迹校验跳过，不是判负）。
/// 注意：kernel 须在 provider 使用期间存活；移动锚点请先 BeginFrame 推进帧时间。
inline Resolver::DistanceProvider make_geocore_distance(geocore::Kernel& kernel,
                                                        double frame_time) {
    return [&kernel, frame_time](const std::string& anchor_a,
                                 const std::string& anchor_b)
        -> std::optional<double> {
        geocore::AnchorPath pa, pb;
        if (!geocore::ParseAnchorPath(anchor_a, pa) ||
            !geocore::ParseAnchorPath(anchor_b, pb))
            return std::nullopt;
        if (pa == pb) return 0.0;
        try {
            geocore::Vec3d o =
                kernel.Convert(pa, pb, geocore::Vec3d{0.0, 0.0, 0.0}, frame_time);
            return o.Norm();
        } catch (const std::exception&) {
            return std::nullopt;  // 无法换算（如无场景覆盖该路径）→ 不约束
        }
    };
}

} // namespace entity
