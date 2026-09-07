// ============================================================================
// mse/dictionary.cpp —— 定义层:四个数据集合 + 锚点表(实现)
//
// 四集合是定义事件流(表 mse_def_events)的物化投影:
//   启动 load() 重放全部定义事件 fold 出集合;运行中 settle_definition()
//   走候选-结算(schema + 引用完整性校验),通过后 append 定义事件并热更新。
//   load 重放与 settle 共用同一份 fold_definition —— 投影纪律。
// ============================================================================

#include <functional>
#include <stdexcept>

#include "mse/dictionary.h"
#include "mse/wasm_sandbox.h"

#include <algorithm>
#include <set>
#include <utility>

#include <storage/record_backend.h>

namespace mse {

// ---- DefVersions JSON 序列化 ----
void to_json(json& j, const DefVersions& v) {
    j = json{
        {"dict",    v.dict},
        {"types",   v.types},
        {"rules",   v.rules},
        {"views",   v.views},
        {"anchors", v.anchors},
    };
}
void from_json(const json& j, DefVersions& v) {
    v.dict    = j.value("dict", int64_t{0});
    v.types   = j.value("types", int64_t{0});
    v.rules   = j.value("rules", int64_t{0});
    v.views   = j.value("views", int64_t{0});
    v.anchors = j.value("anchors", int64_t{0});
}

// ---- AttributeEntry ----
void to_json(json& j, const AttributeEntry& e) {
    j = json{
        {"key",           e.key},
        {"semantic",      e.semantic},
        {"datatype",      e.datatype},
        {"unit",          e.unit},
        {"range",         e.range},
        {"writers",       e.writers},
        {"kind",          e.kind},
        {"rule_ref",      e.rule_ref},
        {"status",        e.status},
        {"replaced_by",   e.replaced_by},
        {"version",       e.version},
        {"registered_by", e.registered_by},
    };
}
void from_json(const json& j, AttributeEntry& e) {
    j.at("key").get_to(e.key);
    j.at("semantic").get_to(e.semantic);
    j.at("datatype").get_to(e.datatype);
    j.at("unit").get_to(e.unit);
    e.range       = j.value("range", json::array());
    j.at("writers").get_to(e.writers);
    j.at("kind").get_to(e.kind);
    e.rule_ref      = j.value("rule_ref", std::string{});
    e.status        = j.value("status", std::string{"active"});
    e.replaced_by   = j.value("replaced_by", std::string{});
    e.version       = j.value("version", int64_t{1});
    e.registered_by = j.value("registered_by", int64_t{0});
}

// ---- EventTypeEntry ----
void to_json(json& j, const EventTypeEntry& e) {
    j = json{
        {"type",          e.type},
        {"required_keys", e.required_keys},
        {"optional_keys", e.optional_keys},
        {"rules",         e.rules},
        {"correction",    e.correction},
        {"multi_target",  e.multi_target},
        {"settlement",    e.settlement},
        {"min_trust",     e.min_trust},
        {"presets",       e.presets},
        {"status",        e.status},
        {"version",       e.version},
        {"registered_by", e.registered_by},
    };
}
void from_json(const json& j, EventTypeEntry& e) {
    j.at("type").get_to(e.type);
    j.at("required_keys").get_to(e.required_keys);
    j.at("optional_keys").get_to(e.optional_keys);
    j.at("rules").get_to(e.rules);
    e.correction    = j.value("correction", std::string{});
    e.multi_target  = j.value("multi_target", false);
    e.settlement    = j.value("settlement", std::string{"sync"});
    e.min_trust     = j.value("min_trust", 0);
    e.presets       = j.value("presets", json::object());
    e.status        = j.value("status", std::string{"active"});
    e.version       = j.value("version", int64_t{1});
    e.registered_by = j.value("registered_by", int64_t{0});
}

// ---- ViewEntry::Variant ----
void to_json(json& j, const ViewEntry::Variant& v) {
    j = json{
        {"columns", v.columns},
        {"emits",   v.emits},
    };
}
void from_json(const json& j, ViewEntry::Variant& v) {
    v.columns = j.value("columns", std::vector<std::string>{});
    v.emits   = j.value("emits", std::vector<std::string>{});
}

// ---- ViewEntry ----
void to_json(json& j, const ViewEntry& e) {
    j = json{
        {"view_id",     e.view_id},
        {"queries",     {{"events", e.queries.events},
                         {"fold", e.queries.fold},
                         {"spacetime", e.queries.spacetime}}},
        {"selects",     e.selects},
        {"rules",       e.rules},
        {"emits",       e.emits},
        {"render_mode", e.render_mode},
        {"variants",    e.variants},
        {"status",      e.status},
        {"version",     e.version},
        {"registered_by", e.registered_by},
    };
}
void from_json(const json& j, ViewEntry& e) {
    j.at("view_id").get_to(e.view_id);
    const json& q = j.at("queries");
    q.at("events").get_to(e.queries.events);
    e.queries.fold      = q.value("fold", std::string{"L1"});
    e.queries.spacetime = q.value("spacetime", std::string{});
    j.at("selects").get_to(e.selects);
    j.at("rules").get_to(e.rules);
    j.at("emits").get_to(e.emits);
    e.render_mode = j.value("render_mode", std::string{"终态"});
    e.variants.clear();
    if (j.contains("variants") && j.at("variants").is_object()) {
        for (const auto& [role, vj] : j.at("variants").items()) {
            ViewEntry::Variant v;
            from_json(vj, v);
            e.variants.emplace(role, std::move(v));
        }
    }
    e.status        = j.value("status", std::string{"active"});
    e.version       = j.value("version", int64_t{1});
    e.registered_by = j.value("registered_by", int64_t{0});
}

// ---- AnchorEntry ----
void to_json(json& j, const AnchorEntry& e) {
    j = json{
        {"path",          e.path},
        {"name",          e.name},
        {"meta",          e.meta},
        {"registered_by", e.registered_by},
    };
}
void from_json(const json& j, AnchorEntry& e) {
    j.at("path").get_to(e.path);
    e.name          = j.value("name", std::string{});
    e.meta          = j.value("meta", json::object());
    e.registered_by = j.value("registered_by", int64_t{0});
}

namespace {

constexpr const char* kDefTable = "mse_def_events";

// 定义事件类型全集
bool is_def_type(const std::string& t) {
    static const std::set<std::string> kTypes = {
        "AttributeRegistered", "AttributeDeprecated", "EventTypeRegistered",
        "RuleRegistered", "ViewRegistered", "AnchorRegistered",
    };
    return kTypes.count(t) != 0;
}

// ---- payload 形状校验辅助(违规描述进 violations) ----
bool need_str(const json& p, const char* k, std::vector<std::string>& v) {
    if (!p.contains(k) || !p.at(k).is_string()) {
        v.push_back(std::string("字段 ") + k + " 缺失或须为字符串");
        return false;
    }
    return true;
}
bool need_str_array(const json& p, const char* k, std::vector<std::string>& v) {
    if (!p.contains(k) || !p.at(k).is_array()) {
        v.push_back(std::string("字段 ") + k + " 缺失或须为数组");
        return false;
    }
    for (const auto& e : p.at(k)) {
        if (!e.is_string()) {
            v.push_back(std::string("字段 ") + k + " 的元素须全为字符串");
            return false;
        }
    }
    return true;
}
std::vector<std::string> str_list(const json& p, const char* k) {
    std::vector<std::string> out;
    for (const auto& e : p.at(k)) out.push_back(e.get<std::string>());
    return out;
}

} // namespace

// ----------------------------------------------------------------------------
// 构造:建表(幂等)
// ----------------------------------------------------------------------------
DefinitionLayer::DefinitionLayer(storage::RecordBackend& backend) : backend_(backend) {
    storage::TableSchema schema;
    schema.name = kDefTable;
    schema.fields = {
        {"seq",      storage::FieldType::kInt},
        {"def_type", storage::FieldType::kText},
        {"payload",  storage::FieldType::kText},
    };
    schema.pk = "seq";
    schema.auto_seq = true;  // append-only 自增主键表
    backend_.create_table(schema);
}

// ----------------------------------------------------------------------------
// load:重放定义事件流,fold 出四集合(重放不做校验——结算时已校验过)
// ----------------------------------------------------------------------------
void DefinitionLayer::load() {
    const auto rows = backend_.query(kDefTable, {}, {{"seq", false}});
    for (const auto& row : rows) {
        const int64_t     seq      = storage::as_int(row.at("seq"));
        const std::string def_type = storage::as_text(row.at("def_type"));
        const json        payload  = json::parse(storage::as_text(row.at("payload")));
        fold_definition(seq, def_type, payload);
        def_event_count_ = std::max(def_event_count_, seq);
    }
}

// ----------------------------------------------------------------------------
// settle_definition:定义候选-结算
// ----------------------------------------------------------------------------
Receipt DefinitionLayer::settle_definition(const std::string& def_type, const json& payload) {
    // ① def_type 合法
    if (!is_def_type(def_type)) {
        return Receipt::rejected(2, {"未知定义事件类型: " + def_type});
    }
    if (!payload.is_object()) {
        return Receipt::rejected(2, {"payload 须为 object"});
    }

    // ② payload 形状校验 + ③ 引用完整性强校验(未登记 = 编译不过)
    std::vector<std::string> violations;
    json enriched;  // 烘焙管线产物(版本钉死字段)非空时,以其为最终定义负载

    if (def_type == "AttributeRegistered") {
        need_str(payload, "key", violations);
        need_str(payload, "semantic", violations);
        need_str(payload, "datatype", violations);
        need_str(payload, "unit", violations);
        if (!payload.contains("range") || !payload.at("range").is_array()) {
            violations.push_back("字段 range 缺失或须为数组");
        }
        need_str_array(payload, "writers", violations);
        need_str(payload, "kind", violations);
        if (violations.empty()) {
            static const std::set<std::string> kDatatypes = {
                "string", "number", "integer", "boolean", "enum", "list", "object", "ref",
            };
            if (kDatatypes.count(payload.at("datatype").get<std::string>()) == 0) {
                violations.push_back("datatype 非法: " + payload.at("datatype").get<std::string>());
            }
            const std::string kind = payload.at("kind").get<std::string>();
            if (kind != "原生" && kind != "派生") {
                violations.push_back("kind 非法(须为 原生/派生): " + kind);
            }
            // 派生属性:rule_ref 非空(规则可后注册,只查非空)
            if (kind == "派生") {
                if (!payload.contains("rule_ref") || !payload.at("rule_ref").is_string() ||
                    payload.at("rule_ref").get<std::string>().empty()) {
                    violations.push_back("派生属性须携带非空 rule_ref");
                }
            }
            if (payload.at("writers").empty() && kind == "原生") {
                // 原生属性须有事件写授权;派生属性由推导规则写回投影,不接收事件直写
                violations.push_back("writers 须非空(写授权)");
            }
            if (payload.at("key").get<std::string>().empty()) {
                violations.push_back("key 不能为空");
            }
        }
    } else if (def_type == "AttributeDeprecated") {
        if (need_str(payload, "key", violations)) {
            const std::string key = payload.at("key").get<std::string>();
            if (dict_.count(key) == 0) {
                violations.push_back("属性未登记,无法废弃: " + key);
            }
        }
        if (payload.contains("replaced_by") && !payload.at("replaced_by").is_string()) {
            violations.push_back("字段 replaced_by 须为字符串");
        }
    } else if (def_type == "EventTypeRegistered") {
        need_str(payload, "type", violations);
        need_str_array(payload, "required_keys", violations);
        need_str_array(payload, "optional_keys", violations);
        need_str_array(payload, "rules", violations);
        if (payload.contains("correction") && !payload.at("correction").is_string()) {
            violations.push_back("字段 correction 须为字符串");
        }
        if (payload.contains("multi_target") && !payload.at("multi_target").is_boolean()) {
            violations.push_back("字段 multi_target 须为布尔");
        }
        if (payload.contains("settlement") &&
            (!payload.at("settlement").is_string() ||
             (payload.at("settlement") != "sync" && payload.at("settlement") != "async"))) {
            violations.push_back("settlement 非法(须为 sync/async)");
        }
        if (payload.contains("min_trust") &&
            (!payload.at("min_trust").is_number_integer() ||
             payload.at("min_trust").get<int>() < 0)) {
            violations.push_back("min_trust 非法(须为非负整数)");
        }
        if (payload.contains("presets") && !payload.at("presets").is_object()) {
            violations.push_back("字段 presets 须为 object");
        }
        if (violations.empty()) {
            // required/optional 键必须已在字典登记(未登记 = 编译不过)
            for (const char* f : {"required_keys", "optional_keys"}) {
                for (const auto& k : str_list(payload, f)) {
                    if (dict_.count(k) == 0) {
                        violations.push_back(std::string("事件类型引用未登记键: ") + k);
                    }
                }
            }
            for (const auto& r : str_list(payload, "rules")) {
                if (rules_.count(r) == 0) {
                    violations.push_back("事件类型引用未注册规则: " + r);
                }
            }
            // correction 非空时必须是已注册类型
            const std::string corr = payload.value("correction", std::string{});
            if (!corr.empty() && types_.count(corr) == 0) {
                violations.push_back("修正类型未注册: " + corr);
            }
            // presets(固定写入值):键须已登记、在该键值域内(range 非空时)、
            // 且出现在 required/optional_keys 清单里(预置的是表单字段,清单外即违规)
            if (payload.contains("presets")) {
                const json& presets = payload.at("presets");
                std::set<std::string> declared;
                for (const char* f : {"required_keys", "optional_keys"})
                    for (const auto& k : str_list(payload, f)) declared.insert(k);
                for (const auto& [k, v] : presets.items()) {
                    auto ait = dict_.find(k);
                    if (ait == dict_.end()) {
                        violations.push_back("presets 引用未登记键: " + k);
                        continue;
                    }
                    if (declared.count(k) == 0) {
                        violations.push_back(
                            "presets 键未列入 required/optional_keys: " + k);
                    }
                    const auto& range = ait->second.range;
                    if (!range.empty() &&
                        std::find(range.begin(), range.end(), v) == range.end()) {
                        violations.push_back("presets 值超出键值域: " + k);
                    }
                }
            }
            if (payload.at("type").get<std::string>().empty()) {
                violations.push_back("type 不能为空");
            }
            // 重复注册视为新版本(append-only 定义演化),不视为违规
        }
    } else if (def_type == "RuleRegistered") {
        need_str(payload, "rule_id", violations);
        need_str_array(payload, "deps", violations);
        need_str(payload, "effect", violations);
        const std::string runtime = payload.value("runtime", std::string{"jsonlogic"});
        if (runtime != "jsonlogic" && runtime != "wasm") {
            violations.push_back("runtime 非法(须为 jsonlogic/wasm): " + runtime);
        }
        if (runtime == "wasm") {
            // WASM 规则:artifact(base64 产物)必填;logic 不需要
            if (!payload.contains("artifact") || !payload.at("artifact").is_string() ||
                payload.at("artifact").get<std::string>().empty()) {
                violations.push_back("wasm 规则缺少 artifact(base64 产物)");
            }
        } else if (!payload.contains("logic")) {
            violations.push_back("字段 logic 缺失");
        }
        if (payload.contains("target_key") && !payload.at("target_key").is_string()) {
            violations.push_back("字段 target_key 须为字符串");
        }
        if (payload.contains("on_types") && !payload.at("on_types").is_array()) {
            violations.push_back("字段 on_types 须为字符串数组");
        }
        if (payload.contains("consumers") &&
            (!payload.at("consumers").is_string() ||
             (payload.at("consumers") != "write" && payload.at("consumers") != "read" &&
              payload.at("consumers") != "both"))) {
            violations.push_back("consumers 非法(须为 write/read/both)");
        }
        if (violations.empty()) {
            Rule r;
            from_json(payload, r);
            if (r.rule_id.empty()) violations.push_back("rule_id 不能为空");
            if (r.runtime == "wasm") {
                // 烘焙管线(编译是定义结算的一部分):解码 → 静态检查 → 沙盒试跑
                // → 记录产物哈希与引擎版本。烘焙失败 = 定义候选被拒。
                auto baked = WasmSandbox::bake(
                    r.artifact, r.deps, r.export_name,
                    [this](const std::string& k) { return this->is_registered_key(k); });
                if (!baked.ok) {
                    violations.insert(violations.end(), baked.errors.begin(), baked.errors.end());
                } else {
                    // 版本钉死:产物哈希 + 沙盒引擎版本写进定义事件负载
                    enriched               = payload;
                    enriched["artifact_hash"]  = baked.artifact_hash;
                    enriched["engine_version"] = baked.engine_version;
                }
            } else {
                // 静态检查:deps 已登记 / var 只引用 deps / logic 形状 / derive target_key
                auto checked = RuleEngine::static_check(
                    r, [this](const std::string& k) { return this->is_registered_key(k); });
                violations.insert(violations.end(), checked.begin(), checked.end());
            }
        }
    } else if (def_type == "ViewRegistered") {
        need_str(payload, "view_id", violations);
        need_str_array(payload, "selects", violations);
        need_str_array(payload, "rules", violations);
        need_str_array(payload, "emits", violations);
        need_str(payload, "render_mode", violations);
        bool queries_ok = true;
        if (!payload.contains("queries") || !payload.at("queries").is_object()) {
            violations.push_back("字段 queries 缺失或须为 object");
            queries_ok = false;
        } else {
            queries_ok = need_str_array(payload.at("queries"), "events", violations);
            const json& q = payload.at("queries");
            if (q.contains("fold") &&
                (!q.at("fold").is_string() || (q.at("fold") != "L1" && q.at("fold") != "L1+L2"))) {
                violations.push_back("fold 非法(须为 L1/L1+L2)");
            }
            if (q.contains("spacetime") && !q.at("spacetime").is_string()) {
                violations.push_back("字段 spacetime 须为字符串(锚点路径)");
            }
        }
        if (payload.contains("variants") && !payload.at("variants").is_object()) {
            violations.push_back("字段 variants 须为 object");
        }
        if (violations.empty()) {
            static const std::set<std::string> kRenderModes = {"终态", "流水", "拦截", "遍历"};
            if (kRenderModes.count(payload.at("render_mode").get<std::string>()) == 0) {
                violations.push_back("render_mode 非法(须为 终态/流水/拦截/遍历): " +
                                     payload.at("render_mode").get<std::string>());
            }
            for (const auto& k : str_list(payload, "selects")) {
                if (dict_.count(k) == 0) {
                    violations.push_back("视图 selects 引用未登记键: " + k);
                }
            }
            for (const auto& r : str_list(payload, "rules")) {
                if (rules_.count(r) == 0) {
                    violations.push_back("视图引用未注册规则: " + r);
                }
            }
            for (const auto& t : str_list(payload, "emits")) {
                if (types_.count(t) == 0) {
                    violations.push_back("视图 emits 引用未注册事件类型: " + t);
                }
            }
            if (queries_ok) {
                for (const auto& t : str_list(payload.at("queries"), "events")) {
                    if (types_.count(t) == 0) {
                        violations.push_back("视图 queries.events 引用未注册事件类型: " + t);
                    }
                }
            }
        }
    } else {  // AnchorRegistered
        if (need_str(payload, "path", violations)) {
            const std::string path = payload.at("path").get<std::string>();
            if (path.empty()) {
                violations.push_back("path 不能为空");
            } else {
                // 父路径(去掉末段)若非空须已注册
                const auto pos = path.find_last_of('/');
                if (pos != std::string::npos && pos > 0) {
                    const std::string parent = path.substr(0, pos);
                    if (anchors_.count(parent) == 0) {
                        violations.push_back("父锚点未注册: " + parent);
                    }
                }
            }
        }
        if (payload.contains("name") && !payload.at("name").is_string()) {
            violations.push_back("字段 name 须为字符串");
        }
        if (payload.contains("meta") && !payload.at("meta").is_object()) {
            violations.push_back("字段 meta 须为 object");
        }
    }

    if (!violations.empty()) return Receipt::rejected(2, std::move(violations));

    // ④ 通过:append 定义事件,fold 热更新(对应集合版本 +1)
    const json& final_payload = enriched.is_null() ? payload : enriched;
    storage::Record rec;
    rec["def_type"] = storage::vtext(def_type);
    rec["payload"]  = storage::vtext(final_payload.dump());
    const int64_t seq = backend_.append(kDefTable, std::move(rec));
    def_event_count_  = seq;
    fold_definition(def_event_count_, def_type, final_payload);
    return Receipt::settled(def_event_count_);
}

// ----------------------------------------------------------------------------
// fold_definition:单条定义事件的折叠(load 重放与 settle 共用 —— 投影纪律)
// 集合版本(DefVersions)每次 fold +1;同一 key 重复注册 version+1。
// ----------------------------------------------------------------------------
void DefinitionLayer::fold_definition(int64_t def_event_id, const std::string& def_type,
                                      const json& payload) {
    if (def_type == "AttributeRegistered") {
        AttributeEntry e;
        from_json(payload, e);
        auto it = dict_.find(e.key);
        e.version       = (it != dict_.end()) ? it->second.version + 1 : 1;
        e.registered_by = def_event_id;
        dict_[e.key] = std::move(e);
        versions_.dict++;
    } else if (def_type == "AttributeDeprecated") {
        const std::string key = payload.at("key").get<std::string>();
        auto it = dict_.find(key);
        if (it != dict_.end()) {
            it->second.status      = "deprecated";
            it->second.replaced_by = payload.value("replaced_by", std::string{});
        }
        versions_.dict++;
    } else if (def_type == "EventTypeRegistered") {
        EventTypeEntry e;
        from_json(payload, e);
        auto it = types_.find(e.type);
        e.version       = (it != types_.end()) ? it->second.version + 1 : 1;
        e.registered_by = def_event_id;
        types_[e.type] = std::move(e);
        versions_.types++;
    } else if (def_type == "RuleRegistered") {
        Rule r;
        from_json(payload, r);
        auto it = rules_.find(r.rule_id);
        r.version       = (it != rules_.end()) ? it->second.version + 1 : 1;
        r.registered_by = def_event_id;
        rules_[r.rule_id] = std::move(r);
        versions_.rules++;
    } else if (def_type == "ViewRegistered") {
        ViewEntry e;
        from_json(payload, e);
        auto it = views_.find(e.view_id);
        e.version       = (it != views_.end()) ? it->second.version + 1 : 1;
        e.registered_by = def_event_id;
        views_[e.view_id] = std::move(e);
        versions_.views++;
    } else if (def_type == "AnchorRegistered") {
        AnchorEntry e;
        from_json(payload, e);
        e.registered_by = def_event_id;
        anchors_[e.path] = std::move(e);
        versions_.anchors++;
    }
}

// ---- 查询(运行侧只读,map 查找) ----
const AttributeEntry* DefinitionLayer::find_attr(const std::string& key) const {
    auto it = dict_.find(key);
    return it == dict_.end() ? nullptr : &it->second;
}
const EventTypeEntry* DefinitionLayer::find_event_type(const std::string& type) const {
    auto it = types_.find(type);
    return it == types_.end() ? nullptr : &it->second;
}
const Rule* DefinitionLayer::find_rule(const std::string& rule_id) const {
    auto it = rules_.find(rule_id);
    return it == rules_.end() ? nullptr : &it->second;
}
const ViewEntry* DefinitionLayer::find_view(const std::string& view_id) const {
    auto it = views_.find(view_id);
    return it == views_.end() ? nullptr : &it->second;
}
const AnchorEntry* DefinitionLayer::find_anchor(const std::string& path) const {
    auto it = anchors_.find(path);
    return it == anchors_.end() ? nullptr : &it->second;
}

std::vector<std::string> DefinitionLayer::attr_keys() const {
    std::vector<std::string> out;
    out.reserve(dict_.size());
    for (const auto& [k, e] : dict_) out.push_back(k);
    return out;
}
std::vector<std::string> DefinitionLayer::event_type_names() const {
    std::vector<std::string> out;
    out.reserve(types_.size());
    for (const auto& [k, e] : types_) out.push_back(k);
    return out;
}
const std::map<std::string, Rule>& DefinitionLayer::rules() const { return rules_; }
const std::map<std::string, AnchorEntry>& DefinitionLayer::anchors() const { return anchors_; }

// ---- 全量只读枚举(自省/工具端点用;只读引用,与 rules()/anchors() 同款约定) ----
const std::map<std::string, AttributeEntry>& DefinitionLayer::attrs_all() const { return dict_; }
const std::map<std::string, EventTypeEntry>& DefinitionLayer::types_all() const { return types_; }
const std::map<std::string, ViewEntry>& DefinitionLayer::views_all() const { return views_; }

DefVersions DefinitionLayer::versions() const { return versions_; }

bool DefinitionLayer::is_registered_key(const std::string& key) const {
    auto it = dict_.find(key);
    return it != dict_.end() && it->second.status == "active";
}

} // namespace mse
