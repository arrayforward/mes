// ============================================================================
// mse/event_log.cpp —— 事件日志(append-only,域内全序)
//
// 挂在 storage RecordBackend 之上:表 mse_events 为 auto_seq append-only
// 表(pk = "seq",由后端分配);运行期持有内存镜像 events_,构造时从后端
// 全表按 seq 升序重放重建。铁律:只 append,已结算事件永不修改。
//
// 确定性纪律:结算逻辑时刻就是 settle_seq(逻辑时钟),不读物理时钟。
// ============================================================================

#include "mse/event_log.h"

#include <stdexcept>
#include <utility>

#include "storage/record_backend.h"

namespace mse {

namespace {

constexpr const char* kTable = "mse_events";

// 表结构(幂等建表):seq 为自增主键;type/corrects 建二级索引
storage::TableSchema event_table_schema() {
    storage::TableSchema s;
    s.name = kTable;
    s.fields = {
        {"seq", storage::FieldType::kInt},         // 自增主键(后端分配)
        {"type", storage::FieldType::kText},       // 事件类型
        {"payload", storage::FieldType::kText},    // Event 完整 JSON dump
        {"occur_time", storage::FieldType::kText}, // 发生时间原文
        {"settle_time", storage::FieldType::kText},
        {"corrects", storage::FieldType::kInt},    // 被修正事件 id;无修正存 -1
    };
    s.pk = "seq";
    s.auto_seq = true;
    s.indexes = {{"type"}, {"corrects"}};
    return s;
}

} // namespace

EventLog::EventLog(storage::RecordBackend& backend) : backend_(backend) {
    backend_.create_table(event_table_schema());  // 幂等

    // 启动重放:全表按 seq 升序,逐条反序列化进内存镜像
    const auto rows = backend_.query(kTable, {}, {{"seq", false}}, 0);
    events_.reserve(rows.size());
    for (const auto& row : rows) {
        const int64_t seq = storage::as_int(row.at("seq"));
        Event e = json::parse(storage::as_text(row.at("payload"))).get<Event>();
        // payload 内已含 event_id/settle_seq,必须与物理 seq 一致
        if (e.event_id != seq || e.settle_seq != seq) {
            throw std::runtime_error(
                "EventLog: seq/payload mismatch at seq " + std::to_string(seq) +
                " (event_id=" + std::to_string(e.event_id) +
                ", settle_seq=" + std::to_string(e.settle_seq) + ")");
        }
        events_.push_back(std::move(e));
    }
}

int64_t EventLog::append(Event e) {
    // event_id 与 settle_seq 由本日志分配:域内单调递增、二者相等;
    // 结算逻辑时刻 = settle_seq(逻辑时钟,不读物理时钟——确定性纪律)
    e.event_id = e.settle_seq = static_cast<int64_t>(events_.size()) + 1;
    if (e.settle_time.empty()) e.settle_time = "";  // 结算时刻缺省为空串

    storage::Record rec;
    rec["type"] = storage::vtext(e.type);
    rec["payload"] = storage::vtext(json(e).dump());
    rec["occur_time"] = storage::vtext(e.occur_time);
    rec["settle_time"] = storage::vtext(e.settle_time);
    rec["corrects"] = storage::vint(e.corrects ? *e.corrects : -1);

    const int64_t seq = backend_.append(kTable, std::move(rec));
    if (seq != e.event_id) {
        throw std::runtime_error("EventLog: backend seq " + std::to_string(seq) +
                                 " != allocated event_id " +
                                 std::to_string(e.event_id));
    }
    events_.push_back(std::move(e));
    return events_.back().event_id;
}

int64_t EventLog::size() const {
    return static_cast<int64_t>(events_.size());
}

std::optional<Event> EventLog::get(int64_t event_id) const {
    if (event_id < 1 || event_id > static_cast<int64_t>(events_.size()))
        return std::nullopt;
    return events_[static_cast<std::size_t>(event_id) - 1];
}

std::vector<Event> EventLog::range(int64_t from_seq, int64_t to_seq) const {
    std::vector<Event> out;
    if (from_seq > to_seq) return out;
    for (const auto& e : events_) {
        if (e.settle_seq >= from_seq && e.settle_seq <= to_seq) out.push_back(e);
    }
    return out;
}

const std::vector<Event>& EventLog::all() const {
    return events_;
}

std::vector<Event> EventLog::events_of_type(const std::string& type) const {
    std::vector<Event> out;
    for (const auto& e : events_) {
        if (e.type == type) out.push_back(e);
    }
    return out;
}

std::vector<Event> EventLog::events_touching(const std::string& ontology_id) const {
    std::vector<Event> out;
    for (const auto& e : events_) {
        if (e.writes.find(ontology_id) != e.writes.end()) out.push_back(e);
    }
    return out;
}

std::vector<Event> EventLog::corrections_of(int64_t original_event_id) const {
    std::vector<Event> out;
    for (const auto& e : events_) {
        if (e.corrects && *e.corrects == original_event_id) out.push_back(e);
    }
    return out;
}

} // namespace mse
