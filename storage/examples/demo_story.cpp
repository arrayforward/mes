// 演示：一个故事、两个事件、每个事件两个视角的侧写；
// "从前 → 第二天" 的虚拟时间顺序靠 Link 表达；含重名"张伟"的指代消解。

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

#include "eventstore/backends/sql_store.h"

using namespace eventstore;

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    SqlStore store("demo_story.db");  // 当前目录生成 demo_story.db，可用 sqlite 工具查看

    auto story = Narrative::create("story", "勇者斗恶龙");
    store.append_narrative(story);

    // 事件一：从前，勇者拔出圣剑
    auto ev1 = Event::create(story.narrative_id, "勇者拔剑");
    store.append_event(ev1);
    auto p1_active = Profile::create(ev1.event_id, "active",
                                     TimeRef::virtual_time("从前"),
                                     "村口老树下", "勇者", "拔出", "圣剑",
                                     {{"mood", "决绝"}},
                                     {Modifier{"verb", "manner", "奋力"},
                                      Modifier{"object", "quantity", "一把"},
                                      Modifier{"object", "quality", "锋利无比"},
                                      Modifier{"object", "degree", "削铁如泥地"}});
    auto p1_observer = Profile::create(ev1.event_id, "observer",
                                       TimeRef::virtual_time("从前"),
                                       "村口老树下", "张伟", "看到", "勇者拔剑");
    store.append_profile(p1_active);
    store.append_profile(p1_observer);

    // 事件二：第二天，恶龙袭击村庄
    auto ev2 = Event::create(story.narrative_id, "恶龙袭击村庄");
    store.append_event(ev2);
    auto p2_active = Profile::create(ev2.event_id, "active",
                                     TimeRef::virtual_time("第二天"),
                                     "村庄上空", "恶龙", "袭击", "村庄");
    auto p2_passive = Profile::create(ev2.event_id, "passive",
                                      TimeRef::virtual_time("第二天"),
                                      "村庄", "张伟", "躲避", "恶龙");
    store.append_profile(p2_active);
    store.append_profile(p2_passive);

    // 虚拟时间没有全序：用 Link 显式表达 "从前 → 第二天"
    store.append_link(Link{p1_active.profile_id, p2_active.profile_id, "before"});
    store.append_link(Link{p1_observer.profile_id, p2_passive.profile_id, "before"});

    // 人名问题：两个侧写里的 "张伟" 其实是两个不同的人
    auto zw_east = Entity::create("张伟", "person", json::array(), {{"village", "东村"}});
    auto zw_west = Entity::create("张伟", "person", json::array(), {{"village", "西村"}});
    store.append_entity(zw_east);
    store.append_entity(zw_west);
    store.append_binding(Binding::create(p1_observer.profile_id, "subject", zw_east.entity_id));
    store.append_binding(Binding::create(p2_passive.profile_id, "subject", zw_west.entity_id));

    std::printf("叙事：《%s》时间线（虚拟时间，按 Link 拓扑排序）\n", story.title.c_str());
    for (const auto& p : store.timeline(story.narrative_id)) {
        std::string who = p.subject;
        if (auto b = store.effective_binding(p.profile_id, "subject")) {
            auto e = store.get_entity(b->entity_id);
            who += "（实体 " + e.entity_id + "，" + e.attributes.value("village", "") + "）";
        }
        std::string mods;
        for (const auto& m : p.modifiers)
            mods += " [" + m.target + ":" + m.kind + ":" + m.text + "]";
        std::printf("  [seq=%lld][%s][视角:%s] %s %s %s @ %s%s\n",
                    (long long)p.seq, p.time.value.c_str(), p.perspective.c_str(),
                    who.c_str(), p.verb.c_str(), p.object.c_str(), p.place.c_str(),
                    mods.c_str());
    }

    std::printf("\n名为「张伟」的实体有 %zu 个（重名不同人）：\n",
                store.find_entities_by_name("张伟").size());
    for (const auto& e : store.find_entities_by_name("张伟")) {
        std::printf("  %s: %s\n", e.entity_id.c_str(), e.attributes.dump().c_str());
        for (const auto& p : store.profiles_of_entity(e.entity_id))  // 实体→侧写反查
            std::printf("    └─ 参与侧写 [seq=%lld][%s] %s %s %s\n",
                        (long long)p.seq, p.time.value.c_str(),
                        p.subject.c_str(), p.verb.c_str(), p.object.c_str());
    }

    std::printf("\n侧写图（%s）：\n%s\n", p1_active.profile_id.c_str(),
                store.get_profile(p1_active.profile_id).graph.dump(2).c_str());
    return 0;
}
