#pragma once

// ============================================================================
// mse/view_engine.h —— 读侧:视图引擎(四元组求值)
//
// 依据《系统API设计》§八 与视图提取文档 §4.3:
//   视图不存状态:视图 = 截至 t 时刻对事件树的查询结果(AS OF t 的 fold)。
//   View = (queries, selects, rules, emits);渲染模式三选一:终态/流水/拦截。
//   规则一份定义两处消费:行过滤/列可见/按钮可用与写侧结算用同一份规则——
//   "界面上能点的 ⇔ 系统能结算的"在结构上一致。
//   观察者上下文(角色)= variants 预设选择;AS OF t 是所有视图的免费能力。
//   红线:视图引擎无任何写路径。
// ============================================================================

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mse/model.h"

namespace mse {

class DefinitionLayer;
class EventLog;
class Projection;
class RuleEngine;
class RejectionLog;
class SnapshotStore;
class SpacetimeTree;

struct ViewParams {
    std::string observer;                      // 观察者角色(variants 键;空 = 默认)
    std::optional<std::string> entity;         // 纵切锚点本体 id
    std::optional<int64_t>      as_of_seq;     // AS OF:截至某事件序号(含)
    std::string spacetime;                     // 覆盖视图的时空切片(空 = 用视图定义)
};

class ViewEngine {
public:
    ViewEngine(DefinitionLayer& defs, Projection& proj, EventLog& log,
               RuleEngine& rules, SpacetimeTree& spacetime);
    void set_rejection_log(const RejectionLog* log);  // 拦截模式视图的数据源
    /// AS OF 加速:挂接快照库后,as_of 从 floor 快照 + 增量重放构建,而非全量重放。
    void set_snapshot_store(const SnapshotStore* snapshots);
    /// AS OF 重放口径:注入含派生钩子的区间重放(WritePipeline::replay_range),
    /// 使 AS OF 临时投影与在线投影的派生属性逐比特一致。
    void set_replay_provider(std::function<void(Projection&, int64_t, int64_t)> provider);

    /// 求值视图契约,返回渲染 JSON(只读,不落任何状态):
    /// {
    ///   "view_id", "render_mode", "observer", "as_of_seq",
    ///   "columns": [...],                      // selects ∪ variant 列(字典序保持声明序)
    ///   "rows":    [...],                      // 终态:本体行(含 "buttons" 每行按钮可用性)
    ///                                          // 流水:事件行(L1+L2 修正成对,带 corrects 标注)
    ///                                          // 拦截:已结算事实行
    ///   "rejections": [...],                   // 仅拦截模式:近期 L0 拦截反馈
    ///   "actions": [{"type", "enabled", "reasons":[]}]  // 视图级可发起事件
    /// }
    /// 视图未注册/已弃用 → 返回 {"error": ...}。
    json render(const std::string& view_id, const ViewParams& params) const;

private:
    DefinitionLayer& defs_;
    Projection&      proj_;
    EventLog&        log_;
    RuleEngine&      rules_;
    SpacetimeTree&   spacetime_;
    const RejectionLog* rejections_ = nullptr;
    const SnapshotStore* snapshots_ = nullptr;  // AS OF 加速:floor 快照 + 增量重放
    /// AS OF 增量重放提供者(含派生钩子,与在线结算同一条 fold 路径);
    /// 由 System 装配注入(WritePipeline::replay_range)。空 = 裸 fold。
    std::function<void(Projection&, int64_t, int64_t)> replay_provider_;
};

} // namespace mse
