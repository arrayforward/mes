// ============================================================================
// mse/projection.cpp —— 投影引擎:fold 事件 → 本体聚合属性集(属性多属)
//
// fold 以属性为单位:事件写入一个键,所有聚合集包含该键的本体观察面同时
// 更新(aggregate_index_ 即多属索引)。聚合声明本身是普通属性
// "aggregates"(json 数组 [{key, from}]),写入时重建该本体的索引条目并
// 按既有属性立即回填。
// 投影可丢弃可重建:clear + 重放事件日志 = 同一投影(逐比特一致,hash 校验)。
// ============================================================================

#include "mse/projection.h"

#include <cstdio>
#include <stdexcept>
#include <utility>

#include "mse/event_log.h"
#include "storage/record_backend.h"

namespace mse {

// ----------------------------------------------------------------------------
// SnapshotStore:投影快照存取(表 mse_snapshots,append-only)
//   seq 为自增主键(物理序);event_seq 为逻辑序号(快照截至的事件 id),
//   latest/floor 都按 event_seq 查询——同一 backend 上可能有多个写者实例,
//   逻辑序号才是恢复语义的依据。
// ----------------------------------------------------------------------------

namespace {

constexpr const char* kSnapshotTable = "mse_snapshots";

storage::TableSchema snapshot_table_schema() {
    storage::TableSchema s;
    s.name = kSnapshotTable;
    s.fields = {
        {"seq", storage::FieldType::kInt},        // 自增主键(后端分配)
        {"event_seq", storage::FieldType::kInt},  // 快照截至的事件 id(逻辑序号)
        {"payload", storage::FieldType::kText},   // 投影快照 JSON dump
    };
    s.pk = "seq";
    s.auto_seq = true;
    s.indexes = {{"event_seq"}};
    return s;
}

} // namespace

SnapshotStore::SnapshotStore(storage::RecordBackend& backend) : backend_(backend) {
    backend_.create_table(snapshot_table_schema());  // 幂等
}

void SnapshotStore::save(int64_t event_seq, const json& projection_snapshot) {
    storage::Record rec;
    rec["event_seq"] = storage::vint(event_seq);
    rec["payload"]   = storage::vtext(projection_snapshot.dump());
    (void)backend_.append(kSnapshotTable, std::move(rec));
}

std::optional<std::pair<int64_t, json>> SnapshotStore::latest() const {
    const auto rows = backend_.query(kSnapshotTable, {}, {{"event_seq", true}}, 1);
    if (rows.empty()) return std::nullopt;
    return std::make_pair(storage::as_int(rows[0].at("event_seq")),
                          json::parse(storage::as_text(rows[0].at("payload"))));
}

std::optional<std::pair<int64_t, json>> SnapshotStore::floor(int64_t event_seq) const {
    const auto rows =
        backend_.query(kSnapshotTable,
                       {{"event_seq", storage::Op::Le, storage::vint(event_seq)}},
                       {{"event_seq", true}}, 1);
    if (rows.empty()) return std::nullopt;
    return std::make_pair(storage::as_int(rows[0].at("event_seq")),
                          json::parse(storage::as_text(rows[0].at("payload"))));
}

// ---- AttrCell 序列化(投影快照/审计用) ----
void to_json(json& j, const AttrCell& c) {
    j = json{{"value", c.value}, {"source_event", c.source_event}};
}

void from_json(const json& j, AttrCell& c) {
    if (auto it = j.find("value"); it != j.end()) c.value = *it;
    c.source_event = 0;
    if (auto it = j.find("source_event"); it != j.end() && it->is_number())
        c.source_event = it->get<int64_t>();
}

// ----------------------------------------------------------------------------
// 写入单格:值 + 来源事件;last_event_id 取 max(单调审计水位)
// ----------------------------------------------------------------------------

void Projection::write_cell(const std::string& id, const std::string& key,
                            const json& value, int64_t source_event) {
    Ontology& ont = ontologies_[id];
    ont.id = id;  // operator[] 默认构造的 Ontology id 为空,此处补上
    ont.attrs[key] = AttrCell{value, source_event};
    if (source_event > ont.last_event_id) ont.last_event_id = source_event;
}

// ----------------------------------------------------------------------------
// 重建某本体的聚合索引条目:摘除全部旧条目 → 按新声明注册 → 立即回填
// ----------------------------------------------------------------------------

void Projection::rebuild_aggregate_entries(const std::string& id) {
    // 1) 从索引移除该本体作为聚合者的全部旧条目
    for (auto it = aggregate_index_.begin(); it != aggregate_index_.end();) {
        it->second.erase(id);
        if (it->second.empty())
            it = aggregate_index_.erase(it);
        else
            ++it;
    }

    // 2) 读取该本体当前的 "aggregates" 声明([{key, from}, ...])
    auto oit = ontologies_.find(id);
    if (oit == ontologies_.end()) return;
    auto ait = oit->second.attrs.find("aggregates");
    if (ait == oit->second.attrs.end() || !ait->second.value.is_array()) return;

    for (const auto& decl : ait->second.value) {
        if (!decl.is_object()) continue;
        auto kit = decl.find("key");
        auto fit = decl.find("from");
        if (kit == decl.end() || fit == decl.end() || !kit->is_string() ||
            !fit->is_string())
            continue;
        const std::string key = kit->get<std::string>();
        const std::string from = fit->get<std::string>();

        // 3) 注册 (from, key) → id
        aggregate_index_[{from, key}].insert(id);

        // 4) 回填:来源本体当前已有该属性,立即把值写进聚合者观察面
        //    (source_event 沿用来源格子的来源事件——审计溯源不断链)
        auto sit = ontologies_.find(from);
        if (sit == ontologies_.end()) continue;
        auto cit = sit->second.attrs.find(key);
        if (cit == sit->second.attrs.end()) continue;
        write_cell(id, key, cit->second.value, cit->second.source_event);
    }
}

// ----------------------------------------------------------------------------
// fold 一条已结算事件
// ----------------------------------------------------------------------------

void Projection::apply(const Event& e) {
    std::vector<std::pair<std::string, std::string>> written;  // (目标id, 键)

    // 1) 逐目标本体、逐属性键写入(writes 内层为 json 对象:{键: 新值})
    for (const auto& [id, kvs] : e.writes) {
        if (!kvs.is_object()) continue;
        for (auto it = kvs.begin(); it != kvs.end(); ++it) {
            const std::string& key = it.key();
            const json& value = it.value();
            write_cell(id, key, value, e.event_id);
            written.emplace_back(id, key);
            // 聚合声明本身是普通属性:先更新属性本身,再重建索引条目并回填
            if (key == "aggregates") rebuild_aggregate_entries(id);
        }
    }

    // 2) 多属同步:刚写的每个 (目标id, 键),同步到所有聚合该格的本体观察面
    for (const auto& [id, key] : written) {
        auto it = aggregate_index_.find({id, key});
        if (it == aggregate_index_.end()) continue;
        const json& value = ontologies_.at(id).attrs.at(key).value;
        for (const auto& aggregator : it->second)
            write_cell(aggregator, key, value, e.event_id);
    }

    // 3) 派生钩子:推导规则是 fold 的一部分(重放与在线结算同路径,逐比特一致)
    if (derive_hook_) derive_hook_(*this, e);
}

void Projection::set_derive_hook(std::function<void(Projection&, const Event&)> hook) {
    derive_hook_ = std::move(hook);
}

void Projection::write_derived(const std::string& ontology_id,
                               const std::string& key, const json& value,
                               int64_t source_event) {
    // 派生属性仍是普通属性:写入 + 多属同步
    write_cell(ontology_id, key, value, source_event);
    auto it = aggregate_index_.find({ontology_id, key});
    if (it == aggregate_index_.end()) return;
    for (const auto& aggregator : it->second)
        write_cell(aggregator, key, value, source_event);
}

const Ontology* Projection::find(const std::string& id) const {
    auto it = ontologies_.find(id);
    return it == ontologies_.end() ? nullptr : &it->second;
}

std::vector<const Ontology*> Projection::ontologies() const {
    std::vector<const Ontology*> out;
    out.reserve(ontologies_.size());
    for (const auto& [id, ont] : ontologies_) out.push_back(&ont);
    return out;  // std::map 迭代序即 id 字典序
}

void Projection::clear() {
    ontologies_.clear();
    aggregate_index_.clear();
}

void Projection::rebuild(const EventLog& log) {
    clear();
    for (const auto& e : log.all()) apply(e);
}

// ----------------------------------------------------------------------------
// 快照:全量投影的确定性 JSON(std::map 迭代天然字典序,输出确定性)
//   {"ontologies": {id: {"attrs": {k: {"value":..,"source_event":n}},
//                        "last_event_id": n}}}
// ----------------------------------------------------------------------------

json Projection::snapshot_json() const {
    json ontologies = json::object();
    for (const auto& [id, ont] : ontologies_) {
        json attrs = json::object();
        for (const auto& [key, cell] : ont.attrs) attrs[key] = cell;  // AttrCell to_json
        ontologies[id] = json{{"attrs", std::move(attrs)},
                              {"last_event_id", ont.last_event_id}};
    }
    return json{{"ontologies", std::move(ontologies)}};
}

void Projection::load_snapshot(const json& payload) {
    auto bad = [](const std::string& what) {
        throw std::runtime_error("Projection::load_snapshot: 快照形状非法(" + what + ")");
    };
    if (!payload.is_object() || !payload.contains("ontologies") ||
        !payload.at("ontologies").is_object())
        bad("缺少 object 字段 ontologies");
    for (const auto& [id, oj] : payload.at("ontologies").items()) {
        if (!oj.is_object() || !oj.contains("attrs") || !oj.at("attrs").is_object())
            bad("本体 " + id + " 缺少 object 字段 attrs");
        if (!oj.contains("last_event_id") || !oj.at("last_event_id").is_number())
            bad("本体 " + id + " 缺少数值字段 last_event_id");
        for (const auto& [key, cj] : oj.at("attrs").items()) {
            if (!cj.is_object() || !cj.contains("value") ||
                !cj.contains("source_event") || !cj.at("source_event").is_number())
                bad("本体 " + id + " 属性 " + key + " 的格子形状非法");
        }
    }

    clear();
    for (const auto& [id, oj] : payload.at("ontologies").items()) {
        Ontology& ont = ontologies_[id];
        ont.id = id;
        ont.last_event_id = oj.at("last_event_id").get<int64_t>();
        for (const auto& [key, cj] : oj.at("attrs").items()) {
            AttrCell cell;
            from_json(cj, cell);
            ont.attrs[key] = std::move(cell);
        }
    }
    // 聚合索引同样要重建:含 "aggregates" 声明的本体逐个重建索引条目,
    // 回填语义与 apply 一致(source_event 沿用来源格子的,审计溯源不断链)。
    // 此刻全部本体属性已就位,回填读到的是快照终态。
    for (const auto& [id, ont] : ontologies_) {
        if (ont.attrs.count("aggregates") != 0) rebuild_aggregate_entries(id);
    }
}

// ----------------------------------------------------------------------------
// 确定性哈希:FNV-1a 64 位
//   输入序列:id 字典序 → 键字典序 → value.dump() → source_event,
//   字段间以 0x1f、记录间以 0x1e 分隔(消除拼接歧义);只依赖 std::map
//   迭代序与 value.dump()(对象键字典序),不依赖任何其他状态。
// ----------------------------------------------------------------------------

std::string Projection::hash() const {
    constexpr std::uint64_t kOffset = 14695981039346656037ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t h = kOffset;
    auto feed = [&](const std::string& s) {
        for (unsigned char b : s) {
            h ^= b;
            h *= kPrime;
        }
    };
    for (const auto& [id, ont] : ontologies_) {
        for (const auto& [key, cell] : ont.attrs) {
            feed(id);
            feed("\x1f");
            feed(key);
            feed("\x1f");
            feed(cell.value.dump());
            feed("\x1f");
            feed(std::to_string(cell.source_event));
            feed("\x1e");
        }
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(buf, 16);  // 16 位十六进制,小写,前导零保留
}

// ----------------------------------------------------------------------------
// 预览:当前属性(纯值) ∪ 候选对应该 id 的 writes 覆盖;只读,不落状态
// ----------------------------------------------------------------------------

json Projection::preview_attrs(const std::string& id, const Candidate& c) const {
    json out = attrs_of(id);
    auto it = c.writes.find(id);
    if (it != c.writes.end() && it->second.is_object()) {
        for (auto kit = it->second.begin(); kit != it->second.end(); ++kit)
            out[kit.key()] = kit.value();
    }
    return out;
}

json Projection::attrs_of(const std::string& id) const {
    json out = json::object();
    auto it = ontologies_.find(id);
    if (it == ontologies_.end()) return out;
    for (const auto& [key, cell] : it->second.attrs) out[key] = cell.value;
    return out;
}

} // namespace mse
