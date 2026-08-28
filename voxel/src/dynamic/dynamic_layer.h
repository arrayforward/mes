// ============================================================================
// 文件: dynamic_layer.h
// 模块: stmb_dynamic(动态实例轨迹层,依赖 core)
// 用途: 声明「动静分离」的动态侧:运动物体不钉进静态体素网格,而是作为
//       动态实例独立跟踪——实例携带 ID、类别、3D 包围盒、速度向量、历史
//       轨迹与观测来源,支持秒级上报更新、跨源关联、状态推进与轨迹回溯。
// 设计思路:
//   1. 轨迹用 deque 保存,超过点数上限丢最老点(有界内存);
//   2. 速度可由相邻轨迹点差分估计:调用方传零向量时自动差分;
//   3. 跨源关联:reportAuto 按「空间重叠 + 类别一致 + 时间接近」匹配既有
//      实例,匹配不到才新建,模拟多终端拍到同一物体;
//   4. 状态机:Active --(近零速度持续超时)--> Stationary;
//      Active/Stationary --(无新观测超时)--> Archived(移出活跃集,
//      轨迹保留供回溯);Archived 实例再次被上报时复活为 Active;
//   5. 本层只维护内存态,持久化由 persistence 模块负责(实例可序列化)。
// 架构角色: 独立部件,由 StmbService 持有(配置超时后启用);线程安全由
//           服务层统一互斥锁保证,本类自身不加锁。
// ============================================================================
#pragma once

#include "types.h"

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stmb {

// 动态实例标识(观测来源标识 SourceId 已在 core/types.h 定义)
using InstanceId = std::uint64_t;

// 实例状态:活跃 -> 静止 -> 已归档(离开监控区,轨迹保留)
enum class InstanceState {
    Active,
    Stationary,
    Archived,
};

// 状态枚举转字符串(日志/演示输出用)
const char* toString(InstanceState state);

// 轨迹点:某时刻的位置与速度
struct TrackPoint {
    TimeStamp t = 0;                        // 观测时刻
    std::array<double, 3> position{0.0, 0.0, 0.0};  // 位置(包围盒中心)
    std::array<double, 3> velocity{0.0, 0.0, 0.0};  // 速度向量(单位/秒)
};

// 动态实例:一个被跟踪的运动物体
struct DynamicInstance {
    InstanceId    id = 0;                   // 实例 ID
    std::string   classLabel;               // 类别(如 "car")
    AABB          bounds;                   // 最新 3D 包围盒
    TrackPoint    latest;                   // 最新轨迹点
    std::deque<TrackPoint> trajectory;      // 历史轨迹(有上限,丢最老点)
    std::unordered_set<SourceId> sources;   // 观测来源集合
    double        confidence = 1.0;         // 观测置信度
    InstanceState state = InstanceState::Active;  // 当前状态
    TimeStamp     lastSeen = 0;             // 最近一次观测时刻
    TimeStamp     stationarySince = -1;     // 开始近零速度的时刻(-1 = 运动中)
};

// 动态层统计:活跃 / 静止 / 已归档实例数
struct DynamicStats {
    std::size_t activeCount = 0;
    std::size_t stationaryCount = 0;
    std::size_t archivedCount = 0;
};

class DynamicLayer {
public:
    // 构造:静止超时 / 归档超时(毫秒);轨迹点上限默认 64;关联时间窗默认 2000ms
    DynamicLayer(std::int64_t stationaryTimeoutMs, std::int64_t archiveTimeoutMs,
                 std::size_t maxTrajectoryPoints = 64,
                 std::int64_t assocWindowMs = 2000);

    // 按 id 上报:新实例创建(id 传 0 自动分配)或已有实例追加轨迹点;
    // 速度传零向量时按相邻轨迹点差分估计;返回实例 id
    InstanceId report(InstanceId id, const std::string& classLabel,
                      const AABB& bounds, const std::array<double, 3>& velocity,
                      TimeStamp now, SourceId source);

    // 跨源关联上报:按「空间重叠 + 类别一致 + 时间接近」匹配活跃实例,
    // 匹配不到才新建;返回匹配/新建的实例 id
    InstanceId reportAuto(const std::string& classLabel, const AABB& bounds,
                          const std::array<double, 3>& velocity,
                          TimeStamp now, SourceId source);

    // 状态推进:静止超时 Active -> Stationary;观测中断超时 -> Archived
    void update(TimeStamp now);

    // 查询活跃(Active/Stationary)实例:包围盒相交且最新观测落在时间范围内
    std::vector<DynamicInstance> queryActive(const AABB& region,
                                             const TimeRange& range) const;

    // 轨迹回溯(含已归档实例)
    std::vector<TrackPoint> trajectoryOf(InstanceId id) const;

    // 与区域相交的活跃实例 id 列表(临时占用标记用)
    std::vector<InstanceId> occupantsOf(const AABB& region) const;

    // 按 id 查找实例(含已归档),不存在返回 nullptr
    const DynamicInstance* find(InstanceId id) const;

    // 把实例标记为 Archived(沉淀到静态层后调用)
    void markArchived(InstanceId id);

    // 统计 / 全量实例(持久化快照用)/ id 发生器状态
    DynamicStats stats() const;
    std::vector<DynamicInstance> allInstances() const;
    InstanceId nextInstanceId() const;

    // 从持久化数据恢复(覆盖当前内存态)
    void restore(const std::vector<DynamicInstance>& instances, InstanceId nextId);

private:
    // 向实例追加一次观测(轨迹点、差分速度、状态字段更新)
    void appendObservation(DynamicInstance& inst, const AABB& bounds,
                           const std::array<double, 3>& velocity,
                           TimeStamp now, SourceId source);

    std::int64_t stationaryTimeoutMs_;  // 静止超时(毫秒)
    std::int64_t archiveTimeoutMs_;     // 归档超时(毫秒)
    std::size_t    maxTrajectoryPoints_;  // 轨迹点上限
    std::int64_t   assocWindowMs_;      // 跨源关联时间窗(毫秒)
    InstanceId     nextInstanceId_ = 1; // 实例 id 发生器(从 1 开始)

    std::unordered_map<InstanceId, DynamicInstance> active_;    // Active + Stationary
    std::unordered_map<InstanceId, DynamicInstance> archived_;  // Archived(轨迹保留)
};

}  // namespace stmb
