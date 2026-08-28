// ============================================================================
// 文件: dynamic_layer.cpp
// 模块: stmb_dynamic(动态实例轨迹层,依赖 core)
// 用途: 实现 DynamicLayer 的上报、跨源关联、状态推进、查询与恢复。
// 设计思路:
//   1. 活跃集(Active/Stationary)与归档集分两个哈希表存放,归档即移动;
//   2. 速度估计:上报零向量且有历史轨迹点时,用「位移 / 时间差(秒)」差分;
//   3. 近零速度判定用速度模长与一个小 epsilon 比较;stationarySince 以 -1
//      为「运动中」哨兵,避免与合法时间戳 0 混淆;
//   4. update 先收集待归档 id 再统一移动,避免遍历中修改容器。
// 架构角色: DynamicLayer 的唯一实现文件。
// ============================================================================
#include "dynamic_layer.h"

#include <cmath>

namespace stmb {
namespace {

// 速度模长判定近零的 epsilon(单位/秒)
constexpr double kSpeedEps = 1e-6;

// 伪代码:
//   1. 计算速度向量的模长并返回。
double speedOf(const std::array<double, 3>& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

}  // namespace

// 伪代码:
//   1. 按枚举值 switch 返回对应字符串字面量;
//   2. 兜底返回 "Unknown"。
const char* toString(InstanceState state) {
    switch (state) {
        case InstanceState::Active:     return "Active";
        case InstanceState::Stationary: return "Stationary";
        case InstanceState::Archived:   return "Archived";
    }
    return "Unknown";
}

// 伪代码:
//   1. 保存超时与上限配置(构造函数体为空,仅做成员初始化)。
DynamicLayer::DynamicLayer(std::int64_t stationaryTimeoutMs,
                           std::int64_t archiveTimeoutMs,
                           std::size_t maxTrajectoryPoints,
                           std::int64_t assocWindowMs)
    : stationaryTimeoutMs_(stationaryTimeoutMs), archiveTimeoutMs_(archiveTimeoutMs),
      maxTrajectoryPoints_(maxTrajectoryPoints), assocWindowMs_(assocWindowMs) {}

// 伪代码:
//   1. id 为 0 时自动分配新 id;否则推进 id 发生器越过已见过的最大 id;
//   2. 活跃集中找到:直接追加观测;
//   3. 归档集中找到:移回活跃集并置 Active(复活),再追加观测;
//   4. 都不存在:创建新实例(类别入档),追加首条观测;
//   5. 返回实例 id。
InstanceId DynamicLayer::report(InstanceId id, const std::string& classLabel,
                                const AABB& bounds,
                                const std::array<double, 3>& velocity,
                                TimeStamp now, SourceId source) {
    if (id == 0) {
        id = nextInstanceId_++;
    } else if (id >= nextInstanceId_) {
        nextInstanceId_ = id + 1;
    }
    auto activeIt = active_.find(id);
    if (activeIt != active_.end()) {
        appendObservation(activeIt->second, bounds, velocity, now, source);
        return id;
    }
    auto archivedIt = archived_.find(id);
    if (archivedIt != archived_.end()) {
        DynamicInstance inst = std::move(archivedIt->second);
        archived_.erase(archivedIt);
        inst.state = InstanceState::Active;
        auto [pos, inserted] = active_.emplace(id, std::move(inst));
        (void)inserted;
        appendObservation(pos->second, bounds, velocity, now, source);
        return id;
    }
    DynamicInstance inst;
    inst.id = id;
    inst.classLabel = classLabel;
    appendObservation(inst, bounds, velocity, now, source);
    active_.emplace(id, std::move(inst));
    return id;
}

// 伪代码:
//   1. 遍历活跃集:类别一致、包围盒相交、|now - lastSeen| 不超关联窗,
//      三者俱全即视为同一物体,转发到按 id 上报并返回其 id;
//   2. 无匹配:按 id=0 新建实例上报,返回新 id。
InstanceId DynamicLayer::reportAuto(const std::string& classLabel,
                                    const AABB& bounds,
                                    const std::array<double, 3>& velocity,
                                    TimeStamp now, SourceId source) {
    for (const auto& [id, inst] : active_) {
        if (inst.classLabel == classLabel && inst.bounds.intersects(bounds) &&
            std::llabs(now - inst.lastSeen) <= assocWindowMs_) {
            return report(id, classLabel, bounds, velocity, now, source);
        }
    }
    return report(0, classLabel, bounds, velocity, now, source);
}

// 伪代码:
//   1. 遍历活跃集:Active 且近零速度持续超过 stationaryTimeoutMs ->
//      Stationary;
//   2. 无新观测超过 archiveTimeoutMs 的实例收集到待归档列表;
//   3. 遍历结束后统一把待归档实例移入归档集(状态置 Archived)。
void DynamicLayer::update(TimeStamp now) {
    std::vector<InstanceId> toArchive;
    for (auto& [id, inst] : active_) {
        if (inst.state == InstanceState::Active && inst.stationarySince >= 0 &&
            now - inst.stationarySince >= stationaryTimeoutMs_) {
            inst.state = InstanceState::Stationary;
        }
        if (now - inst.lastSeen >= archiveTimeoutMs_) {
            toArchive.push_back(id);
        }
    }
    for (InstanceId id : toArchive) {
        auto it = active_.find(id);
        it->second.state = InstanceState::Archived;
        archived_.emplace(id, std::move(it->second));
        active_.erase(it);
    }
}

// 伪代码:
//   1. 遍历活跃集:包围盒与区域相交且最新观测时刻落在时间范围内的实例,
//      复制进结果列表;
//   2. 返回结果(Archived 不参与)。
std::vector<DynamicInstance> DynamicLayer::queryActive(const AABB& region,
                                                       const TimeRange& range) const {
    std::vector<DynamicInstance> result;
    for (const auto& [id, inst] : active_) {
        if (inst.bounds.intersects(region) && range.contains(inst.latest.t)) {
            result.push_back(inst);
        }
    }
    return result;
}

// 伪代码:
//   1. 先查活跃集再查归档集,找到则把轨迹 deque 拷成 vector 返回;
//   2. 找不到返回空 vector。
std::vector<TrackPoint> DynamicLayer::trajectoryOf(InstanceId id) const {
    const DynamicInstance* inst = find(id);
    if (inst == nullptr) {
        return {};
    }
    return std::vector<TrackPoint>(inst->trajectory.begin(), inst->trajectory.end());
}

// 伪代码:
//   1. 遍历活跃集,收集包围盒与区域相交的实例 id(Active/Stationary);
//   2. 返回 id 列表(Archived 不参与)。
std::vector<InstanceId> DynamicLayer::occupantsOf(const AABB& region) const {
    std::vector<InstanceId> result;
    for (const auto& [id, inst] : active_) {
        if (inst.bounds.intersects(region)) {
            result.push_back(id);
        }
    }
    return result;
}

// 伪代码:
//   1. 先查活跃集,命中返回地址;
//   2. 再查归档集,命中返回地址;
//   3. 都没有返回 nullptr。
const DynamicInstance* DynamicLayer::find(InstanceId id) const {
    auto it = active_.find(id);
    if (it != active_.end()) {
        return &it->second;
    }
    auto archivedIt = archived_.find(id);
    if (archivedIt != archived_.end()) {
        return &archivedIt->second;
    }
    return nullptr;
}

// 伪代码:
//   1. 在活跃集中查找该实例,不存在直接返回;
//   2. 置 Archived 并移入归档集(轨迹保留)。
void DynamicLayer::markArchived(InstanceId id) {
    auto it = active_.find(id);
    if (it == active_.end()) {
        return;
    }
    it->second.state = InstanceState::Archived;
    archived_.emplace(id, std::move(it->second));
    active_.erase(it);
}

// 伪代码:
//   1. 分别统计活跃集中 Active / Stationary 数量,加上归档集大小,返回。
DynamicStats DynamicLayer::stats() const {
    DynamicStats s;
    for (const auto& [id, inst] : active_) {
        if (inst.state == InstanceState::Stationary) {
            ++s.stationaryCount;
        } else {
            ++s.activeCount;
        }
    }
    s.archivedCount = archived_.size();
    return s;
}

// 伪代码:
//   1. 合并活跃集与归档集的全部实例副本到结果列表;
//   2. 返回(持久化快照用)。
std::vector<DynamicInstance> DynamicLayer::allInstances() const {
    std::vector<DynamicInstance> result;
    result.reserve(active_.size() + archived_.size());
    for (const auto& [id, inst] : active_) {
        result.push_back(inst);
    }
    for (const auto& [id, inst] : archived_) {
        result.push_back(inst);
    }
    return result;
}

// 伪代码:
//   1. 返回 id 发生器当前值。
InstanceId DynamicLayer::nextInstanceId() const {
    return nextInstanceId_;
}

// 伪代码:
//   1. 清空活跃集与归档集;
//   2. 按 state 把实例分别放入两个集合;
//   3. 恢复 id 发生器。
void DynamicLayer::restore(const std::vector<DynamicInstance>& instances,
                           InstanceId nextId) {
    active_.clear();
    archived_.clear();
    for (const DynamicInstance& inst : instances) {
        if (inst.state == InstanceState::Archived) {
            archived_.emplace(inst.id, inst);
        } else {
            active_.emplace(inst.id, inst);
        }
    }
    nextInstanceId_ = nextId;
}

// 伪代码:
//   1. 取包围盒中心为位置;
//   2. 速度处理:调用方传零向量且有历史轨迹点时,用「(当前位置 - 上一点
//      位置)/ 时间差(秒)」差分估计;时间差非正保持零向量;
//   3. 轨迹追加点,超过上限丢最老点;更新 latest/bounds/lastSeen/sources;
//   4. 状态字段:近零速度则记录 stationarySince(首次),运动中则清哨兵,
//      Stationary 实例重新运动则回到 Active。
void DynamicLayer::appendObservation(DynamicInstance& inst, const AABB& bounds,
                                     const std::array<double, 3>& velocity,
                                     TimeStamp now, SourceId source) {
    TrackPoint point;
    point.t = now;
    point.position = bounds.center();
    point.velocity = velocity;
    if (speedOf(velocity) < kSpeedEps && !inst.trajectory.empty()) {
        const TrackPoint& prev = inst.trajectory.back();
        const TimeStamp dt = now - prev.t;
        if (dt > 0) {
            const double dtSec = static_cast<double>(dt) / 1000.0;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                point.velocity[axis] = (point.position[axis] - prev.position[axis]) / dtSec;
            }
        }
    }
    inst.trajectory.push_back(point);
    while (inst.trajectory.size() > maxTrajectoryPoints_) {
        inst.trajectory.pop_front();
    }
    inst.latest = inst.trajectory.back();
    inst.bounds = bounds;
    inst.lastSeen = now;
    inst.sources.insert(source);

    if (speedOf(inst.latest.velocity) < kSpeedEps) {
        if (inst.stationarySince < 0) {
            inst.stationarySince = now;
        }
    } else {
        inst.stationarySince = -1;
        if (inst.state == InstanceState::Stationary) {
            inst.state = InstanceState::Active;
        }
    }
}

}  // namespace stmb
