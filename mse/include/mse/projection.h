#pragma once

// ============================================================================
// mse/projection.h —— 投影引擎:fold 事件 → 本体聚合属性集(属性多属)
//
// 依据实现方案 §2.2 / §3.4 与视图提取文档 §4.7:
//   本体 = 唯一 id(现实事物的数字句柄)+ k-v 属性聚合。没有类。
//   本体的诞生 = 首个携带某 id 键的事件(系统无独立 create)。
//   fold 以属性为单位:事件写入一个键,所有聚合集包含该键的本体观察面同时
//   更新(多属的实现机制)。聚合集声明本身是普通属性 "aggregates":
//   json 数组 [{key, from}]——表示本本体聚合 from 本体的 key 属性;
//   部分-整体 = 聚合集的包含关系,不是类型继承。
//   投影可丢弃可重建:clear + 重放事件日志 = 同一投影(逐比特一致)。
// ============================================================================

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "mse/model.h"

namespace storage { class RecordBackend; }

namespace mse {

class EventLog;

// ---- 快照存取(挂在 storage RecordBackend 上;表 mse_snapshots,append-only) ----
class SnapshotStore {
public:
    /// backend 生命周期须长于本对象。构造即建表(幂等)。
    explicit SnapshotStore(storage::RecordBackend& backend);
    /// 保存一份截至 event_seq 的投影快照。
    void save(int64_t event_seq, const json& projection_snapshot);
    /// 最新快照:(event_seq, payload);无 → std::nullopt。
    std::optional<std::pair<int64_t, json>> latest() const;
    /// 不超过 event_seq 的最近快照(AS OF t = floor 快照 + (seq, t] 增量重放)。
    std::optional<std::pair<int64_t, json>> floor(int64_t event_seq) const;

private:
    storage::RecordBackend& backend_;
};

// ---- 属性格:值 + 来源事件(fold 结果,带审计溯源) ----
struct AttrCell {
    json    value;
    int64_t source_event = 0;   // 写入该值的事件 id(派生属性 = 触发它的事件 id)
};
void to_json(json& j, const AttrCell& c);
void from_json(const json& j, AttrCell& c);

// ---- 本体投影(运行时为内存投影;id 即现实事物的数字句柄) ----
struct Ontology {
    std::string id;
    std::map<std::string, AttrCell> attrs;  // 键按字典序,序列化确定性
    int64_t last_event_id = 0;
};

// ----------------------------------------------------------------------------
// Projection:本体投影表。单写者结算器串行调用,无锁。
// ----------------------------------------------------------------------------
class Projection {
public:
    /// fold 一条已结算事件:逐目标本体、逐属性键写入;再按多属索引同步到
    /// 所有聚合该 (from, key) 的本体观察面。写入 "aggregates" 键时重建该本体
    /// 的聚合索引条目并按既有属性立即回填。
    /// fold 末尾调用派生钩子(若已挂接)——推导规则是 fold 的一部分,只有这样
    /// 重放(rebuild/崩溃恢复)才能与在线结算逐比特一致。
    void apply(const Event& e);

    /// 派生钩子:写侧管线挂接,apply 末尾回调(只跑 derive,不跑 trigger——
    /// 触发产生的事件已在日志中,重放不能再发)。崩溃恢复/重建路径同样经过。
    void set_derive_hook(std::function<void(Projection&, const Event&)> hook);

    /// 派生属性写入(推导规则的产物仍是普通属性,可被多本体聚合)。
    void write_derived(const std::string& ontology_id, const std::string& key,
                       const json& value, int64_t source_event);

    const Ontology* find(const std::string& id) const;
    /// 全部本体(按 id 字典序)。
    std::vector<const Ontology*> ontologies() const;

    void clear();
    /// 重建:clear + 逐事件重放(投影可丢弃可重建;崩溃恢复 = 日志重放)。
    void rebuild(const EventLog& log);

    /// 确定性哈希:对 (id 字典序, 键字典序, value 规范序列化, source_event)
    /// 做 FNV-1a 64 位折叠,返回 16 位十六进制串。同输入重放必须逐比特一致。
    std::string hash() const;

    // ---- 快照(定距增量快照的基座;崩溃恢复 = 最近快照 + 日志增量重放) ----
    /// 全量投影的确定性 JSON 快照:{ontologies: {id: {attrs: {k: {value, source_event}}, last_event_id}}}。
    json snapshot_json() const;
    /// 从快照恢复(先 clear)。payload 非法抛 std::runtime_error。
    void load_snapshot(const json& payload);

    /// 预览辅助:目标本体"变化后"的属性集(当前属性 ∪ 候选写入;
    /// 决策环预演用——只读副本,不落任何状态)。
    json preview_attrs(const std::string& id, const Candidate& c) const;

    /// 当前属性集(纯值视图:{键: 值})。
    json attrs_of(const std::string& id) const;

private:
    std::map<std::string, Ontology> ontologies_;
    // 多属索引:(来源本体 id, 属性键) → 聚合它的本体 id 集
    std::map<std::pair<std::string, std::string>, std::set<std::string>> aggregate_index_;
    std::function<void(Projection&, const Event&)> derive_hook_;  // 可空

    void write_cell(const std::string& id, const std::string& key,
                    const json& value, int64_t source_event);
    void rebuild_aggregate_entries(const std::string& id);
};

} // namespace mse
