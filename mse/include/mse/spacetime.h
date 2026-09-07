#pragma once

// ============================================================================
// mse/spacetime.h —— 时空树子系统(车间时空信息管理)
//
// 依据实现方案 §3.6 与用户约定"voxel 是对车间时空信息的管理":
//   时空是第一类维度,与属性正交,由时空树子系统自治——字典的手伸不过去。
//   三形态参照统一:{锚点 | 坐标 | 模糊原文, 精度}——模糊原文永不改写。
//   精度匹配:查"总装线"命中"总装线中段"(层级段前缀)。
//
// 实现:voxel(stmb::StmbService)做运行期时空记忆块服务(块状态机、版本
// 回溯 AS OF t、动态轨迹);storage(voxelstore::RecordVoxelStore)做持久化
// 落盘(storage 是所有访问的底层接口)。命名锚点层级(车间/产线/工位/区域)
// 是 mse 侧薄层:锚点表来自定义层(AnchorRegistered),路径段间 '/' 分隔。
//
// 时空记忆是投影:可丢弃可重建——replay(事件日志) 可完整重建。
// ============================================================================

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mse/model.h"

namespace voxelstore { class VoxelStore; }
namespace stmb { class StmbService; }

namespace mse {

class DefinitionLayer;
class EventLog;

class SpacetimeTree {
public:
    /// defs 提供锚点表;voxel_store 为持久化后端(可空 = 纯内存,仅 stmb 服务)。
    explicit SpacetimeTree(DefinitionLayer& defs, voxelstore::VoxelStore* voxel_store);
    ~SpacetimeTree();
    SpacetimeTree(const SpacetimeTree&) = delete;
    SpacetimeTree& operator=(const SpacetimeTree&) = delete;

    // ---- 精度匹配(纯函数) ----
    /// query 是否命中 event_anchor:逐段匹配——query 每段须等于 event 对应段,
    /// 或 query 末段是 event 对应段的字符串前缀("总装线"命中"总装线中段")。
    static bool anchor_matches(const std::string& query, const std::string& event_anchor);
    /// 事件时空参照是否落在切片内(kRawText 永不匹配锚点切片——模糊原文只存档)。
    static bool in_slice(const std::string& slice_anchor, const SpaceRef& event_space);

    // ---- 存证(结算后由写侧管线调用) ----
    /// 把一条已结算事件写入时空记忆:锚点块 put/confirm(版本链),
    /// 并为每个目标本体更新轨迹(任何带锚点的单目标事件都是一次过点观测)。
    /// 时间轴用逻辑时钟(event.settle_seq);发生时间原文进块 payload。
    void record_event(const Event& e);

    /// 重建:clear + 逐事件重放(投影可丢弃可重建)。
    void replay(const EventLog& log);

    // ---- 查询 ----
    /// 区域内时空块:返回锚点(含下钻)在 [from_seq, to_seq] 内的事件块摘要。
    std::vector<json> query_region(const std::string& anchor_path,
                                   int64_t from_seq = 0, int64_t to_seq = 0) const;
    /// AS OF t:锚点块在 settle_seq == t 时刻生效的版本(payload)。
    std::optional<json> anchor_as_of(const std::string& anchor_path, int64_t settle_seq) const;
    /// 本体轨迹:按时间升序的锚点路径序列(AVI 车辆位置历史的数据源)。
    std::vector<std::pair<int64_t, std::string>> trajectory_of(const std::string& ontology_id) const;
    /// 本体当前所在锚点(最后一次过点)。
    std::optional<std::string> current_anchor_of(const std::string& ontology_id) const;

private:
    DefinitionLayer&        defs_;
    voxelstore::VoxelStore* store_;       // 持久化(可空)
    stmb::StmbService*      svc_ = nullptr;  // 运行期时空块服务(拥有)

    std::map<std::string, uint64_t> anchor_blocks_;     // 锚点路径 → 块 id
    std::map<std::string, uint64_t> ontology_instance_; // 本体 id → 动态实例 id
};

} // namespace mse
