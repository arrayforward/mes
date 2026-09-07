// ============================================================================
// mse/model.cpp —— 核心数据形态序列化与静态工厂(存在层)
//
// 纪律:
//   to_json 字段序固定(nlohmann::json 默认按字典序存放对象键,dump 结果
//   逐比特确定);from_json 宽容读取——缺字段取默认值,类型不符不抛(能取
//   则取),保证旧版本日志可被新代码重放。
// ============================================================================

#include "mse/model.h"

namespace mse {

// ---- 宽容读取小工具:缺键/类型不符时返回默认值,绝不抛异常 ----
namespace {

std::string get_string(const json& j, const char* key, std::string def = "") {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

int64_t get_int(const json& j, const char* key, int64_t def = 0) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<int64_t>();
}

} // namespace

// ----------------------------------------------------------------------------
// SpaceRef 静态工厂
// ----------------------------------------------------------------------------

SpaceRef SpaceRef::anchor_ref(std::string path, int precision) {
    SpaceRef s;
    s.kind = Kind::kAnchor;
    s.anchor = std::move(path);
    s.precision = precision;
    return s;
}

SpaceRef SpaceRef::coordinate(json xyz) {
    SpaceRef s;
    s.kind = Kind::kCoord;
    s.coord = std::move(xyz);
    s.precision = 0;
    return s;
}

SpaceRef SpaceRef::fuzzy(std::string raw) {
    SpaceRef s;
    s.kind = Kind::kRawText;
    s.raw_text = std::move(raw);
    s.precision = 2;  // 模糊文本精度恒为 2
    return s;
}

// ----------------------------------------------------------------------------
// Receipt 静态工厂
// ----------------------------------------------------------------------------

Receipt Receipt::settled(int64_t event_id) {
    Receipt r;
    r.status = Status::kSettled;
    r.event_id = event_id;
    r.layer = -1;
    return r;
}

Receipt Receipt::rejected(int layer, std::vector<std::string> violations) {
    Receipt r;
    r.status = Status::kRejected;
    r.layer = layer;
    r.violations = std::move(violations);
    return r;
}

Receipt Receipt::accepted(int64_t queue_seq) {
    Receipt r;
    r.status = Status::kAccepted;
    r.queue_seq = queue_seq;
    r.layer = -1;
    return r;
}

// ----------------------------------------------------------------------------
// SpaceRef 序列化
//   形态:{"kind":"anchor"|"coord"|"raw_text","anchor":..,"coord":..,
//         "raw_text":..,"precision":n}
// ----------------------------------------------------------------------------

void to_json(json& j, const SpaceRef& s) {
    const char* kind = "anchor";
    switch (s.kind) {
        case SpaceRef::Kind::kAnchor:  kind = "anchor";   break;
        case SpaceRef::Kind::kCoord:   kind = "coord";    break;
        case SpaceRef::Kind::kRawText: kind = "raw_text"; break;
    }
    j = json{
        {"kind", kind},
        {"anchor", s.anchor},
        {"coord", s.coord},
        {"raw_text", s.raw_text},
        {"precision", s.precision},
    };
}

void from_json(const json& j, SpaceRef& s) {
    const std::string kind = get_string(j, "kind", "anchor");
    if (kind == "coord")         s.kind = SpaceRef::Kind::kCoord;
    else if (kind == "raw_text") s.kind = SpaceRef::Kind::kRawText;
    else                         s.kind = SpaceRef::Kind::kAnchor;
    s.anchor = get_string(j, "anchor");
    auto cit = j.find("coord");
    s.coord = (cit != j.end()) ? *cit : json();
    s.raw_text = get_string(j, "raw_text");
    s.precision = static_cast<int>(get_int(j, "precision", 0));
}

// ----------------------------------------------------------------------------
// ChangeSet 序列化
// ----------------------------------------------------------------------------

void to_json(json& j, const ChangeSet& c) {
    j = json{
        {"type", c.type},
        {"writes", c.writes},
        {"actor", c.actor},
        {"space", c.space},
        {"occur_time", c.occur_time},
        {"evidence", c.evidence ? *c.evidence : json()},
        {"corrects", c.corrects ? json(*c.corrects) : json()},
        {"idempotency_key", c.idempotency_key ? json(*c.idempotency_key) : json()},
    };
}

void from_json(const json& j, ChangeSet& c) {
    c.type = get_string(j, "type");
    c.writes.clear();
    auto wit = j.find("writes");
    if (wit != j.end() && wit->is_object()) {
        for (auto it = wit->begin(); it != wit->end(); ++it) {
            if (it.value().is_object()) c.writes[it.key()] = it.value();
        }
    }
    c.actor = get_string(j, "actor");
    auto sit = j.find("space");
    if (sit != j.end() && sit->is_object()) from_json(*sit, c.space);
    else c.space = SpaceRef{};
    c.occur_time = get_string(j, "occur_time");
    c.evidence.reset();
    auto eit = j.find("evidence");
    if (eit != j.end() && !eit->is_null()) c.evidence = *eit;
    c.corrects.reset();
    auto cit = j.find("corrects");
    if (cit != j.end() && cit->is_number()) c.corrects = cit->get<int64_t>();
    c.idempotency_key.reset();
    auto iit = j.find("idempotency_key");
    if (iit != j.end() && iit->is_string()) c.idempotency_key = iit->get<std::string>();
}

// ----------------------------------------------------------------------------
// Event 序列化:ChangeSet 全部字段 + 结算元数据
// ----------------------------------------------------------------------------

void to_json(json& j, const Event& e) {
    to_json(j, static_cast<const ChangeSet&>(e));
    j["event_id"] = e.event_id;
    j["settle_seq"] = e.settle_seq;
    j["settle_time"] = e.settle_time;
    j["def_versions"] = e.def_versions;
}

void from_json(const json& j, Event& e) {
    from_json(j, static_cast<ChangeSet&>(e));
    e.event_id = get_int(j, "event_id", 0);
    e.settle_seq = get_int(j, "settle_seq", 0);
    e.settle_time = get_string(j, "settle_time");
    auto dit = j.find("def_versions");
    e.def_versions = (dit != j.end() && !dit->is_null()) ? *dit : json();
}

// ----------------------------------------------------------------------------
// Receipt 序列化
// ----------------------------------------------------------------------------

void to_json(json& j, const Receipt& r) {
    const char* status = "rejected";
    switch (r.status) {
        case Receipt::Status::kSettled:  status = "settled";  break;
        case Receipt::Status::kAccepted: status = "accepted"; break;
        case Receipt::Status::kRejected: status = "rejected"; break;
    }
    j = json{
        {"status", status},
        {"event_id", r.event_id ? json(*r.event_id) : json()},
        {"queue_seq", r.queue_seq ? json(*r.queue_seq) : json()},
        {"layer", r.layer},
        {"violations", r.violations},
    };
}

void from_json(const json& j, Receipt& r) {
    const std::string status = get_string(j, "status", "rejected");
    r.status = status == "settled"  ? Receipt::Status::kSettled
               : status == "accepted" ? Receipt::Status::kAccepted
                                      : Receipt::Status::kRejected;
    r.event_id.reset();
    auto eit = j.find("event_id");
    if (eit != j.end() && eit->is_number()) r.event_id = eit->get<int64_t>();
    r.queue_seq.reset();
    auto qit = j.find("queue_seq");
    if (qit != j.end() && qit->is_number()) r.queue_seq = qit->get<int64_t>();
    r.layer = static_cast<int>(get_int(j, "layer", -1));
    r.violations.clear();
    auto vit = j.find("violations");
    if (vit != j.end() && vit->is_array())
        r.violations = vit->get<std::vector<std::string>>();
}

} // namespace mse
