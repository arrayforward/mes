// ============================================================================
// mse/api.cpp —— API 门面实现:两个动词 + 扁平载荷归一化
// ============================================================================

#include "mse/api.h"

#include <set>

#include "mse/dictionary.h"
#include "mse/pipeline.h"
#include "mse/view_engine.h"

namespace mse {
namespace {

// 归一化时的顶层系统键(其余键 = 属性写入)
const std::set<std::string>& system_keys() {
    static const std::set<std::string> keys = {"type",   "id",       "actor",
                                               "space",  "time",     "evidence",
                                               "corrects", "idempotency_key", "writes"};
    return keys;
}

// Receipt → 回执 JSON(POST /events 与定义入口同形态)
json receipt_json(const Receipt& r) {
    if (r.status == Receipt::Status::kSettled)
        return json{{"status", "settled"}, {"event_id", r.event_id.value_or(0)}};
    if (r.status == Receipt::Status::kAccepted)
        return json{{"status", "accepted"}, {"queue_seq", r.queue_seq.value_or(0)}};
    return json{{"status", "rejected"}, {"layer", r.layer}, {"violations", r.violations}};
}

} // namespace

ApiGateway::ApiGateway(WritePipeline& pipeline, ViewEngine& views, DefinitionLayer& defs)
    : pipeline_(pipeline), views_(views), defs_(defs) {}

std::optional<Candidate> ApiGateway::normalize(const json& payload, std::string& error) {
    if (!payload.is_object()) {
        error = "载荷必须是 JSON object";
        return std::nullopt;
    }
    if (!payload.contains("type") || !payload["type"].is_string() ||
        payload["type"].get<std::string>().empty()) {
        error = "缺少系统键: type";
        return std::nullopt;
    }

    Candidate c;
    c.type = payload["type"].get<std::string>();

    // ---- 目标与变化集:{"writes": {...}} 显式形态优先,否则扁平键应用到每个 id ----
    if (payload.contains("writes") && payload["writes"].is_object()) {
        for (auto it = payload["writes"].begin(); it != payload["writes"].end(); ++it) {
            if (!it.value().is_object()) {
                error = "writes 的每个目标必须是 object: " + it.key();
                return std::nullopt;
            }
            c.writes[it.key()] = it.value();
        }
        // id 可省;给了则须与 writes 目标一致
        if (payload.contains("id")) {
            const json& idj = payload["id"];
            if (idj.is_string()) {
                if (!c.writes.contains(idj.get<std::string>())) {
                    error = "id 与 writes 目标不一致";
                    return std::nullopt;
                }
            } else if (idj.is_array()) {
                if (idj.size() != c.writes.size()) {
                    error = "id 与 writes 目标不一致";
                    return std::nullopt;
                }
                for (const json& e : idj) {
                    if (!e.is_string() || !c.writes.contains(e.get<std::string>())) {
                        error = "id 与 writes 目标不一致";
                        return std::nullopt;
                    }
                }
            } else {
                error = "id 必须是字符串或字符串数组";
                return std::nullopt;
            }
        }
    } else {
        std::vector<std::string> ids;
        if (!payload.contains("id")) {
            error = "缺少系统键: id";
            return std::nullopt;
        }
        const json& idj = payload["id"];
        if (idj.is_string()) {
            ids.push_back(idj.get<std::string>());
        } else if (idj.is_array()) {
            for (const json& e : idj) {
                if (!e.is_string()) {
                    error = "id 数组元素必须是字符串";
                    return std::nullopt;
                }
                ids.push_back(e.get<std::string>());
            }
            if (ids.empty()) {
                error = "id 数组为空";
                return std::nullopt;
            }
        } else {
            error = "id 必须是字符串或字符串数组";
            return std::nullopt;
        }
        // 其余非系统键组成每个目标的变化集
        std::map<std::string, json> changes;
        for (auto it = payload.begin(); it != payload.end(); ++it)
            if (!system_keys().count(it.key())) changes[it.key()] = it.value();
        for (const std::string& id : ids) c.writes[id] = changes;
    }

    // ---- 其余系统键 ----
    if (payload.contains("actor") && payload["actor"].is_string())
        c.actor = payload["actor"].get<std::string>();

    if (payload.contains("space")) {
        const json& s = payload["space"];
        if (s.is_string()) {
            // 字符串原样存:短串按锚点路径;含空格/明显长句按模糊文本
            const std::string str = s.get<std::string>();
            if (str.find(' ') != std::string::npos || str.size() > 32)
                c.space = SpaceRef::fuzzy(str);
            else
                c.space = SpaceRef::anchor_ref(str);
        } else if (s.is_object()) {
            if (s.contains("anchor") && s["anchor"].is_string())
                c.space = SpaceRef::anchor_ref(s["anchor"].get<std::string>());
            else if (s.contains("coord"))
                c.space = SpaceRef::coordinate(s["coord"]);
            else if (s.contains("raw") && s["raw"].is_string())
                c.space = SpaceRef::fuzzy(s["raw"].get<std::string>());
            else
                c.space = SpaceRef::anchor_ref("");
        } else {
            c.space = SpaceRef::anchor_ref("");
        }
    } else {
        c.space = SpaceRef::anchor_ref("");
    }

    if (payload.contains("time") && payload["time"].is_string())
        c.occur_time = payload["time"].get<std::string>();
    if (payload.contains("evidence")) c.evidence = payload["evidence"];
    if (payload.contains("corrects") && payload["corrects"].is_number_integer())
        c.corrects = payload["corrects"].get<int64_t>();
    if (payload.contains("idempotency_key") && payload["idempotency_key"].is_string())
        c.idempotency_key = payload["idempotency_key"].get<std::string>();

    return c;
}

json ApiGateway::post_events(const json& payload, int trust) {
    std::string error;
    std::optional<Candidate> c = normalize(payload, error);
    if (!c) {
        // 载荷形状非法:第 -1 层(未进管线)
        return json{{"status", "rejected"}, {"layer", -1}, {"violations", {error}}};
    }
    c->trust = trust;  // 入口信任级注入(凭证元数据,不进事件)
    return receipt_json(pipeline_.submit(*c));
}

void ApiGateway::set_trust_tokens(std::map<std::string, int> tokens) {
    trust_tokens_ = std::move(tokens);
}

int ApiGateway::trust_of(const std::string& token) const {
    const auto it = trust_tokens_.find(token);
    return it != trust_tokens_.end() ? it->second : 0;
}

json ApiGateway::get_view(const std::string& view_id,
                          const std::map<std::string, std::string>& query) {
    ViewParams params;
    if (auto it = query.find("observer"); it != query.end()) params.observer = it->second;
    if (auto it = query.find("entity"); it != query.end()) params.entity = it->second;
    if (auto it = query.find("t"); it != query.end()) {
        try {
            params.as_of_seq = std::stoll(it->second);  // t = AS OF 事件序号
        } catch (...) {
            // 非数字的 t 忽略(按全量渲染)
        }
    }
    return views_.render(view_id, params);
}

json ApiGateway::post_definition(const std::string& def_type, const json& payload) {
    return receipt_json(defs_.settle_definition(def_type, payload));
}

} // namespace mse
