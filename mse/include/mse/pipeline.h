#pragma once

// ============================================================================
// mse/pipeline.h —— 写侧管线:四层校验 + 决策环(单写者结算器)
//
// 依据《系统API设计》§三 与实现方案 §3.2:
//   提交 → [0]字典登记校验(所有键已登记且 active?未登记=系统不认识,拒绝)
//        → [1]可聚合性校验(writes 非空、id 键非空?不含 id 键的变化不予结算)
//        → [2]类型 schema 校验(类型已注册?必填键齐?该类型被授权写这些键?
//               值在值域内?multi_target 约束?)
//        → [3]规则过滤(决策环预演:候选+当前投影副本→规则求值;
//               不通过即内存回收,零事件零补偿零污染,返回违反的规则清单)
//        → 结算:单写者 append 事件日志(域内全序)→ 投影 fold → 时空存证
//               → 推导规则(派生属性写回投影)→ 触发规则(新候选回到本管线)
//   幂等:候选层幂等键,重复提交返回原回执。
// ============================================================================

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mse/model.h"

namespace storage { class RecordBackend; }

namespace mse {

class DefinitionLayer;
class EventLog;
class Projection;
class RuleEngine;
class SnapshotStore;
class SpacetimeTree;

// ---- 拦截记录(L0 候选的即时反馈;拦截模式视图的数据源;内存环形缓冲) ----
struct Rejection {
    Candidate  candidate;
    int        layer = -1;
    std::vector<std::string> violations;
    int64_t    seen_seq = 0;   // 观察到时的日志长度(逻辑时刻)
};

class RejectionLog {
public:
    explicit RejectionLog(size_t capacity = 256) : capacity_(capacity) {}
    void push(Rejection r);
    std::vector<Rejection> recent(size_t limit) const;  // 新的在前
private:
    size_t capacity_;
    std::deque<Rejection> buf_;
};

// ----------------------------------------------------------------------------
// WritePipeline:决策环。单写者——全部 submit 串行调用,无锁。
// ----------------------------------------------------------------------------
class WritePipeline {
public:
    WritePipeline(DefinitionLayer& defs, EventLog& log, Projection& proj,
                  SpacetimeTree& spacetime, RuleEngine& rules);

    /// 提交变化描述:四层校验 → 预演 → 结算。同步类型立即结算返回回执;
    /// 异步类型(settlement=="async")四层校验通过后入队,返回 kAccepted
    /// (含队列序号),由 drain_async 统一串行结算(单写者,全序保持)。
    /// 被拒候选进入 RejectionLog(若已挂接),不进事件树。
    Receipt submit(const Candidate& c);

    /// 结算异步队列中的候选(单写者串行)。max=0 表示全部。返回结算条数。
    size_t drain_async(size_t max = 0);
    /// 异步队列中待结算的候选数。
    size_t async_pending() const;

    void set_rejection_log(RejectionLog* log);   // 挂接拦截记录(拦截模式视图用)
    void set_trigger_depth_limit(int d);         // 触发规则递归深度上限(默认 4)

    /// 定距快照:每 n 条已结算事件写一份投影快照(0 = 关闭,默认)。
    /// store 由调用方持有(SnapshotStore 挂在同一 RecordBackend 上)。
    void set_snapshots(SnapshotStore* store, int64_t interval);

    /// 幂等键持久化(默认仅内存):挂接后重复提交跨进程重启仍返回原回执。
    /// 表 mse_idempotency(pk = 幂等键,值为回执 JSON)由管线自建(幂等)。
    void set_idempotency_store(storage::RecordBackend* backend);

    /// 读写一致性的共用入口:对某事件类型 + 目标本体当前属性集求值
    /// 该类型引用的全部适用 filter 规则,返回拒绝理由清单(空 = 可结算)。
    /// 候选写入的新值由规则经 {"write":...} 算子读取(如状态机校验迁移);
    /// 写侧 L3 与读侧按钮可用性调用同一份——"界面上能点的 ⇔ 系统能结算的"。
    std::vector<std::string> check_filters(const std::string& type,
                                           const std::string& target_id,
                                           const json& attrs_current,
                                           const Candidate& candidate) const;

    /// 重放重建投影(含派生属性):新建 Projection、挂同一派生钩子、逐事件 fold。
    /// 审计重放/验证"重放逐比特一致"的统一入口——与在线结算同一条 fold 路径。
    Projection replay_projection(const EventLog& log);

    /// 区间重放到指定投影(含派生属性):挂同一派生钩子后 fold [from_seq, to_seq]。
    /// 视图引擎 AS OF(快照 + 增量重放)经此获得与在线一致的派生属性。
    void replay_range(Projection& p, const EventLog& log, int64_t from_seq, int64_t to_seq);

private:
    DefinitionLayer& defs_;
    EventLog&        log_;
    Projection&      proj_;
    SpacetimeTree&   spacetime_;
    RuleEngine&      rules_;
    RejectionLog*    rejections_ = nullptr;
    int              trigger_depth_limit_ = 4;

    std::map<std::string, Receipt> idempotency_;  // 幂等键 → 原回执

    // 异步结算边界:settlement=="async" 的类型四层校验通过后在此排队,
    // 由 drain_async 单写者 FIFO 串行结算(全序保持)。
    std::deque<Candidate> async_queue_;  // 已验未结的悬态候选(见 submit 注释)
    int64_t               async_seq_ = 0;  // 异步队列序号(回执 queue_seq)

    SnapshotStore*          snapshots_ = nullptr;         // 定距快照库(可空 = 关)
    int64_t                 snapshot_interval_ = 0;       // 快照间隔(0 = 关)
    storage::RecordBackend* idem_backend_ = nullptr;      // 幂等键持久化(可空 = 仅内存)

    Receipt submit_inner(const Candidate& c, int depth);
    Receipt submit_entry(const Candidate& c);             // 顶层入口:校验后按 settlement 分流
    Receipt validate_all(const Candidate& c) const;       // L0-L3 顺序短路
    Receipt validate_layer0(const Candidate& c) const;  // 字典登记
    Receipt validate_layer1(const Candidate& c) const;  // 可聚合性
    Receipt validate_layer2(const Candidate& c) const;  // 类型 schema + 写授权 + 值域
    Receipt validate_layer3(const Candidate& c) const;  // 规则过滤(预演)
    void    settle(const Candidate& c, int depth, Receipt& out);  // 结算 + 触发
    // 推导规则求值(派生属性写回投影):挂进 Projection 的派生钩子,fold 的一部分
    void    derive_for_event(Projection& p, const Event& e);
};

} // namespace mse
