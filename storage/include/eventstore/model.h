#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace eventstore {

using nlohmann::json;

/// 生成带前缀的随机 ID，如 "pf-3fa9c2e17b04a1c2"
std::string new_id(const std::string& prefix);

/// 时间引用：真实时间（ISO 时间戳）或虚拟时间（"从前"、"第二天"）。
/// 虚拟时间没有全序，侧写间的先后关系由 Link 显式表达。
struct TimeRef {
    std::string kind;                // "real" | "virtual"
    std::string value;               // ISO 时间戳 或虚拟时间标签
    std::optional<int64_t> ordinal;  // 可选的局部序数

    static TimeRef real(std::string iso_timestamp);
    static TimeRef virtual_time(std::string label, std::optional<int64_t> ordinal = std::nullopt);
};

/// 叙事来源：一个故事 / 一段事实 / 一个说明书场景，是事件的顶层容器。
struct Narrative {
    std::string narrative_id;
    std::string kind;  // "story" | "fact" | "scenario"
    std::string title;
    json meta = json::object();

    static Narrative create(std::string kind, std::string title, json meta = json::object());
};

/// 事件：只作分组锚点，事件的内容全部在侧写（Profile）里。
struct Event {
    std::string event_id;
    std::string narrative_id;
    std::string summary;
    json meta = json::object();

    static Event create(std::string narrative_id, std::string summary, json meta = json::object());
};

/// 修饰语：附着在侧写五要素上的形容词 / 量词 / 程度词 / 方式词。
/// 骨架抽取会丢掉"锋利""奋力""一把"这类语义，Modifier 把它们作为
/// 侧写的一等组成部分保留下来：有序、可索引、进图。
struct Modifier {
    std::string target;  // 附着点："subject" | "verb" | "object" | "place" | "time"
    std::string kind;    // "quality"(性质形容词) | "quantity"(量词/数量)
                         // "degree"(程度词) | "manner"(方式/情态副词)
    std::string text;    // 原文："锋利" / "一把" / "非常" / "奋力"
};

/// 侧写：原子单元。同一事件可有多个侧写（不同视角），各自独立存储。
/// 五要素：时间、地点、谁、（动词）干了什么、对什么/谁。
/// subject/object/place 只存表面字符串（surface），不内嵌实体 ID ——
/// 指代消解由 Binding 以 append-only 方式单独记录，侧写本身不可变。
struct Profile {
    std::string profile_id;
    std::string event_id;
    std::string perspective;      // 视角：active/passive/observer/left/right/... 自由字符串
    TimeRef time;
    std::string place;
    std::string subject;
    std::string verb;
    std::string object;
    std::vector<Modifier> modifiers;  // 修饰语（保留顺序：同槽位多个修饰语的先后有意义）
    json graph = json::object();   // 五要素+修饰语组装成的图（见 build_graph）
    json payload = json::object(); // 任意附加数据
    int64_t seq = 0;               // 存储层分配的全局自增序号（物理追加序）

    static Profile create(std::string event_id, std::string perspective, TimeRef time,
                          std::string place, std::string subject, std::string verb,
                          std::string object, json payload = json::object(),
                          std::vector<Modifier> modifiers = {});
};

/// 侧写间的先后/因果边（DAG）。虚拟时间下表达"谁先谁后"的唯一手段。
struct Link {
    std::string from_profile_id;
    std::string to_profile_id;
    std::string relation;  // "before" | "causes" | "refines" | "retracts"
};

/// 实体：世界中可辨识的对象（人、地点、组织、物……），实体搜索树的雏形。
/// name 可重复：两个 "张伟" 是两个不同的 entity_id，重名不同人天然支持。
/// aliases 支持同人异名："张三"/"小三"/"张老三" → 同一 entity_id。
struct Entity {
    std::string entity_id;
    std::string name;                 // 规范名（可与其他实体重复）
    std::string type;                 // "person" | "place" | "org" | "thing" | ...
    json aliases = json::array();     // 别名/指称
    json attributes = json::object(); // 任意属性

    static Entity create(std::string name, std::string type,
                         json aliases = json::array(), json attributes = json::object());
};

/// 指代消解记录（append-only）：把某侧写的某个槽位绑定到某实体。
/// 同一 (profile_id, slot) 可有多条 Binding：重消歧 = 追加新记录，
/// 有效指代 = seq 最新的一条（latest wins），历史全部保留可审计。
struct Binding {
    std::string binding_id;
    std::string profile_id;
    std::string slot;        // "subject" | "object" | "place"
    std::string entity_id;
    double confidence = 1.0; // 概率性消解：感知层/LLM 的不确定判断可给 <1.0
    std::string note;
    int64_t seq = 0;         // 存储层分配的自增序号

    static Binding create(std::string profile_id, std::string slot, std::string entity_id,
                          double confidence = 1.0, std::string note = "");
};

/// 把侧写五要素+修饰语组装成图：节点 = 主体/客体/地点/时间/修饰语，
/// 边 = 动词及附属关系 + 修饰关系。
json build_graph(const Profile& p);

void to_json(json& j, const TimeRef& t);
void from_json(const json& j, TimeRef& t);
void to_json(json& j, const Modifier& m);
void from_json(const json& j, Modifier& m);
void to_json(json& j, const Narrative& n);
void from_json(const json& j, Narrative& n);
void to_json(json& j, const Event& e);
void from_json(const json& j, Event& e);
void to_json(json& j, const Profile& p);
void from_json(const json& j, Profile& p);
void to_json(json& j, const Link& l);
void from_json(const json& j, Link& l);
void to_json(json& j, const Entity& e);
void from_json(const json& j, Entity& e);
void to_json(json& j, const Binding& b);
void from_json(const json& j, Binding& b);

} // namespace eventstore
