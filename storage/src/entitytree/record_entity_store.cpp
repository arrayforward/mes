#include "entitytree/record_entity_store.h"

#include <algorithm>
#include <unordered_set>

#include "entitytree/errors.h"

namespace entitytree {
namespace {

using namespace storage;

// ---- 表结构声明 ----
// 设计要点（同 eventstore）：
//   - 索引字段全部声明二级索引（一次写入、多次索引）；
//   - 侧写/实体的属性拆成独立的倒排索引表（et_profile_attrs / et_entity_attrs），
//     使"按 (key,value) 召回"在所有后端上都是普通等值查询，
//     不依赖某种数据库特有的 JSON 函数。

TableSchema kObservations{
    "et_observations",
    {{"seq", FieldType::kInt}, {"observation_id", FieldType::kText},
     {"attribute_key", FieldType::kText}, {"value", FieldType::kText},
     {"confidence", FieldType::kReal}, {"source_id", FieldType::kText},
     {"anchor_ref", FieldType::kText}, {"timestamp", FieldType::kInt},
     {"event_ref", FieldType::kText}},
    "seq", true,
    {{"observation_id"}, {"attribute_key"}, {"anchor_ref"}, {"source_id"}},
};

TableSchema kProfiles{
    "et_profiles",
    {{"profile_id", FieldType::kText}, {"anchor_ref", FieldType::kText},
     {"time_bucket", FieldType::kInt}, {"level", FieldType::kInt},
     {"attributes", FieldType::kText},
     {"info_score", FieldType::kReal}, {"status", FieldType::kText},
     {"seq", FieldType::kInt}},
    "profile_id", false,
    {{"anchor_ref"}, {"time_bucket"}, {"status"}, {"level"}},
};

/// 侧写属性倒排索引表：一行一个 (profile, key, value)，随侧写 upsert 整组替换。
TableSchema kProfileAttrs{
    "et_profile_attrs",
    {{"id", FieldType::kInt}, {"profile_id", FieldType::kText},
     {"attribute_key", FieldType::kText}, {"value", FieldType::kText}},
    "id", true,
    {{"profile_id"}, {"attribute_key", "value"}},
};

TableSchema kEntities{
    "et_entities",
    {{"entity_id", FieldType::kText}, {"name", FieldType::kText},
     {"type", FieldType::kText}, {"attributes", FieldType::kText},
     {"view_version", FieldType::kInt}, {"credibility", FieldType::kReal},
     {"status", FieldType::kText}, {"updated_at", FieldType::kInt}},
    "entity_id", false,
    {{"status"}},
};

/// 实体属性倒排索引表：一行一个 (entity, key, value)，随 merged view 重建整组替换。
TableSchema kEntityAttrs{
    "et_entity_attrs",
    {{"id", FieldType::kInt}, {"entity_id", FieldType::kText},
     {"attribute_key", FieldType::kText}, {"value", FieldType::kText}},
    "id", true,
    {{"entity_id"}, {"attribute_key", "value"}},
};

TableSchema kBindings{
    "et_bindings",
    {{"seq", FieldType::kInt}, {"binding_id", FieldType::kText},
     {"profile_id", FieldType::kText}, {"entity_id", FieldType::kText},
     {"confidence", FieldType::kReal}, {"kind", FieldType::kText},
     {"note", FieldType::kText}},
    "seq", true,
    {{"binding_id"}, {"profile_id"}, {"entity_id"}},
};

/// 来源可靠性表：entitytree 与 stmb 共享（同一数据库文件即天然共享）。
TableSchema kSources{
    "et_sources",
    {{"source_id", FieldType::kText}, {"reliability", FieldType::kReal},
     {"updated_at", FieldType::kInt}, {"meta", FieldType::kText}},
    "source_id", false, {},
};

// ---- Record <-> 领域结构 ----

Record to_record(const Observation& o) {
    return {{"observation_id", vtext(o.observation_id)},
            {"attribute_key", vtext(o.attribute_key)}, {"value", vtext(o.value)},
            {"confidence", vreal(o.confidence)}, {"source_id", vtext(o.source_id)},
            {"anchor_ref", vtext(o.anchor_ref)}, {"timestamp", vint(o.timestamp)},
            {"event_ref", vtext(o.event_ref)}};
}
Observation observation_from(const Record& r) {
    Observation o;
    o.seq = as_int(r.at("seq"));
    o.observation_id = as_text(r.at("observation_id"));
    o.attribute_key = as_text(r.at("attribute_key"));
    o.value = as_text(r.at("value"));
    o.confidence = as_real(r.at("confidence"));
    o.source_id = as_text(r.at("source_id"));
    o.anchor_ref = as_text(r.at("anchor_ref"));
    o.timestamp = as_int(r.at("timestamp"));
    o.event_ref = as_text(r.at("event_ref"));
    return o;
}

Record to_record(const AttrProfile& p) {
    return {{"profile_id", vtext(p.profile_id)}, {"anchor_ref", vtext(p.anchor_ref)},
            {"time_bucket", vint(p.time_bucket)}, {"level", vint(p.level)},
            {"attributes", vtext(p.attributes.dump())},
            {"info_score", vreal(p.info_score)}, {"status", vtext(p.status)},
            {"seq", vint(p.seq)}};
}
AttrProfile profile_from(const Record& r) {
    AttrProfile p;
    p.profile_id = as_text(r.at("profile_id"));
    p.anchor_ref = as_text(r.at("anchor_ref"));
    p.time_bucket = as_int(r.at("time_bucket"));
    p.level = (int)as_int(r.at("level"));
    p.attributes = json::parse(as_text(r.at("attributes")));
    p.info_score = as_real(r.at("info_score"));
    p.status = as_text(r.at("status"));
    p.seq = as_int(r.at("seq"));
    return p;
}

Record to_record(const EntityNode& e) {
    return {{"entity_id", vtext(e.entity_id)}, {"name", vtext(e.name)},
            {"type", vtext(e.type)}, {"attributes", vtext(e.attributes.dump())},
            {"view_version", vint(e.view_version)}, {"credibility", vreal(e.credibility)},
            {"status", vtext(e.status)}, {"updated_at", vint(e.updated_at)}};
}
EntityNode entity_from(const Record& r) {
    EntityNode e;
    e.entity_id = as_text(r.at("entity_id"));
    e.name = as_text(r.at("name"));
    e.type = as_text(r.at("type"));
    e.attributes = json::parse(as_text(r.at("attributes")));
    e.view_version = as_int(r.at("view_version"));
    e.credibility = as_real(r.at("credibility"));
    e.status = as_text(r.at("status"));
    e.updated_at = as_int(r.at("updated_at"));
    return e;
}

Record to_record(const EntityBinding& b) {
    return {{"binding_id", vtext(b.binding_id)}, {"profile_id", vtext(b.profile_id)},
            {"entity_id", vtext(b.entity_id)}, {"confidence", vreal(b.confidence)},
            {"kind", vtext(b.kind)}, {"note", vtext(b.note)}};
}
EntityBinding binding_from(const Record& r) {
    EntityBinding b;
    b.seq = as_int(r.at("seq"));
    b.binding_id = as_text(r.at("binding_id"));
    b.profile_id = as_text(r.at("profile_id"));
    b.entity_id = as_text(r.at("entity_id"));
    b.confidence = as_real(r.at("confidence"));
    b.kind = as_text(r.at("kind"));
    b.note = as_text(r.at("note"));
    return b;
}

Record to_record(const SourceRecord& s) {
    return {{"source_id", vtext(s.source_id)}, {"reliability", vreal(s.reliability)},
            {"updated_at", vint(s.updated_at)}, {"meta", vtext(s.meta.dump())}};
}
SourceRecord source_from(const Record& r) {
    SourceRecord s;
    s.source_id = as_text(r.at("source_id"));
    s.reliability = as_real(r.at("reliability"));
    s.updated_at = as_int(r.at("updated_at"));
    s.meta = json::parse(as_text(r.at("meta")));
    return s;
}

/// 从属性 JSON（{key: [{value,...}...]}）提取去重后的 (key, value) 对。
std::vector<std::pair<std::string, std::string>> attr_pairs(const json& attributes) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [key, entries] : attributes.items()) {
        if (!entries.is_array()) continue;
        std::unordered_set<std::string> seen;
        for (const auto& e : entries) {
            std::string v = e.value("value", "");
            if (seen.insert(v).second) out.emplace_back(key, v);
        }
    }
    return out;
}

} // namespace

RecordEntityStore::RecordEntityStore(std::unique_ptr<storage::RecordBackend> backend)
    : backend_(std::move(backend)) {
    backend_->create_table(kObservations);
    backend_->create_table(kProfiles);
    backend_->create_table(kProfileAttrs);
    backend_->create_table(kEntities);
    backend_->create_table(kEntityAttrs);
    backend_->create_table(kBindings);
    backend_->create_table(kSources);
}

// ---- 观测（append-only） ----

int64_t RecordEntityStore::append_observation(const Observation& o) {
    if (!backend_->query(kObservations.name,
                         {{"observation_id", Op::Eq, vtext(o.observation_id)}}, {}, 1).empty())
        throw DuplicateError("observation " + o.observation_id);
    return backend_->append(kObservations.name, to_record(o));
}

Observation RecordEntityStore::get_observation(const std::string& observation_id) {
    auto rows = backend_->query(kObservations.name,
                                {{"observation_id", Op::Eq, vtext(observation_id)}}, {}, 1);
    if (rows.empty()) throw NotFoundError("observation " + observation_id);
    return observation_from(rows[0]);
}

std::vector<Observation> RecordEntityStore::observations_of(const std::string& anchor_ref) {
    std::vector<Observation> out;
    for (const auto& r : backend_->query(kObservations.name,
                                         {{"anchor_ref", Op::Eq, vtext(anchor_ref)}},
                                         {{"seq", false}}))
        out.push_back(observation_from(r));
    return out;
}

std::vector<Observation> RecordEntityStore::query_observations(
    std::optional<std::string> attribute_key,
    std::optional<std::string> source_id,
    std::optional<std::string> anchor_ref) {
    std::vector<storage::Condition> conds;
    if (attribute_key) conds.push_back({"attribute_key", Op::Eq, vtext(*attribute_key)});
    if (source_id) conds.push_back({"source_id", Op::Eq, vtext(*source_id)});
    if (anchor_ref) conds.push_back({"anchor_ref", Op::Eq, vtext(*anchor_ref)});
    std::vector<Observation> out;
    for (const auto& r : backend_->query(kObservations.name, conds, {{"seq", false}}))
        out.push_back(observation_from(r));
    return out;
}

// ---- 侧写 ----

void RecordEntityStore::upsert_profile(const AttrProfile& p) {
    backend_->put(kProfiles.name, to_record(p));
    // 倒排索引整组替换：删旧插新
    for (const auto& r : backend_->query(kProfileAttrs.name,
                                         {{"profile_id", Op::Eq, vtext(p.profile_id)}}))
        backend_->remove(kProfileAttrs.name, r.at("id"));
    for (const auto& [key, value] : attr_pairs(p.attributes))
        backend_->append(kProfileAttrs.name,
                         {{"profile_id", vtext(p.profile_id)},
                          {"attribute_key", vtext(key)}, {"value", vtext(value)}});
}

AttrProfile RecordEntityStore::get_profile(const std::string& profile_id) {
    auto r = backend_->get(kProfiles.name, vtext(profile_id));
    if (!r) throw NotFoundError("profile " + profile_id);
    return profile_from(*r);
}

std::vector<AttrProfile> RecordEntityStore::query_profiles(
    std::optional<std::string> anchor_ref,
    std::optional<int64_t> time_bucket_from,
    std::optional<int64_t> time_bucket_to,
    std::optional<std::string> status,
    std::optional<int> level) {
    std::vector<storage::Condition> conds;
    if (anchor_ref) conds.push_back({"anchor_ref", Op::Eq, vtext(*anchor_ref)});
    if (time_bucket_from) conds.push_back({"time_bucket", Op::Ge, vint(*time_bucket_from)});
    if (time_bucket_to) conds.push_back({"time_bucket", Op::Le, vint(*time_bucket_to)});
    if (status) conds.push_back({"status", Op::Eq, vtext(*status)});
    if (level) conds.push_back({"level", Op::Eq, vint(*level)});
    std::vector<AttrProfile> out;
    for (const auto& r : backend_->query(kProfiles.name, conds, {{"seq", false}}))
        out.push_back(profile_from(r));
    return out;
}

std::vector<AttrProfile> RecordEntityStore::find_profiles_by_attribute(
    const std::string& key, const std::string& value) {
    std::vector<AttrProfile> out;
    std::unordered_set<std::string> seen;
    for (const auto& r : backend_->query(kProfileAttrs.name,
                                         {{"attribute_key", Op::Eq, vtext(key)},
                                          {"value", Op::Eq, vtext(value)}})) {
        const std::string& pid = as_text(r.at("profile_id"));
        if (!seen.insert(pid).second) continue;
        out.push_back(get_profile(pid));
    }
    std::sort(out.begin(), out.end(),
              [](const AttrProfile& a, const AttrProfile& b) { return a.seq < b.seq; });
    return out;
}

// ---- 实体 ----

void RecordEntityStore::upsert_entity(const EntityNode& e) {
    backend_->put(kEntities.name, to_record(e));
    // 倒排索引整组替换：删旧插新
    for (const auto& r : backend_->query(kEntityAttrs.name,
                                         {{"entity_id", Op::Eq, vtext(e.entity_id)}}))
        backend_->remove(kEntityAttrs.name, r.at("id"));
    for (const auto& [key, value] : attr_pairs(e.attributes))
        backend_->append(kEntityAttrs.name,
                         {{"entity_id", vtext(e.entity_id)},
                          {"attribute_key", vtext(key)}, {"value", vtext(value)}});
}

EntityNode RecordEntityStore::get_entity(const std::string& entity_id) {
    auto r = backend_->get(kEntities.name, vtext(entity_id));
    if (!r) throw NotFoundError("entity " + entity_id);
    return entity_from(*r);
}

std::vector<EntityNode> RecordEntityStore::find_entities_by_attribute(
    const std::string& key, const std::string& value) {
    std::vector<EntityNode> out;
    std::unordered_set<std::string> seen;
    for (const auto& r : backend_->query(kEntityAttrs.name,
                                         {{"attribute_key", Op::Eq, vtext(key)},
                                          {"value", Op::Eq, vtext(value)}})) {
        const std::string& eid = as_text(r.at("entity_id"));
        if (!seen.insert(eid).second) continue;
        if (auto e = backend_->get(kEntities.name, vtext(eid)))
            out.push_back(entity_from(*e));
    }
    std::sort(out.begin(), out.end(),
              [](const EntityNode& a, const EntityNode& b) {
                  return a.entity_id < b.entity_id;
              });
    return out;
}

// ---- 绑定 ----

int64_t RecordEntityStore::append_binding(const EntityBinding& b) {
    get_profile(b.profile_id);  // 不存在则抛 NotFoundError
    get_entity(b.entity_id);
    return backend_->append(kBindings.name, to_record(b));
}

std::vector<EntityBinding> RecordEntityStore::bindings_of(const std::string& profile_id) {
    std::vector<EntityBinding> out;
    for (const auto& r : backend_->query(kBindings.name,
                                         {{"profile_id", Op::Eq, vtext(profile_id)}},
                                         {{"seq", false}}))
        out.push_back(binding_from(r));
    return out;
}

std::optional<EntityBinding> RecordEntityStore::effective_binding(
    const std::string& profile_id) {
    auto rows = backend_->query(kBindings.name,
                                {{"profile_id", Op::Eq, vtext(profile_id)}},
                                {{"seq", true}}, 1);  // seq DESC LIMIT 1
    if (rows.empty()) return std::nullopt;
    return binding_from(rows[0]);
}

std::vector<AttrProfile> RecordEntityStore::profiles_of_entity(const std::string& entity_id) {
    // 有效绑定语义：只收"该侧写最新一条 Binding 仍指向此实体"的侧写
    std::vector<AttrProfile> out;
    std::unordered_set<std::string> seen;
    for (const auto& r : backend_->query(kBindings.name,
                                         {{"entity_id", Op::Eq, vtext(entity_id)}},
                                         {{"seq", false}})) {
        EntityBinding b = binding_from(r);
        if (!seen.insert(b.profile_id).second) continue;
        auto eff = effective_binding(b.profile_id);
        if (eff && eff->entity_id == entity_id) out.push_back(get_profile(b.profile_id));
    }
    std::sort(out.begin(), out.end(),
              [](const AttrProfile& a, const AttrProfile& b) { return a.seq < b.seq; });
    return out;
}

// ---- 来源可靠性 ----

void RecordEntityStore::upsert_source(const SourceRecord& s) {
    backend_->put(kSources.name, to_record(s));
}

std::optional<SourceRecord> RecordEntityStore::get_source(const std::string& source_id) {
    auto r = backend_->get(kSources.name, vtext(source_id));
    if (!r) return std::nullopt;
    return source_from(*r);
}

std::vector<SourceRecord> RecordEntityStore::list_sources() {
    std::vector<SourceRecord> out;
    for (const auto& r : backend_->query(kSources.name, {}, {{"source_id", false}}))
        out.push_back(source_from(r));
    return out;
}

} // namespace entitytree
