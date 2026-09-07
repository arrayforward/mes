#pragma once

// ============================================================================
// mse/event_log.h —— 事件日志(append-only,域内全序)
//
// 依据实现方案 §3.1:单车间单域起步,一个分区一根日志,域内全序。
// 挂在 storage 底座之上(RecordBackend SPI):表 mse_events 为 auto_seq
// append-only 表;运行期持有内存镜像,启动时从后端重放重建。
// 铁律:只 append,已结算事件永不修改——接口不提供任何 update/delete。
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mse/model.h"

namespace storage { class RecordBackend; }

namespace mse {

class EventLog {
public:
    /// backend 生命周期须长于本对象。构造即建表(幂等)并从后端重放到内存镜像。
    explicit EventLog(storage::RecordBackend& backend);

    /// 追加一条已结算事件(调用方须已填好 event_id/settle_seq 以外的字段;
    /// event_id 与 settle_seq 由本日志分配,保证域内单调递增、二者相等)。
    /// 返回分配的 event_id。
    int64_t append(Event e);

    /// 事件总数(== 下一条将分配的 event_id;event_id 从 1 开始)。
    int64_t size() const;

    /// 按 event_id 取事件(1..size()),越界返回 std::nullopt。
    std::optional<Event> get(int64_t event_id) const;

    /// 区间取事件 [from_seq, to_seq](闭区间,1 起),按 settle_seq 升序。
    std::vector<Event> range(int64_t from_seq, int64_t to_seq) const;

    /// 全量事件(按 settle_seq 升序)——重放/投影重建用。
    const std::vector<Event>& all() const;

    /// 索引查询:某事件类型的全部事件,按 settle_seq 升序。
    std::vector<Event> events_of_type(const std::string& type) const;

    /// 索引查询:写及某本体 id 的全部事件,按 settle_seq 升序。
    std::vector<Event> events_touching(const std::string& ontology_id) const;

    /// 索引查询:corrects == 原事件 id 的全部修正事件(L2),按 settle_seq 升序。
    std::vector<Event> corrections_of(int64_t original_event_id) const;

private:
    storage::RecordBackend& backend_;
    std::vector<Event>      events_;  // 内存镜像,下标 = event_id - 1
};

} // namespace mse
