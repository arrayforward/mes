#include "eventstore/event_store.h"

#include <algorithm>
#include <queue>
#include <unordered_map>

#include "eventstore/errors.h"

namespace eventstore {

std::vector<Profile> EventStore::timeline(const std::string& narrative_id) {
    // 收集叙事下全部侧写
    std::vector<Profile> all;
    for (const auto& ev : events_of(narrative_id)) {
        auto ps = profiles_of(ev.event_id);
        all.insert(all.end(), ps.begin(), ps.end());
    }
    std::sort(all.begin(), all.end(),
              [](const Profile& a, const Profile& b) { return a.seq < b.seq; });

    const int n = (int)all.size();
    std::unordered_map<std::string, int> idx;
    idx.reserve((size_t)n * 2);
    for (int i = 0; i < n; ++i) idx.emplace(all[i].profile_id, i);

    // 只有 before/causes 边参与排序；refines/retracts 是纠正关系，不约束叙事顺序
    std::vector<std::vector<int>> adj((size_t)n);
    std::vector<int> indeg((size_t)n, 0);
    for (int i = 0; i < n; ++i) {
        for (const auto& l : links_from(all[i].profile_id)) {
            if (l.relation != "before" && l.relation != "causes") continue;
            auto it = idx.find(l.to_profile_id);
            if (it == idx.end()) continue;  // 指向叙事之外的边不参与排序
            adj[(size_t)i].push_back(it->second);
            ++indeg[(size_t)it->second];
        }
    }

    // Kahn 拓扑排序；就绪节点中 seq 小者优先，保证输出确定。
    auto cmp = [&](int a, int b) { return all[(size_t)a].seq > all[(size_t)b].seq; };
    std::priority_queue<int, std::vector<int>, decltype(cmp)> pq(cmp);
    for (int i = 0; i < n; ++i)
        if (indeg[(size_t)i] == 0) pq.push(i);

    std::vector<Profile> out;
    out.reserve((size_t)n);
    while (!pq.empty()) {
        int u = pq.top();
        pq.pop();
        out.push_back(all[(size_t)u]);
        for (int v : adj[(size_t)u])
            if (--indeg[(size_t)v] == 0) pq.push(v);
    }
    if ((int)out.size() != n)
        throw EventStoreError("timeline: cycle detected in narrative " + narrative_id);
    return out;
}

} // namespace eventstore
