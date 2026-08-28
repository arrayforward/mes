#include "entitytree/model.h"

#include <atomic>
#include <cstdio>
#include <random>

namespace entitytree {

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

// ---- 序列化 ----

void to_json(json& j, const Observation& o) {
    j = json{{"observation_id", o.observation_id}, {"attribute_key", o.attribute_key},
             {"value", o.value}, {"confidence", o.confidence},
             {"source_id", o.source_id}, {"anchor_ref", o.anchor_ref},
             {"timestamp", o.timestamp}, {"event_ref", o.event_ref},
             {"seq", o.seq}};
}
void from_json(const json& j, Observation& o) {
    o.observation_id = j.at("observation_id").get<std::string>();
    o.attribute_key = j.at("attribute_key").get<std::string>();
    o.value = j.at("value").get<std::string>();
    o.confidence = j.value("confidence", 1.0);
    o.source_id = j.value("source_id", "");
    o.anchor_ref = j.value("anchor_ref", "");
    o.timestamp = j.value("timestamp", (int64_t)0);
    o.event_ref = j.value("event_ref", "");
    o.seq = j.value("seq", (int64_t)0);
}

void to_json(json& j, const AttrProfile& p) {
    j = json{{"profile_id", p.profile_id}, {"anchor_ref", p.anchor_ref},
             {"time_bucket", p.time_bucket}, {"level", p.level},
             {"attributes", p.attributes},
             {"info_score", p.info_score}, {"status", p.status},
             {"seq", p.seq}};
}
void from_json(const json& j, AttrProfile& p) {
    p.profile_id = j.at("profile_id").get<std::string>();
    p.anchor_ref = j.value("anchor_ref", "");
    p.time_bucket = j.value("time_bucket", (int64_t)0);
    p.level = j.value("level", 0);
    p.attributes = j.value("attributes", json::object());
    p.info_score = j.value("info_score", 0.0);
    p.status = j.value("status", "pool");
    p.seq = j.value("seq", (int64_t)0);
}

void to_json(json& j, const EntityNode& e) {
    j = json{{"entity_id", e.entity_id}, {"name", e.name}, {"type", e.type},
             {"attributes", e.attributes}, {"view_version", e.view_version},
             {"credibility", e.credibility}, {"status", e.status},
             {"updated_at", e.updated_at}};
}
void from_json(const json& j, EntityNode& e) {
    e.entity_id = j.at("entity_id").get<std::string>();
    e.name = j.value("name", "");
    e.type = j.value("type", "");
    e.attributes = j.value("attributes", json::object());
    e.view_version = j.value("view_version", (int64_t)0);
    e.credibility = j.value("credibility", 0.0);
    e.status = j.value("status", "candidate");
    e.updated_at = j.value("updated_at", (int64_t)0);
}

void to_json(json& j, const EntityBinding& b) {
    j = json{{"binding_id", b.binding_id}, {"profile_id", b.profile_id},
             {"entity_id", b.entity_id}, {"confidence", b.confidence},
             {"kind", b.kind}, {"note", b.note}, {"seq", b.seq}};
}
void from_json(const json& j, EntityBinding& b) {
    b.binding_id = j.at("binding_id").get<std::string>();
    b.profile_id = j.at("profile_id").get<std::string>();
    b.entity_id = j.at("entity_id").get<std::string>();
    b.confidence = j.value("confidence", 1.0);
    b.kind = j.value("kind", "");
    b.note = j.value("note", "");
    b.seq = j.value("seq", (int64_t)0);
}

void to_json(json& j, const SourceRecord& s) {
    j = json{{"source_id", s.source_id}, {"reliability", s.reliability},
             {"updated_at", s.updated_at}, {"meta", s.meta}};
}
void from_json(const json& j, SourceRecord& s) {
    s.source_id = j.at("source_id").get<std::string>();
    s.reliability = j.value("reliability", 1.0);
    s.updated_at = j.value("updated_at", (int64_t)0);
    s.meta = j.value("meta", json::object());
}

} // namespace entitytree
