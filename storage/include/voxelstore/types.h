#pragma once

// voxelstore：时空记忆块（stmb/voxel 数据）的存储数据结构。
// 镜像 D:\manufacture\voxel 的 MemoryBlock / BlockVersion / DynamicInstance
// 落盘字段（查询时临时字段 hasFinerData / temporarilyOccupiedBy 不收）。
// 与事件树（eventstore）是相互独立的两个子系统：
//   事件树存"叙事/事实"（append-only 侧写图）；
//   voxelstore 存"空间状态"（可变当前态 + append-only 版本链 + 动态轨迹）。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace voxelstore {

using nlohmann::json;

/// 轴对齐包围盒（三维空间区域）。
struct Aabb {
    double min_x = 0, min_y = 0, min_z = 0;
    double max_x = 0, max_y = 0, max_z = 0;

    /// 相交判定（含边界相接）。
    bool intersects(const Aabb& o) const;
};

/// 周期模式相位（Seasonal 判定用）。
struct PatternPhase {
    int64_t offset_ms = 0;
    int64_t duration_ms = 0;
    std::string payload;
};

struct PeriodicPattern {
    int64_t period_ms = 0;
    std::vector<PatternPhase> phases;
};

/// 时空记忆块：绑定一个 AABB 区域和一个时间点的语义负载。
/// state: "Pending" | "Stable" | "Changing"（块状态机）。
struct VoxelBlock {
    uint64_t id = 0;

    // BlockKey：格子坐标 + 时间槽
    int64_t cell_x = 0, cell_y = 0, cell_z = 0, time_slot = 0;

    Aabb region;
    std::string payload;
    int64_t timestamp = 0;
    uint32_t version = 0;
    int64_t last_access = 0;

    std::string state;           // Pending | Stable | Changing
    double confidence = 1.0;     // 基准置信度（不被衰减改写）
    double confirmations = 0.0;  // 加权确认计数
    int64_t last_update = 0;

    // 候选变更（仅 Changing 状态有效）
    std::optional<std::string> pending_payload;
    std::optional<double> pending_confidence;

    // 观察窗口（观测管线，window_start = -1 表示无挂起）
    int64_t window_start = -1;
    double window_weight = 0.0;
    std::vector<uint64_t> window_sources;

    std::optional<PeriodicPattern> pattern;
    bool suspect = false;  // 空间一致性标记（仅标记不拒绝）

    // LOD 多尺度金字塔
    int level = -1;
    bool is_summary = false;
    int source_level = -1;
};

/// 历史版本快照：某块在 [valid_from, valid_to) 区间内生效的数据。
/// 版本链 append-mostly：只允许 seal（给当前生效版本填 valid_to）这一种"修改"。
struct VoxelVersion {
    uint64_t block_id = 0;
    uint32_t version = 0;
    std::string payload;
    double confidence = 1.0;
    std::string state;  // 归档时的块状态
    int64_t valid_from = 0;
    std::optional<int64_t> valid_to;  // 空 = 当前生效
};

/// 轨迹点：某时刻的位置与速度。
struct TrackPoint {
    int64_t t = 0;
    double px = 0, py = 0, pz = 0;  // 位置
    double vx = 0, vy = 0, vz = 0;  // 速度
};

/// 动态实例：被跟踪的运动物体（动静分离的动态侧）。
/// state: "Active" | "Stationary" | "Archived"。
struct VoxelInstance {
    uint64_t id = 0;
    std::string class_label;  // 类别（如 "car"）
    Aabb bounds;              // 最新包围盒
    TrackPoint latest;        // 最新轨迹点
    std::vector<TrackPoint> trajectory;
    std::vector<uint64_t> sources;
    double confidence = 1.0;
    std::string state;  // Active | Stationary | Archived
    int64_t last_seen = 0;
    int64_t stationary_since = -1;  // -1 = 运动中
};

void to_json(json& j, const PatternPhase& p);
void from_json(const json& j, PatternPhase& p);
void to_json(json& j, const PeriodicPattern& p);
void from_json(const json& j, PeriodicPattern& p);
void to_json(json& j, const TrackPoint& p);
void from_json(const json& j, TrackPoint& p);

} // namespace voxelstore
