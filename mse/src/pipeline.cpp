// ============================================================================
// mse/pipeline.cpp —— 写侧管线实现:四层校验 + 决策环(单写者)
// ============================================================================

#include "mse/pipeline.h"

#include <algorithm>
#include <set>
#include <utility>

#include "mse/dictionary.h"
#include "mse/event_log.h"
#include "mse/projection.h"
#include "mse/rules.h"
#include "mse/spacetime.h"
#include "storage/record_backend.h"

namespace mse {

namespace {

constexpr const char* kIdempotencyTable = "mse_idempotency";

// 幂等键持久化表(幂等建表):pk = 幂等键,值为回执 JSON
storage::TableSchema idempotency_table_schema() {
    storage::TableSchema s;
    s.name = kIdempotencyTable;
    s.fields = {
        {"ikey", storage::FieldType::kText},     // 主键:候选层幂等键
        {"receipt", storage::FieldType::kText},  // 原回执 JSON dump
    };
    s.pk = "ikey";
    s.auto_seq = false;
    return s;
}

// on_types 过滤(变化驱动):非空时规则只在列出的事件类型结算后求值
bool on_type_matches(const Rule& r, const std::string& event_type) {
    if (r.on_types.empty()) return true;
    return std::find(r.on_types.begin(), r.on_types.end(), event_type) !=
           r.on_types.end();
}

} // namespace

// ---- RejectionLog:内存环形缓冲(超容量弹最旧) ----

void RejectionLog::push(Rejection r) {
    if (buf_.size() >= capacity_) buf_.pop_front();
    buf_.push_back(std::move(r));
}

std::vector<Rejection> RejectionLog::recent(size_t limit) const {
    std::vector<Rejection> out;
    const size_t n = std::min(limit, buf_.size());
    out.reserve(n);
    for (auto it = buf_.rbegin(); it != buf_.rend() && out.size() < n; ++it)
        out.push_back(*it);
    return out;
}

// ---- WritePipeline ----

WritePipeline::WritePipeline(DefinitionLayer& defs, EventLog& log, Projection& proj,
                             SpacetimeTree& spacetime, RuleEngine& rules)
    : defs_(defs), log_(log), proj_(proj), spacetime_(spacetime), rules_(rules) {
    // 推导规则是 fold 的一部分:挂进投影的派生钩子,在线结算(apply)与重放
    // (rebuild/崩溃恢复)走同一条路径——重放逐比特一致由此覆盖派生属性。
    // 触发规则不在钩子内:触发产生的事件已在日志中,重放不得再次发起。
    proj_.set_derive_hook([this](Projection& p, const Event& e) { derive_for_event(p, e); });
}

// 推导:对事件写及的目标本体,凡 deps ⊆ 本体键集(target 键自身可缺省——计数器
// 类派生从 null→0 起步)且本事件写及 deps 的 derive 规则,求值并把派生值写回
// 投影(多属同步在 write_derived 内部完成)。规则遍历走 std::map(字典序),确定性。
void WritePipeline::derive_for_event(Projection& p, const Event& e) {
    const Candidate& c = e;  // Event 即结算后的 ChangeSet
    for (const auto& [id, changes] : e.writes) {
        const json attrs = p.attrs_of(id);
        std::set<std::string> owned_keys;
        for (auto it = attrs.begin(); it != attrs.end(); ++it) owned_keys.insert(it.key());

        std::set<std::string> written_keys;
        if (changes.is_object())
            for (auto it = changes.begin(); it != changes.end(); ++it)
                written_keys.insert(it.key());
        auto dep_touched = [&written_keys](const Rule* r) {
            for (const std::string& d : r->deps)
                if (written_keys.count(d) != 0) return true;
            return false;
        };
        auto inject = [&](const Rule* r) {
            json inj = json::object();
            for (const std::string& d : r->deps)
                inj[d] = attrs.contains(d) ? attrs[d] : json(nullptr);
            return inj;
        };

        for (const auto& [rid, rule] : defs_.rules()) {
            const Rule* r = &rule;
            if (r->status != "active" || r->effect != "derive") continue;
            // 适用性:deps 全部存在,唯独 target_key 可缺省(派生键尚不存在时
            // var 取 null——"旧值+1"从 0 起步的初值语义;其余 dep 缺失则不适用的
            // 纪律不变,避免把缺输入的公式算出无意义值)。
            bool applicable = true;
            for (const std::string& d : r->deps) {
                if (d == r->target_key) continue;
                if (owned_keys.count(d) == 0) { applicable = false; break; }
            }
            if (!applicable) continue;
            if (!on_type_matches(*r, e.type)) continue;  // on_types:变化驱动过滤
            if (!dep_touched(r)) continue;
            try {
                RuleOutcome oc = rules_.eval(*r, inject(r), c, id);
                if (oc.kind == RuleOutcome::Kind::kValue)
                    p.write_derived(id, r->target_key, oc.value, e.event_id);
            } catch (const RuleError&) {
                // 定义层 bug:跳过该规则,不阻断已结算事实的后续处理
            }
        }
    }
}

void WritePipeline::set_rejection_log(RejectionLog* log) { rejections_ = log; }

void WritePipeline::set_trigger_depth_limit(int d) { trigger_depth_limit_ = d; }

void WritePipeline::set_snapshots(SnapshotStore* store, int64_t interval) {
    snapshots_ = store;
    snapshot_interval_ = interval;
}

void WritePipeline::set_idempotency_store(storage::RecordBackend* backend) {
    idem_backend_ = backend;
    if (idem_backend_) idem_backend_->create_table(idempotency_table_schema());  // 幂等
}

Receipt WritePipeline::submit(const Candidate& c) {
    // 候选层幂等:键已见 → 返回原回执(先内存 map,再持久化后端,命中回填内存)
    if (c.idempotency_key && !c.idempotency_key->empty()) {
        auto it = idempotency_.find(*c.idempotency_key);
        if (it != idempotency_.end()) return it->second;
        if (idem_backend_) {
            if (auto rec = idem_backend_->get(kIdempotencyTable,
                                              storage::vtext(*c.idempotency_key))) {
                Receipt r = json::parse(storage::as_text(rec->at("receipt")))
                                .get<Receipt>();
                idempotency_[*c.idempotency_key] = r;  // 回填内存 map
                return r;
            }
        }
    }
    Receipt r = submit_entry(c);
    if (r.status == Receipt::Status::kRejected) {
        // 被拒候选进拦截记录(零事件零污染,仅留反馈)
        if (rejections_)
            rejections_->push(Rejection{c, r.layer, r.violations, log_.size()});
    } else if (c.idempotency_key && !c.idempotency_key->empty()) {
        idempotency_[*c.idempotency_key] = r;
        if (idem_backend_) {  // 结算回执持久化:跨进程重启仍返回原回执
            storage::Record rec;
            rec["ikey"]    = storage::vtext(*c.idempotency_key);
            rec["receipt"] = storage::vtext(json(r).dump());
            idem_backend_->put(kIdempotencyTable, std::move(rec));
        }
    }
    return r;
}

// 顶层提交入口:四层校验(同步、即时返回拒绝)→ 按类型 settlement 分流。
//   sync  :立即结算(现有路径);
//   async :入队,返回 kAccepted(队列序号)。
// 悬态口径:异步候选"已验未结"——L0-L3 已通过但尚未进事件日志,drain_async
// 之前视图看不到它;它悬在 L0(候选)与 L1(事实)之间的队列里。被任一
// 层拒绝的候选立即返回 rejected,不入队、零污染。
Receipt WritePipeline::submit_entry(const Candidate& c) {
    Receipt r = validate_all(c);
    if (r.status == Receipt::Status::kRejected) return r;
    const EventTypeEntry* entry = defs_.find_event_type(c.type);
    if (entry && entry->settlement == "async") {
        async_queue_.push_back(c);
        return Receipt::accepted(++async_seq_);
    }
    settle(c, 0, r);
    return r;
}

// 异步队列 drain:FIFO 逐个 settle(单写者串行,全序保持;max=0 表示全部)。
// settle 内触发的递归候选仍走 submit_inner 同步语义(不回到本队列)。
size_t WritePipeline::drain_async(size_t max) {
    size_t n = 0;
    while (!async_queue_.empty() && (max == 0 || n < max)) {
        Candidate c = std::move(async_queue_.front());
        async_queue_.pop_front();
        Receipt r;
        settle(c, 0, r);
        ++n;
    }
    return n;
}

size_t WritePipeline::async_pending() const { return async_queue_.size(); }

Receipt WritePipeline::submit_inner(const Candidate& c, int depth) {
    // 触发规则的递归候选:保持同步语义(四层校验 + 立即结算),不进异步队列
    Receipt r = validate_all(c);
    if (r.status == Receipt::Status::kRejected) return r;
    settle(c, depth, r);
    return r;
}

// 四层顺序短路:任何一层不过即返回拒收回执
Receipt WritePipeline::validate_all(const Candidate& c) const {
    Receipt r = validate_layer0(c);
    if (r.status == Receipt::Status::kRejected) return r;
    r = validate_layer1(c);
    if (r.status == Receipt::Status::kRejected) return r;
    r = validate_layer2(c);
    if (r.status == Receipt::Status::kRejected) return r;
    r = validate_layer3(c);
    if (r.status == Receipt::Status::kRejected) return r;
    r.status = Receipt::Status::kSettled;
    return r;
}

// ---- [0] 字典登记:写入键全部已登记且 active(顶层系统键由种子保证,不查) ----
Receipt WritePipeline::validate_layer0(const Candidate& c) const {
    std::vector<std::string> violations;
    for (const auto& [id, changes] : c.writes) {
        if (id.empty()) violations.push_back("目标本体 id 为空");
        if (!changes.is_object()) continue;  // 形状问题由 L2 收集
        for (const auto& [key, value] : changes.items()) {
            (void)value;
            if (!defs_.is_registered_key(key))
                violations.push_back("属性键未登记: " + key);
        }
    }
    if (!violations.empty()) return Receipt::rejected(0, std::move(violations));
    Receipt ok;
    ok.status = Receipt::Status::kSettled;
    return ok;
}

// ---- [1] 可聚合性:不含 id 键的变化描述无法 fold,不予结算 ----
Receipt WritePipeline::validate_layer1(const Candidate& c) const {
    if (c.writes.empty())
        return Receipt::rejected(1, {"变化集为空:不含 id 键的变化描述无法折叠结算"});
    Receipt ok;
    ok.status = Receipt::Status::kSettled;
    return ok;
}

namespace {

// 顶层系统键必填判定:候选上该键是否"有值"
bool system_key_has_value(const std::string& key, const Candidate& c) {
    if (key == "type") return !c.type.empty();
    // "id" 键 = 可聚合性:变化集非空即携带本体句柄(writes 的键即 id)
    if (key == "id") return !c.writes.empty();
    if (key == "actor") return !c.actor.empty();
    if (key == "space")
        return c.space.kind != SpaceRef::Kind::kAnchor || !c.space.anchor.empty();
    if (key == "time") return !c.occur_time.empty();
    if (key == "evidence") return c.evidence.has_value();
    if (key == "corrects") return c.corrects.has_value();
    return false;  // 其余键不是顶层系统键
}

bool is_system_key_name(const std::string& key) {
    return key == "type" || key == "id" || key == "actor" || key == "space" ||
           key == "time" || key == "evidence" || key == "corrects";
}

// datatype 粗校验("enum" 由值域校验覆盖,未知 datatype 不拦——定义层责任)
bool datatype_matches(const std::string& datatype, const json& v) {
    if (datatype == "string") return v.is_string();
    if (datatype == "number") return v.is_number();
    if (datatype == "integer") return v.is_number_integer() || v.is_number_unsigned();
    if (datatype == "boolean") return v.is_boolean();
    if (datatype == "list") return v.is_array();
    if (datatype == "object") return v.is_object();
    if (datatype == "ref") return v.is_string();  // ref = 另一本体 id(字符串)
    return true;
}

} // namespace

// ---- [2] 类型 schema:类型已注册?必填键齐?写授权?值域?multi_target?信任级? ----
Receipt WritePipeline::validate_layer2(const Candidate& c) const {
    const EventTypeEntry* entry = defs_.find_event_type(c.type);
    if (!entry || entry->status != "active")
        return Receipt::rejected(2, {"事件类型未注册: " + c.type});

    std::vector<std::string> violations;

    // 信任分级:候选 trust 低于该类型 min_trust 即拒(凭证由 API 网关按入口注入)
    if (entry->min_trust > c.trust)
        violations.push_back("信任级不足: " + c.type + " 要求 >=" +
                             std::to_string(entry->min_trust));

    // multi_target 约束
    if (!entry->multi_target && c.writes.size() > 1)
        violations.push_back("事件类型 \"" + c.type + "\" 不允许多目标写入(writes 含 " +
                             std::to_string(c.writes.size()) + " 个目标)");

    // 必填键:顶层系统键看候选上是否有值;属性键须出现在每个目标的写入键集中
    for (const std::string& key : entry->required_keys) {
        if (is_system_key_name(key)) {
            if (!system_key_has_value(key, c))
                violations.push_back("必填系统键缺失: " + key);
            continue;
        }
        for (const auto& [id, changes] : c.writes) {
            if (changes.is_object() && !changes.contains(key))
                violations.push_back("必填键缺失: " + key + "(目标 " + id + ")");
        }
    }

    // 写授权 + 值域 + datatype(逐目标逐键,问题一次收集)
    for (const auto& [id, changes] : c.writes) {
        if (!changes.is_object()) {
            violations.push_back("目标 " + id + " 的变化集不是 object");
            continue;
        }
        for (const auto& [key, value] : changes.items()) {
            const AttributeEntry* attr = defs_.find_attr(key);
            if (!attr) continue;  // 未登记已由 L0 拦截
            bool authorized = false;
            for (const std::string& w : attr->writers) {
                if (w == "*" || w == c.type) { authorized = true; break; }
            }
            if (!authorized)
                violations.push_back("事件类型 \"" + c.type + "\" 未获授权写属性键: " + key);
            if (!attr->range.empty()) {
                bool in_range = false;
                for (const json& rv : attr->range) {
                    if (rv == value) { in_range = true; break; }
                }
                if (!in_range)
                    violations.push_back("值越出值域: " + key + "(目标 " + id + ")");
            }
            if (!datatype_matches(attr->datatype, value))
                violations.push_back("值类型不符: " + key + " 声明 " + attr->datatype +
                                     "(目标 " + id + ")");
        }
    }

    if (!violations.empty()) return Receipt::rejected(2, std::move(violations));
    Receipt ok;
    ok.status = Receipt::Status::kSettled;
    return ok;
}

// ---- [3] 规则过滤:决策环预演(当前属性集 → 该类型引用的 filter 规则) ----
// 口径:var 取目标本体当前属性(与读侧按钮可用性同一口径——读写一致性),
// 候选写入的新值由规则经 {"write":...} 算子读取(如状态机校验 旧值→新值 迁移)。
Receipt WritePipeline::validate_layer3(const Candidate& c) const {
    std::vector<std::string> violations;
    for (const auto& [id, changes] : c.writes) {
        (void)changes;
        const json attrs_current = proj_.attrs_of(id);
        std::vector<std::string> reasons = check_filters(c.type, id, attrs_current, c);
        violations.insert(violations.end(), std::make_move_iterator(reasons.begin()),
                          std::make_move_iterator(reasons.end()));
    }
    if (!violations.empty()) return Receipt::rejected(3, std::move(violations));
    Receipt ok;
    ok.status = Receipt::Status::kSettled;
    return ok;
}

// ---- 读写一致性共用入口:写侧 L3 与读侧按钮可用性走同一函数 ----
std::vector<std::string> WritePipeline::check_filters(const std::string& type,
                                                      const std::string& target_id,
                                                      const json& attrs_current,
                                                      const Candidate& candidate) const {
    const EventTypeEntry* entry = defs_.find_event_type(type);
    if (!entry) return {"事件类型未注册: " + type};
    const std::map<std::string, json> attrs_per_target{{target_id, attrs_current}};
    return rules_.eval_filters(entry->rules, defs_.rules(), attrs_per_target, candidate);
}

// ---- 重放重建:与在线结算同一条 fold 路径(含派生钩子) ----
Projection WritePipeline::replay_projection(const EventLog& log) {
    Projection p;
    p.set_derive_hook([this](Projection& pr, const Event& e) { derive_for_event(pr, e); });
    p.rebuild(log);
    return p;
}

void WritePipeline::replay_range(Projection& p, const EventLog& log,
                                 int64_t from_seq, int64_t to_seq) {
    p.set_derive_hook([this](Projection& pr, const Event& e) { derive_for_event(pr, e); });
    for (const Event& e : log.range(from_seq, to_seq)) p.apply(e);
}

// ---- 结算:append 日志 → 投影 fold(含派生)→ 时空存证 → 触发 ----
void WritePipeline::settle(const Candidate& c, int depth, Receipt& out) {
    Event e;
    static_cast<ChangeSet&>(e) = c;
    e.def_versions = json(defs_.versions());  // 四集合版本快照(审计重放的锚)
    const int64_t event_id = log_.append(std::move(e));  // event_id/settle_seq 由日志分配
    const Event stored = *log_.get(event_id);  // 取回日志分配好序号的事实副本

    proj_.apply(stored);
    spacetime_.record_event(stored);

    // 定距快照:每 interval 条已结算事件落一份投影快照(0 = 关闭,默认)
    if (snapshots_ && snapshot_interval_ > 0 && event_id % snapshot_interval_ == 0)
        snapshots_->save(event_id, proj_.snapshot_json());

    // 对刚写入的全部目标本体:触发规则(新候选回本管线走完整四层校验)。
    // 推导不在此处——它已挂进投影派生钩子,随 apply 与重放同路径执行。
    for (const auto& [id, changes] : c.writes) {
        const json attrs = proj_.attrs_of(id);
        std::set<std::string> owned_keys;
        for (auto it = attrs.begin(); it != attrs.end(); ++it) owned_keys.insert(it.key());

        // 本事件对该目标写及的键:触发是变化驱动的——deps 与写及键无交集
        // 的规则不求值(deps 声明使触发时机静态可知;也避免后续事件反复触发)。
        std::set<std::string> written_keys;
        if (changes.is_object())
            for (auto it = changes.begin(); it != changes.end(); ++it)
                written_keys.insert(it.key());
        auto dep_touched = [&written_keys](const Rule* r) {
            for (const std::string& d : r->deps)
                if (written_keys.count(d) != 0) return true;
            return false;
        };

        // 依赖注入:规则只看得见 deps 声明的键(最小权限)
        auto inject = [&](const Rule* r) {
            json inj = json::object();
            for (const std::string& d : r->deps)
                inj[d] = attrs.contains(d) ? attrs[d] : json(nullptr);
            return inj;
        };

        for (const Rule* r : rules_.match(owned_keys, defs_.rules(), "trigger")) {
            if (!on_type_matches(*r, c.type)) continue;  // on_types:变化驱动过滤
            if (!dep_touched(r)) continue;
            RuleOutcome oc;
            try {
                oc = rules_.eval(*r, inject(r), c, id);
            } catch (const RuleError&) {
                continue;  // 定义层 bug:跳过
            }
            if (oc.kind != RuleOutcome::Kind::kEmit) continue;
            for (Candidate em : std::move(oc.emitted)) {
                // 时空缺省继承父候选
                if (em.occur_time.empty()) em.occur_time = c.occur_time;
                if (em.space.kind == SpaceRef::Kind::kAnchor && em.space.anchor.empty())
                    em.space = c.space;
                if (depth + 1 <= trigger_depth_limit_) {
                    Receipt tr = submit_inner(em, depth + 1);
                    // 被拒的触发候选同样进拦截记录
                    if (tr.status == Receipt::Status::kRejected && rejections_)
                        rejections_->push(
                            Rejection{em, tr.layer, tr.violations, log_.size()});
                }
            }
        }
    }

    out = Receipt::settled(event_id);
}

} // namespace mse
