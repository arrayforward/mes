// ============================================================================
// mse/view_engine.cpp —— 读侧视图引擎实现(四元组求值;全程只读,无写路径)
// ============================================================================

#include "mse/view_engine.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <tuple>

#include "mse/dictionary.h"
#include "mse/event_log.h"
#include "mse/pipeline.h"  // RejectionLog(拦截模式数据源)
#include "mse/projection.h"
#include "mse/rules.h"
#include "mse/spacetime.h"

namespace mse {

ViewEngine::ViewEngine(DefinitionLayer& defs, Projection& proj, EventLog& log,
                       RuleEngine& rules, SpacetimeTree& spacetime)
    : defs_(defs), proj_(proj), log_(log), rules_(rules), spacetime_(spacetime) {
    (void)spacetime_;  // 时空切片用静态精度匹配,运行期服务不由读侧触碰
}

void ViewEngine::set_rejection_log(const RejectionLog* log) { rejections_ = log; }

void ViewEngine::set_snapshot_store(const SnapshotStore* snapshots) {
    snapshots_ = snapshots;
}

void ViewEngine::set_replay_provider(
    std::function<void(Projection&, int64_t, int64_t)> provider) {
    replay_provider_ = std::move(provider);
}

namespace {

// 遍历视图的 BFS 深度(ViewParams 无深度字段,固定 3)
constexpr int kTraversalDepth = 3;

// 一条遍历边:{from, key, to, source_event};按 (from,key,to) 字典序去重排序
using EdgeKey = std::tuple<std::string, std::string, std::string>;

// 遍历模式求值:以 entity 为起点,沿 ref 边双向 BFS(正反向都游走),
// corrects 因果边只在其两端本体均已访问时纳入(遍历的主角是本体;
// 同 id 自环无意义,跳过)。nodes/edges 按字典序输出,确定性。
json render_traversal(const DefinitionLayer& defs, const EventLog& log,
                      const ViewEntry& view, const ViewParams& params,
                      const Projection& proj, int64_t seq_limit,
                      const std::vector<std::string>& columns) {
    if (!params.entity || params.entity->empty()) {
        return json{{"view_id", view.view_id},
                    {"render_mode", "遍历"},
                    {"error", "遍历视图缺少 entity 参数(纵切起点)"}};
    }
    const std::string& root = *params.entity;

    // ① ref 边集:终态投影中 datatype=="ref" 且值为字符串的属性即关系
    std::map<EdgeKey, int64_t> edges;
    // 邻接表(双向):from → [(key, to)]
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> adj;
    for (const Ontology* ont : proj.ontologies()) {
        for (const auto& [key, cell] : ont->attrs) {
            const AttributeEntry* attr = defs.find_attr(key);
            if (!attr || attr->datatype != "ref") continue;
            if (!cell.value.is_string() || cell.value.get<std::string>().empty())
                continue;
            const std::string to = cell.value.get<std::string>();
            EdgeKey ek{ont->id, key, to};
            if (edges.count(ek) == 0) edges.emplace(ek, cell.source_event);
            adj[ont->id].emplace_back(key, to);
            adj[to].emplace_back(key, ont->id);  // 反向:他者 ref 指向我
        }
    }

    // ② BFS:正反向都游走,深度 ≤ kTraversalDepth
    std::set<std::string> visited;
    std::queue<std::pair<std::string, int>> bfs;
    visited.insert(root);
    bfs.emplace(root, 0);
    while (!bfs.empty()) {
        auto [cur, depth] = bfs.front();
        bfs.pop();
        if (depth >= kTraversalDepth) continue;
        auto it = adj.find(cur);
        if (it == adj.end()) continue;
        for (const auto& [key, next] : it->second) {
            (void)key;
            if (visited.count(next) != 0) continue;
            visited.insert(next);
            bfs.emplace(next, depth + 1);
        }
    }

    // ③ corrects 因果边:L1+L2 口径——修正的终态已由投影 fold 处理,
    //    遍历只看终态投影 + 全量日志的 corrects 边;仅两端本体均已访问时纳入
    for (const Event& e : log.all()) {
        if (e.settle_seq > seq_limit || !e.corrects) continue;
        const auto orig = log.get(*e.corrects);
        if (!orig) continue;
        for (const auto& [a, wa] : e.writes) {
            (void)wa;
            if (visited.count(a) == 0) continue;
            for (const auto& [b, wb] : orig->writes) {
                (void)wb;
                if (a == b || visited.count(b) == 0) continue;  // 自环跳过
                EdgeKey ek{a, "corrects", b};
                if (edges.count(ek) == 0) edges.emplace(ek, e.event_id);
            }
        }
    }

    // ④ 输出:nodes 按 id 字典序(std::set 迭代序);edges 只保留两端均已访问
    //    的边(遍历视图渲染从 root 可达的子图,未达节点的边是噪声)
    json nodes = json::array();
    for (const std::string& id : visited) {
        const json attrs = proj.attrs_of(id);
        json node;
        node["id"] = id;
        for (const std::string& col : columns)
            node[col] = attrs.contains(col) ? attrs[col] : json(nullptr);
        nodes.push_back(std::move(node));
    }
    json edge_arr = json::array();
    for (const auto& [ek, src] : edges) {
        const auto& [from, key, to] = ek;
        if (visited.count(from) == 0 || visited.count(to) == 0) continue;
        edge_arr.push_back(
            {{"from", from}, {"key", key}, {"to", to}, {"source_event", src}});
    }

    json out;
    out["view_id"] = view.view_id;
    out["render_mode"] = "遍历";
    out["observer"] = params.observer;
    out["as_of_seq"] = params.as_of_seq ? json(*params.as_of_seq) : json(nullptr);
    out["root"] = root;
    out["nodes"] = std::move(nodes);
    out["edges"] = std::move(edge_arr);
    return out;
}

} // namespace

json ViewEngine::render(const std::string& view_id, const ViewParams& params) const {
    const ViewEntry* view = defs_.find_view(view_id);
    if (!view) return json{{"error", "视图未注册: " + view_id}};
    if (view->status != "active") return json{{"error", "视图已弃用: " + view_id}};

    // 观察者上下文:variants 预设覆盖 columns/emits(空则继承视图声明)
    std::vector<std::string> columns = view->selects;
    std::vector<std::string> emits = view->emits;
    if (!params.observer.empty()) {
        auto it = view->variants.find(params.observer);
        if (it != view->variants.end()) {
            if (!it->second.columns.empty()) columns = it->second.columns;
            if (!it->second.emits.empty()) emits = it->second.emits;
        }
    }

    // AS OF t:临时投影 = floor 快照(若有)+ (floor.seq, t] 增量重放;
    // 未挂接快照库或未命中时退化为 [1, seq] 全量重放。
    // 重放优先走 replay_provider(含派生钩子的同一条 fold 路径),
    // 否则裸 fold(无派生属性——仅供无 System 装配的裸测试)。
    std::optional<Projection> as_of_proj;
    if (params.as_of_seq) {
        as_of_proj.emplace();
        int64_t from = 1;
        if (snapshots_) {
            if (auto snap = snapshots_->floor(*params.as_of_seq)) {
                as_of_proj->load_snapshot(snap->second);
                from = snap->first + 1;
            }
        }
        if (replay_provider_) {
            replay_provider_(*as_of_proj, from, *params.as_of_seq);
        } else {
            for (const Event& e : log_.range(from, *params.as_of_seq)) as_of_proj->apply(e);
        }
    }
    const Projection& proj = as_of_proj ? *as_of_proj : proj_;
    const int64_t seq_limit =
        params.as_of_seq.value_or(std::numeric_limits<int64_t>::max());

    // 遍历模式:以 params.entity 为起点沿 ref 键(关系即属性)双向 BFS,
    // 叠加 corrects 因果边(仅两端本体均已访问时纳入)。输出节点/边图。
    if (view->render_mode == "遍历") {
        return render_traversal(defs_, log_, *view, params, proj, seq_limit, columns);
    }

    // 事件收集:queries.events 逐类型合并,按 settle_seq 排序
    std::vector<Event> events;
    for (const std::string& type : view->queries.events) {
        for (const Event& e : log_.events_of_type(type)) {
            if (e.settle_seq > seq_limit) continue;
            // entity 纵切:事件 writes 键含该本体 id
            if (params.entity && !e.writes.contains(*params.entity)) continue;
            events.push_back(e);
        }
    }
    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) { return a.settle_seq < b.settle_seq; });
    events.erase(std::unique(events.begin(), events.end(),
                             [](const Event& a, const Event& b) {
                                 return a.event_id == b.event_id;
                             }),
                 events.end());

    // 时空切片:参数覆盖视图定义;kRawText 永不匹配锚点切片
    const std::string& slice =
        params.spacetime.empty() ? view->queries.spacetime : params.spacetime;
    if (!slice.empty()) {
        events.erase(std::remove_if(events.begin(), events.end(),
                                    [&](const Event& e) {
                                        return !SpacetimeTree::in_slice(slice, e.space);
                                    }),
                     events.end());
    }

    // 行过滤规则:视图 rules 引用中 consumers ∈ {read, both} 的 filter 规则
    std::vector<std::string> row_filter_refs;
    for (const std::string& rid : view->rules) {
        const Rule* r = defs_.find_rule(rid);
        if (r && r->effect == "filter" && r->status == "active" &&
            (r->consumers == "read" || r->consumers == "both"))
            row_filter_refs.push_back(rid);
    }

    // 终态/拦截共用:行 = 被这些事件触及的本体(最近事实)
    auto build_terminal_rows = [&]() -> json {
        json rows = json::array();
        std::set<std::string> ids;  // 去重排序
        for (const Event& e : events)
            for (const auto& [id, changes] : e.writes) {
                (void)changes;
                ids.insert(id);
            }
        for (const std::string& id : ids) {
            const json attrs = proj.attrs_of(id);  // 纯值视图
            // 行过滤:synthetic candidate(无类型、空变化集)
            Candidate cand;
            cand.type = "";
            cand.writes = {{id, json::object()}};
            const std::map<std::string, json> attrs_per_target{{id, attrs}};
            if (!rules_.eval_filters(row_filter_refs, defs_.rules(), attrs_per_target, cand,
                                     "read")
                     .empty())
                continue;  // 任一 reject → 行不可见

            json row;
            row["id"] = id;
            for (const std::string& col : columns)
                row[col] = attrs.contains(col) ? attrs[col] : json(nullptr);

            // 行级按钮:每个 emit 类型求其 rules 引用的 filter 规则(与写侧 L3 同一份)。
            // 按钮可用性按"该类型的语义动作自带写入(presets)"求值:固定写入值并入
            // 合成候选的变化集,写值类 filter 规则(如 R-CALL-ANSWER-VAL)看到的即
            // 按钮真实会带的写入;按钮输出携带 presets 供表单预填(空 object 也带)。
            json buttons = json::array();
            for (const std::string& t : emits) {
                const EventTypeEntry* te = defs_.find_event_type(t);
                const json presets = te ? te->presets : json::object();
                const std::vector<std::string> refs =
                    te ? te->rules : std::vector<std::string>{};
                cand.writes[id] = presets;
                std::vector<std::string> reasons =
                    rules_.eval_filters(refs, defs_.rules(), attrs_per_target, cand);
                buttons.push_back({{"type", t},
                                   {"enabled", reasons.empty()},
                                   {"reasons", reasons},
                                   {"presets", presets}});
            }
            row["buttons"] = std::move(buttons);
            rows.push_back(std::move(row));
        }
        return rows;
    };

    // 流水:行 = 事件;fold=="L1" 排除修正,"L1+L2" 全含
    auto build_flow_rows = [&]() -> json {
        json rows = json::array();
        const bool l1_only = view->queries.fold != "L1+L2";
        for (const Event& e : events) {
            if (l1_only && e.corrects) continue;
            json row;
            row["event_id"] = e.event_id;
            row["type"] = e.type;
            row["actor"] = e.actor;
            row["occur_time"] = e.occur_time;
            row["space"] = e.space;
            row["writes"] = e.writes;
            row["corrects"] = e.corrects ? json(*e.corrects) : json(nullptr);
            row["is_correction"] = e.corrects.has_value();
            rows.push_back(std::move(row));
        }
        return rows;
    };

    json out;
    out["view_id"] = view_id;
    out["render_mode"] = view->render_mode;
    out["observer"] = params.observer;
    out["as_of_seq"] = params.as_of_seq ? json(*params.as_of_seq) : json(nullptr);
    out["columns"] = columns;

    if (view->render_mode == "流水") {
        out["rows"] = build_flow_rows();
    } else {
        // 终态与拦截的行同为已结算事实(拦截额外带近期 L0 拦截反馈)
        out["rows"] = build_terminal_rows();
    }

    if (view->render_mode == "拦截") {
        json rejections = json::array();
        if (rejections_) {
            for (const Rejection& r : rejections_->recent(50)) {
                rejections.push_back({{"layer", r.layer},
                                      {"violations", r.violations},
                                      {"candidate",
                                       {{"type", r.candidate.type},
                                        {"actor", r.candidate.actor},
                                        {"writes", r.candidate.writes}}}});
            }
        }
        out["rejections"] = std::move(rejections);
    }

    // 视图级 actions:仅列可发起类型(行级可用性在 buttons);presets 随动作携带
    json actions = json::array();
    for (const std::string& t : emits) {
        const EventTypeEntry* te = defs_.find_event_type(t);
        actions.push_back(
            {{"type", t}, {"presets", te ? te->presets : json::object()}});
    }
    out["actions"] = std::move(actions);

    return out;
}

} // namespace mse
