// 演示：两个摄像头对同一辆车的观测流 → 侧写浮现 → 双源合并为同一实体；
// 一次误合并的拆分；最后按属性检索实体。
// 生成 demo_entity.db（sqlite），可用 sqlite 工具查看 et_* 七张表。

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

#include "entity/resolver.h"
#include "entitytree/backends/sql_store.h"

using namespace entity;      // Resolver / ResolverConfig（算法层）
using namespace entitytree;  // SqlEntityStore / Observation（存储层）

static Observation obs(const std::string& key, const std::string& value, double conf,
                       const std::string& src, const std::string& anchor, int64_t ts) {
    Observation o;
    o.attribute_key = key;
    o.value = value;
    o.confidence = conf;
    o.source_id = src;
    o.anchor_ref = anchor;
    o.timestamp = ts;
    return o;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    SqlEntityStore store("demo_entity.db");  // 当前目录生成 demo_entity.db

    ResolverConfig cfg;
    cfg.bucket_size = 300;   // 5 分钟一个时间桶
    cfg.theta_form = 3.0;
    cfg.attribute_weight = {{"plate_number", 3.0}, {"color", 1.0}, {"brand", 1.0}};
    Resolver resolver(store, cfg);

    // 1 号门摄像头 cam1：t=100 拍到一辆车
    auto p1 = resolver.ingest(obs("plate_number", "沪A12345", 0.95, "cam1", "gate-1", 100));
    resolver.ingest(obs("color", "red", 0.9, "cam1", "gate-1", 110));
    resolver.ingest(obs("brand", "VW", 0.85, "cam1", "gate-1", 120));
    std::printf("侧写 %s 浮现，info_score=%.3f，status=%s\n", p1.c_str(),
                store.get_profile(p1).info_score, store.get_profile(p1).status.c_str());
    std::string e1 = store.effective_binding(p1)->entity_id;
    std::printf("  → 新建实体 %s（credibility=%.3f）\n", e1.c_str(),
                store.get_entity(e1).credibility);

    // 2 号门摄像头 cam2：下一个时间桶拍到"同一辆车"（相同车牌）
    auto p2 = resolver.ingest(obs("plate_number", "沪A12345", 0.92, "cam2", "gate-1", 350));
    resolver.ingest(obs("color", "red", 0.88, "cam2", "gate-1", 360));
    resolver.ingest(obs("brand", "VW", 0.8, "cam2", "gate-1", 370));
    std::printf("侧写 %s 浮现 → 自动合并到实体 %s（绑定置信度=%.3f）\n", p2.c_str(),
                store.effective_binding(p2)->entity_id.c_str(),
                store.effective_binding(p2)->confidence);
    std::printf("  实体 %s 现挂载 %zu 个侧写，credibility=%.3f（双源互证上升）\n",
                e1.c_str(), store.profiles_of_entity(e1).size(),
                store.get_entity(e1).credibility);

    // 人工复核：其实第二个侧写是另一辆同款车 → 拆分为新实体
    std::string e2 = resolver.split(p2, "嫌疑车B");
    std::printf("拆分：侧写 %s → 新实体 %s（%s）\n", p2.c_str(), e2.c_str(),
                store.get_entity(e2).name.c_str());
    std::printf("  绑定历史（append-only，全部保留）：\n");
    for (const auto& b : store.bindings_of(p2))
        std::printf("    [seq=%lld] %s → %s (conf=%.2f)\n", (long long)b.seq,
                    b.kind.c_str(), b.entity_id.c_str(), b.confidence);

    // 按属性检索实体
    std::printf("\n检索 plate_number=沪A12345：\n");
    for (const auto& h : resolver.search({{"plate_number", "沪A12345"}}, 10)) {
        auto e = store.get_entity(h.entity_id);
        std::printf("  %s %s  match=%.2f cred=%.3f final=%.3f  %s\n",
                    h.entity_id.c_str(), e.name.c_str(), h.match_score, h.credibility,
                    h.final_score, h.explanation.dump().c_str());
    }
    return 0;
}
