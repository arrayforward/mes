#include "eventstore/model.h"

#include <atomic>
#include <cstdio>
#include <random>

namespace eventstore {

std::string new_id(const std::string& prefix) {
    static std::mt19937_64 rng{std::random_device{}()};
    static std::atomic<uint64_t> counter{0};
    uint64_t hi = rng();
    uint64_t lo = rng() ^ (counter.fetch_add(1) * 0x9e3779b97f4a7c15ULL);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%08llx%08llx",
                  (unsigned long long)(hi & 0xffffffffULL),
                  (unsigned long long)(lo & 0xffffffffULL));
    return prefix + "-" + buf;
}

// ---- TimeRef ----

TimeRef TimeRef::real(std::string iso_timestamp) {
    return TimeRef{"real", std::move(iso_timestamp), std::nullopt};
}
TimeRef TimeRef::virtual_time(std::string label, std::optional<int64_t> ordinal) {
    return TimeRef{"virtual", std::move(label), ordinal};
}

// ---- create 工厂 ----

Narrative Narrative::create(std::string kind, std::string title, json meta) {
    return Narrative{new_id("na"), std::move(kind), std::move(title), std::move(meta)};
}
Event Event::create(std::string narrative_id, std::string summary, json meta) {
    return Event{new_id("ev"), std::move(narrative_id), std::move(summary), std::move(meta)};
}
Profile Profile::create(std::string event_id, std::string perspective, TimeRef time,
                        std::string place, std::string subject, std::string verb,
                        std::string object, json payload, std::vector<Modifier> modifiers) {
    Profile p;
    p.profile_id = new_id("pf");
    p.event_id = std::move(event_id);
    p.perspective = std::move(perspective);
    p.time = std::move(time);
    p.place = std::move(place);
    p.subject = std::move(subject);
    p.verb = std::move(verb);
    p.object = std::move(object);
    p.payload = std::move(payload);
    p.modifiers = std::move(modifiers);
    p.graph = build_graph(p);
    return p;
}
Entity Entity::create(std::string name, std::string type, json aliases, json attributes) {
    return Entity{new_id("en"), std::move(name), std::move(type),
                  std::move(aliases), std::move(attributes)};
}
Binding Binding::create(std::string profile_id, std::string slot, std::string entity_id,
                        double confidence, std::string note) {
    Binding b;
    b.binding_id = new_id("bd");
    b.profile_id = std::move(profile_id);
    b.slot = std::move(slot);
    b.entity_id = std::move(entity_id);
    b.confidence = confidence;
    b.note = std::move(note);
    return b;
}

// ---- 图构建 ----

json build_graph(const Profile& p) {
    json nodes = json::array({
        {{"id", "subject"}, {"role", "subject"}, {"label", p.subject}},
        {{"id", "object"},  {"role", "object"},  {"label", p.object}},
        {{"id", "place"},   {"role", "place"},   {"label", p.place}},
        {{"id", "time"},    {"role", "time"},    {"label", p.time.value}},
    });
    json edges = json::array({
        {{"from", "subject"}, {"to", "object"}, {"relation", "action"},     {"label", p.verb}},
        {{"from", "subject"}, {"to", "place"},  {"relation", "located_at"}, {"label", p.place}},
        {{"from", "subject"}, {"to", "time"},   {"relation", "occurs_at"},  {"label", p.time.value}},
    });
    // 修饰语：每个 Modifier 一个节点，一条从附着点指出的修饰边
    for (size_t i = 0; i < p.modifiers.size(); ++i) {
        const auto& m = p.modifiers[i];
        std::string nid = "mod" + std::to_string(i);
        nodes.push_back({{"id", nid}, {"role", "modifier"}, {"kind", m.kind}, {"label", m.text}});
        edges.push_back({{"from", m.target}, {"to", nid}, {"relation", m.kind}, {"label", m.text}});
    }
    return {{"nodes", nodes}, {"edges", edges}};
}

// ---- 序列化 ----

void to_json(json& j, const TimeRef& t) {
    j = json{{"kind", t.kind}, {"value", t.value}};
    if (t.ordinal) j["ordinal"] = *t.ordinal;
}
void from_json(const json& j, TimeRef& t) {
    t.kind = j.at("kind").get<std::string>();
    t.value = j.at("value").get<std::string>();
    t.ordinal = std::nullopt;
    if (j.contains("ordinal") && !j["ordinal"].is_null())
        t.ordinal = j["ordinal"].get<int64_t>();
}

void to_json(json& j, const Modifier& m) {
    j = json{{"target", m.target}, {"kind", m.kind}, {"text", m.text}};
}
void from_json(const json& j, Modifier& m) {
    m.target = j.at("target").get<std::string>();
    m.kind = j.at("kind").get<std::string>();
    m.text = j.at("text").get<std::string>();
}

void to_json(json& j, const Narrative& n) {
    j = json{{"narrative_id", n.narrative_id}, {"kind", n.kind},
             {"title", n.title}, {"meta", n.meta}};
}
void from_json(const json& j, Narrative& n) {
    n.narrative_id = j.at("narrative_id").get<std::string>();
    n.kind = j.at("kind").get<std::string>();
    n.title = j.value("title", "");
    n.meta = j.value("meta", json::object());
}

void to_json(json& j, const Event& e) {
    j = json{{"event_id", e.event_id}, {"narrative_id", e.narrative_id},
             {"summary", e.summary}, {"meta", e.meta}};
}
void from_json(const json& j, Event& e) {
    e.event_id = j.at("event_id").get<std::string>();
    e.narrative_id = j.at("narrative_id").get<std::string>();
    e.summary = j.value("summary", "");
    e.meta = j.value("meta", json::object());
}

void to_json(json& j, const Profile& p) {
    j = json{{"profile_id", p.profile_id}, {"event_id", p.event_id},
             {"perspective", p.perspective}, {"time", p.time},
             {"place", p.place}, {"subject", p.subject}, {"verb", p.verb},
             {"object", p.object}, {"modifiers", p.modifiers},
             {"graph", p.graph}, {"payload", p.payload},
             {"seq", p.seq}};
}
void from_json(const json& j, Profile& p) {
    p.profile_id = j.at("profile_id").get<std::string>();
    p.event_id = j.at("event_id").get<std::string>();
    p.perspective = j.value("perspective", "");
    p.time = j.at("time").get<TimeRef>();
    p.place = j.value("place", "");
    p.subject = j.value("subject", "");
    p.verb = j.value("verb", "");
    p.object = j.value("object", "");
    p.modifiers = j.value("modifiers", std::vector<Modifier>{});
    p.graph = j.value("graph", json::object());
    p.payload = j.value("payload", json::object());
    p.seq = j.value("seq", (int64_t)0);
}

void to_json(json& j, const Link& l) {
    j = json{{"from_profile_id", l.from_profile_id},
             {"to_profile_id", l.to_profile_id}, {"relation", l.relation}};
}
void from_json(const json& j, Link& l) {
    l.from_profile_id = j.at("from_profile_id").get<std::string>();
    l.to_profile_id = j.at("to_profile_id").get<std::string>();
    l.relation = j.at("relation").get<std::string>();
}

void to_json(json& j, const Entity& e) {
    j = json{{"entity_id", e.entity_id}, {"name", e.name}, {"type", e.type},
             {"aliases", e.aliases}, {"attributes", e.attributes}};
}
void from_json(const json& j, Entity& e) {
    e.entity_id = j.at("entity_id").get<std::string>();
    e.name = j.at("name").get<std::string>();
    e.type = j.value("type", "");
    e.aliases = j.value("aliases", json::array());
    e.attributes = j.value("attributes", json::object());
}

void to_json(json& j, const Binding& b) {
    j = json{{"binding_id", b.binding_id}, {"profile_id", b.profile_id},
             {"slot", b.slot}, {"entity_id", b.entity_id},
             {"confidence", b.confidence}, {"note", b.note}, {"seq", b.seq}};
}
void from_json(const json& j, Binding& b) {
    b.binding_id = j.at("binding_id").get<std::string>();
    b.profile_id = j.at("profile_id").get<std::string>();
    b.slot = j.at("slot").get<std::string>();
    b.entity_id = j.at("entity_id").get<std::string>();
    b.confidence = j.value("confidence", 1.0);
    b.note = j.value("note", "");
    b.seq = j.value("seq", (int64_t)0);
}

} // namespace eventstore
