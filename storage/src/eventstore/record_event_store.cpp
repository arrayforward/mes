#include "eventstore/record_event_store.h"

#include <algorithm>
#include <unordered_set>

#include "eventstore/errors.h"

namespace eventstore {
namespace {

using namespace storage;

// ---- 表结构声明 ----
// 设计要点：
//   - 索引字段全部声明二级索引（一次写入、多次索引）；
//   - 别名、修饰语拆成独立的索引表（entity_names / profile_modifiers），
//     使 name/alias、modifier 查询在所有后端上都是普通等值查询，
//     不依赖某种数据库特有的 JSON 函数。

TableSchema kNarratives{
    "narratives",
    {{"narrative_id", FieldType::kText}, {"kind", FieldType::kText},
     {"title", FieldType::kText}, {"meta", FieldType::kText}},
    "narrative_id", false, {},
};

TableSchema kEvents{
    "events",
    {{"ord", FieldType::kInt}, {"event_id", FieldType::kText},
     {"narrative_id", FieldType::kText}, {"summary", FieldType::kText},
     {"meta", FieldType::kText}},
    "ord", true,
    {{"event_id"}, {"narrative_id"}},
};

TableSchema kProfiles{
    "profiles",
    {{"seq", FieldType::kInt}, {"profile_id", FieldType::kText},
     {"event_id", FieldType::kText}, {"perspective", FieldType::kText},
     {"time_kind", FieldType::kText}, {"time_value", FieldType::kText},
     {"time_ordinal", FieldType::kInt}, {"place", FieldType::kText},
     {"subject", FieldType::kText}, {"verb", FieldType::kText},
     {"object", FieldType::kText}, {"graph", FieldType::kText},
     {"payload", FieldType::kText}, {"modifiers", FieldType::kText}},
    "seq", true,
    {{"profile_id"}, {"event_id"}, {"subject"}, {"verb"}, {"perspective"},
     {"place"}, {"object"}},
};

TableSchema kLinks{
    "links",
    {{"id", FieldType::kInt}, {"from_profile_id", FieldType::kText},
     {"to_profile_id", FieldType::kText}, {"relation", FieldType::kText}},
    "id", true,
    {{"from_profile_id"}, {"to_profile_id"}},
};

TableSchema kEntities{
    "entities",
    {{"entity_id", FieldType::kText}, {"name", FieldType::kText},
     {"type", FieldType::kText}, {"aliases", FieldType::kText},
     {"attributes", FieldType::kText}},
    "entity_id", false,
    {{"name"}},
};

/// 实体名/别名索引表：一行一个名字（规范名 + 每个别名各一行）。
TableSchema kEntityNames{
    "entity_names",
    {{"id", FieldType::kInt}, {"entity_id", FieldType::kText},
     {"name", FieldType::kText}},
    "id", true,
    {{"name"}, {"entity_id"}},
};

TableSchema kBindings{
    "bindings",
    {{"seq", FieldType::kInt}, {"binding_id", FieldType::kText},
     {"profile_id", FieldType::kText}, {"slot", FieldType::kText},
     {"entity_id", FieldType::kText}, {"confidence", FieldType::kReal},
     {"note", FieldType::kText}},
    "seq", true,
    {{"binding_id"}, {"profile_id", "slot"}, {"entity_id"}},
};

/// 修饰语索引表：一行一个修饰语（与 profiles.modifiers JSON 冗余，专供查询）。
TableSchema kProfileModifiers{
    "profile_modifiers",
    {{"id", FieldType::kInt}, {"profile_id", FieldType::kText},
     {"target", FieldType::kText}, {"kind", FieldType::kText},
     {"text", FieldType::kText}},
    "id", true,
    {{"text"}, {"kind"}, {"profile_id"}},
};

// ---- Record <-> 领域结构 ----

Record to_record(const Narrative& n) {
    return {{"narrative_id", vtext(n.narrative_id)}, {"kind", vtext(n.kind)},
            {"title", vtext(n.title)}, {"meta", vtext(n.meta.dump())}};
}
Narrative narrative_from(const Record& r) {
    return Narrative{as_text(r.at("narrative_id")), as_text(r.at("kind")),
                     as_text(r.at("title")), json::parse(as_text(r.at("meta")))};
}

Record to_record(const Event& e) {
    return {{"event_id", vtext(e.event_id)}, {"narrative_id", vtext(e.narrative_id)},
            {"summary", vtext(e.summary)}, {"meta", vtext(e.meta.dump())}};
}
Event event_from(const Record& r) {
    return Event{as_text(r.at("event_id")), as_text(r.at("narrative_id")),
                 as_text(r.at("summary")), json::parse(as_text(r.at("meta")))};
}

Record to_record(const Profile& p) {
    return {{"profile_id", vtext(p.profile_id)}, {"event_id", vtext(p.event_id)},
            {"perspective", vtext(p.perspective)}, {"time_kind", vtext(p.time.kind)},
            {"time_value", vtext(p.time.value)}, {"time_ordinal", vopt(p.time.ordinal)},
            {"place", vtext(p.place)}, {"subject", vtext(p.subject)},
            {"verb", vtext(p.verb)}, {"object", vtext(p.object)},
            {"graph", vtext(p.graph.dump())}, {"payload", vtext(p.payload.dump())},
            {"modifiers", vtext(json(p.modifiers).dump())}};
}
Profile profile_from(const Record& r) {
    Profile p;
    p.seq = as_int(r.at("seq"));
    p.profile_id = as_text(r.at("profile_id"));
    p.event_id = as_text(r.at("event_id"));
    p.perspective = as_text(r.at("perspective"));
    p.time.kind = as_text(r.at("time_kind"));
    p.time.value = as_text(r.at("time_value"));
    if (!is_null(r.at("time_ordinal"))) p.time.ordinal = as_int(r.at("time_ordinal"));
    p.place = as_text(r.at("place"));
    p.subject = as_text(r.at("subject"));
    p.verb = as_text(r.at("verb"));
    p.object = as_text(r.at("object"));
    p.graph = json::parse(as_text(r.at("graph")));
    p.payload = json::parse(as_text(r.at("payload")));
    p.modifiers = json::parse(as_text(r.at("modifiers"))).get<std::vector<Modifier>>();
    return p;
}

Record to_record(const Link& l) {
    return {{"from_profile_id", vtext(l.from_profile_id)},
            {"to_profile_id", vtext(l.to_profile_id)}, {"relation", vtext(l.relation)}};
}
Link link_from(const Record& r) {
    return Link{as_text(r.at("from_profile_id")), as_text(r.at("to_profile_id")),
                as_text(r.at("relation"))};
}

Record to_record(const Entity& e) {
    return {{"entity_id", vtext(e.entity_id)}, {"name", vtext(e.name)},
            {"type", vtext(e.type)}, {"aliases", vtext(e.aliases.dump())},
            {"attributes", vtext(e.attributes.dump())}};
}
Entity entity_from(const Record& r) {
    return Entity{as_text(r.at("entity_id")), as_text(r.at("name")),
                  as_text(r.at("type")), json::parse(as_text(r.at("aliases"))),
                  json::parse(as_text(r.at("attributes")))};
}

Record to_record(const Binding& b) {
    return {{"binding_id", vtext(b.binding_id)}, {"profile_id", vtext(b.profile_id)},
            {"slot", vtext(b.slot)}, {"entity_id", vtext(b.entity_id)},
            {"confidence", vreal(b.confidence)}, {"note", vtext(b.note)}};
}
Binding binding_from(const Record& r) {
    Binding b;
    b.seq = as_int(r.at("seq"));
    b.binding_id = as_text(r.at("binding_id"));
    b.profile_id = as_text(r.at("profile_id"));
    b.slot = as_text(r.at("slot"));
    b.entity_id = as_text(r.at("entity_id"));
    b.confidence = as_real(r.at("confidence"));
    b.note = as_text(r.at("note"));
    return b;
}

} // namespace

RecordEventStore::RecordEventStore(std::unique_ptr<storage::RecordBackend> backend)
    : backend_(std::move(backend)) {
    backend_->create_table(kNarratives);
    backend_->create_table(kEvents);
    backend_->create_table(kProfiles);
    backend_->create_table(kLinks);
    backend_->create_table(kEntities);
    backend_->create_table(kEntityNames);
    backend_->create_table(kBindings);
    backend_->create_table(kProfileModifiers);
}

// ---- 叙事 / 事件 / 侧写 / 先后边 ----

void RecordEventStore::append_narrative(const Narrative& n) {
    if (backend_->get(kNarratives.name, vtext(n.narrative_id)))
        throw DuplicateError("narrative " + n.narrative_id);
    backend_->put(kNarratives.name, to_record(n));
}

void RecordEventStore::append_event(const Event& e) {
    if (!backend_->get(kNarratives.name, vtext(e.narrative_id)))
        throw NotFoundError("narrative " + e.narrative_id);
    if (!backend_->query(kEvents.name, {{"event_id", Op::Eq, vtext(e.event_id)}}, {}, 1).empty())
        throw DuplicateError("event " + e.event_id);
    backend_->append(kEvents.name, to_record(e));
}

int64_t RecordEventStore::append_profile(const Profile& p) {
    if (backend_->query(kEvents.name, {{"event_id", Op::Eq, vtext(p.event_id)}}, {}, 1).empty())
        throw NotFoundError("event " + p.event_id);
    if (!backend_->query(kProfiles.name, {{"profile_id", Op::Eq, vtext(p.profile_id)}}, {}, 1).empty())
        throw DuplicateError("profile " + p.profile_id);
    int64_t seq = backend_->append(kProfiles.name, to_record(p));
    // 修饰语索引表（冗余写入，专供查询）
    for (const auto& m : p.modifiers) {
        backend_->append(kProfileModifiers.name,
                         {{"profile_id", vtext(p.profile_id)},
                          {"target", vtext(m.target)}, {"kind", vtext(m.kind)},
                          {"text", vtext(m.text)}});
    }
    return seq;
}

void RecordEventStore::append_link(const Link& l) {
    auto exists = [&](const std::string& id) {
        return !backend_->query(kProfiles.name, {{"profile_id", Op::Eq, vtext(id)}}, {}, 1).empty();
    };
    if (!exists(l.from_profile_id)) throw NotFoundError("profile " + l.from_profile_id);
    if (!exists(l.to_profile_id)) throw NotFoundError("profile " + l.to_profile_id);
    backend_->append(kLinks.name, to_record(l));
}

Narrative RecordEventStore::get_narrative(const std::string& id) {
    auto r = backend_->get(kNarratives.name, vtext(id));
    if (!r) throw NotFoundError("narrative " + id);
    return narrative_from(*r);
}

Event RecordEventStore::get_event(const std::string& id) {
    auto rows = backend_->query(kEvents.name, {{"event_id", Op::Eq, vtext(id)}}, {}, 1);
    if (rows.empty()) throw NotFoundError("event " + id);
    return event_from(rows[0]);
}

Profile RecordEventStore::get_profile(const std::string& id) {
    auto rows = backend_->query(kProfiles.name, {{"profile_id", Op::Eq, vtext(id)}}, {}, 1);
    if (rows.empty()) throw NotFoundError("profile " + id);
    return profile_from(rows[0]);
}

std::vector<Event> RecordEventStore::events_of(const std::string& narrative_id) {
    std::vector<Event> out;
    for (const auto& r : backend_->query(kEvents.name,
                                         {{"narrative_id", Op::Eq, vtext(narrative_id)}},
                                         {{"ord", false}}))
        out.push_back(event_from(r));
    return out;
}

std::vector<Profile> RecordEventStore::profiles_of(const std::string& event_id) {
    std::vector<Profile> out;
    for (const auto& r : backend_->query(kProfiles.name,
                                         {{"event_id", Op::Eq, vtext(event_id)}},
                                         {{"seq", false}}))
        out.push_back(profile_from(r));
    return out;
}

std::vector<Link> RecordEventStore::links_from(const std::string& profile_id) {
    std::vector<Link> out;
    for (const auto& r : backend_->query(kLinks.name,
                                         {{"from_profile_id", Op::Eq, vtext(profile_id)}},
                                         {{"id", false}}))
        out.push_back(link_from(r));
    return out;
}

std::vector<Link> RecordEventStore::links_to(const std::string& profile_id) {
    std::vector<Link> out;
    for (const auto& r : backend_->query(kLinks.name,
                                         {{"to_profile_id", Op::Eq, vtext(profile_id)}},
                                         {{"id", false}}))
        out.push_back(link_from(r));
    return out;
}

std::vector<Profile> RecordEventStore::query_profiles(std::optional<std::string> subject,
                                                      std::optional<std::string> verb,
                                                      std::optional<std::string> perspective,
                                                      std::optional<std::string> place,
                                                      std::optional<std::string> object) {
    std::vector<storage::Condition> conds;
    if (subject) conds.push_back({"subject", Op::Eq, vtext(*subject)});
    if (verb) conds.push_back({"verb", Op::Eq, vtext(*verb)});
    if (perspective) conds.push_back({"perspective", Op::Eq, vtext(*perspective)});
    if (place) conds.push_back({"place", Op::Eq, vtext(*place)});
    if (object) conds.push_back({"object", Op::Eq, vtext(*object)});
    std::vector<Profile> out;
    for (const auto& r : backend_->query(kProfiles.name, conds, {{"seq", false}}))
        out.push_back(profile_from(r));
    return out;
}

std::vector<Profile> RecordEventStore::find_profiles_by_modifier(
    std::optional<std::string> text,
    std::optional<std::string> kind,
    std::optional<std::string> target) {
    // 同一行上的 AND = 条件落在同一修饰语上
    std::vector<storage::Condition> conds;
    if (text) conds.push_back({"text", Op::Eq, vtext(*text)});
    if (kind) conds.push_back({"kind", Op::Eq, vtext(*kind)});
    if (target) conds.push_back({"target", Op::Eq, vtext(*target)});
    std::vector<Profile> out;
    std::unordered_set<std::string> seen;
    for (const auto& r : backend_->query(kProfileModifiers.name, conds)) {
        const std::string& pid = as_text(r.at("profile_id"));
        if (!seen.insert(pid).second) continue;
        out.push_back(get_profile(pid));
    }
    std::sort(out.begin(), out.end(),
              [](const Profile& a, const Profile& b) { return a.seq < b.seq; });
    return out;
}

// ---- 实体与指代消解 ----

void RecordEventStore::append_entity(const Entity& e) {
    if (backend_->get(kEntities.name, vtext(e.entity_id)))
        throw DuplicateError("entity " + e.entity_id);
    backend_->put(kEntities.name, to_record(e));
    backend_->append(kEntityNames.name,
                     {{"entity_id", vtext(e.entity_id)}, {"name", vtext(e.name)}});
    for (const auto& alias : e.aliases)
        if (alias.is_string())
            backend_->append(kEntityNames.name,
                             {{"entity_id", vtext(e.entity_id)},
                              {"name", vtext(alias.get<std::string>())}});
}

Entity RecordEventStore::get_entity(const std::string& id) {
    auto r = backend_->get(kEntities.name, vtext(id));
    if (!r) throw NotFoundError("entity " + id);
    return entity_from(*r);
}

std::vector<Entity> RecordEventStore::find_entities_by_name(const std::string& name) {
    std::vector<Entity> out;
    std::unordered_set<std::string> seen;
    for (const auto& r : backend_->query(kEntityNames.name,
                                         {{"name", Op::Eq, vtext(name)}}, {{"id", false}})) {
        const std::string& eid = as_text(r.at("entity_id"));
        if (!seen.insert(eid).second) continue;
        if (auto e = backend_->get(kEntities.name, vtext(eid)))
            out.push_back(entity_from(*e));
    }
    return out;
}

int64_t RecordEventStore::append_binding(const Binding& b) {
    get_profile(b.profile_id);  // 不存在则抛 NotFoundError
    get_entity(b.entity_id);
    return backend_->append(kBindings.name, to_record(b));
}

std::vector<Binding> RecordEventStore::bindings_of(const std::string& profile_id) {
    std::vector<Binding> out;
    for (const auto& r : backend_->query(kBindings.name,
                                         {{"profile_id", Op::Eq, vtext(profile_id)}},
                                         {{"seq", false}}))
        out.push_back(binding_from(r));
    return out;
}

std::vector<Binding> RecordEventStore::bindings_for(const std::string& profile_id,
                                                    const std::string& slot) {
    std::vector<Binding> out;
    for (const auto& r : backend_->query(kBindings.name,
                                         {{"profile_id", Op::Eq, vtext(profile_id)},
                                          {"slot", Op::Eq, vtext(slot)}},
                                         {{"seq", false}}))
        out.push_back(binding_from(r));
    return out;
}

std::optional<Binding> RecordEventStore::effective_binding(const std::string& profile_id,
                                                           const std::string& slot) {
    auto rows = backend_->query(kBindings.name,
                                {{"profile_id", Op::Eq, vtext(profile_id)},
                                 {"slot", Op::Eq, vtext(slot)}},
                                {{"seq", true}}, 1);  // seq DESC LIMIT 1
    if (rows.empty()) return std::nullopt;
    return binding_from(rows[0]);
}

std::vector<Profile> RecordEventStore::profiles_of_entity(const std::string& entity_id,
                                                          std::optional<std::string> slot) {
    // 有效绑定语义：只收"该 (profile,slot) 最新一条 Binding 仍指向此实体"的侧写
    std::vector<storage::Condition> conds{{"entity_id", Op::Eq, vtext(entity_id)}};
    if (slot) conds.push_back({"slot", Op::Eq, vtext(*slot)});
    std::vector<Profile> out;
    std::unordered_set<std::string> seen;
    for (const auto& r : backend_->query(kBindings.name, conds, {{"seq", false}})) {
        Binding b = binding_from(r);
        if (!seen.insert(b.profile_id).second) continue;
        auto eff = effective_binding(b.profile_id, b.slot);
        if (eff && eff->entity_id == entity_id) out.push_back(get_profile(b.profile_id));
    }
    std::sort(out.begin(), out.end(),
              [](const Profile& a, const Profile& b) { return a.seq < b.seq; });
    return out;
}

} // namespace eventstore
