#pragma once

// ============================================================================
// mse/dictionary.h —— 定义层:四个数据集合(抽象层,"法"的家族)
//
// 依据《系统API设计_事件即接口》v3 第六章:
//   四个数据集合 = 属性字典 / 事件类型注册表 / 规则集 / 视图注册表,
//   外加时空锚点表(时空树子系统的定义侧)。
//   定义是数据不是远程调用:定义动作暴露为内部方法调用;启动时整体加载,
//   运行中由定义事件热更新;四集合本身是定义事件流的物化投影。
//   定义也走候选-结算(自同构):未结算的定义候选不生效。
//   引用完整性在定义结算时强校验:事件类型/规则/视图中引用的键必须已在
//   字典登记(未登记 = 编译不过);视图的 emits 必须是已注册事件类型。
//
// 定义事件类型(append-only 进定义事件树,表 mse_def_events):
//   AttributeRegistered  {key, semantic, datatype, unit, range[], writers[], kind, rule_ref?}
//   AttributeDeprecated  {key, replaced_by?}
//   EventTypeRegistered  {type, required_keys[], optional_keys[], rules[], correction?, multi_target, settlement}
//   RuleRegistered       {rule_id, deps[], effect, target_key?, logic, consumers}
//   ViewRegistered       {view_id, queries{events[], fold, spacetime?}, selects[], rules[], emits[], render_mode, variants?}
//   AnchorRegistered     {path, name?, meta?}
// 负载即各 Entry 的 JSON 形态(snake_case 字段,见各 to_json/from_json)。
// ============================================================================

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mse/model.h"
#include "mse/rules.h"

namespace storage { class RecordBackend; }

namespace mse {

// ---- 数据集合 ① 属性字典条目(API 文档 §6.1) ----
struct AttributeEntry {
    std::string key;           // 键名,域内唯一
    std::string semantic;      // 语义定义(消歧第一道防线)
    std::string datatype;      // "string"|"number"|"integer"|"boolean"|"enum"|"list"|"object"|"ref"
                               // ref:值为另一本体的 id(字符串)——关系即属性,遍历视图沿 ref 键游走
    std::string unit;          // 单位(物理量必填;无单位为空串)
    std::vector<json> range;   // 值域:该方面可取的状态集合(空 = 不约束)
    std::vector<std::string> writers;   // 写授权:允许写它的事件类型;"*"= 任意类型
    std::string kind;          // "原生"(事件直写) | "派生"(规则推导,须有 rule_ref)
    std::string rule_ref;      // 派生属性的推导规则引用(原生为空)
    std::string status = "active";   // "active" | "deprecated"
    std::string replaced_by;   // deprecated 时的替代键
    int64_t     version = 1;
    int64_t     registered_by = 0;  // 登记定义事件 id
};

// ---- 数据集合 ② 事件类型注册表条目(API 文档 §6.2) ----
struct EventTypeEntry {
    std::string type;
    std::vector<std::string> required_keys;  // 必填键(字典引用)
    std::vector<std::string> optional_keys;  // 可选键(字典引用)
    std::vector<std::string> rules;          // 适用规则引用(校验第 3 层)
    std::string correction;                  // 修正类型引用(可空;每个类型应配平)
    bool        multi_target = false;        // 是否允许一次发生写多个 id
    std::string settlement = "sync";         // "sync" | "async"(async:四层校验后入队,
                                           // drain_async 时由单写者串行结算——全序保持)
    int         min_trust = 0;               // 信任分级:候选 trust 低于此值即拒(L2);
                                             // 凭证由 API 网关按入口注入(PLC网关/人工UI/外部系统)
    std::string status = "active";
    int64_t     version = 1;
    int64_t     registered_by = 0;
};

// ---- 数据集合 ④ 视图注册表条目(API 文档 §6.4) ----
struct ViewEntry {
    struct Queries {
        std::vector<std::string> events;  // 查询的事件类型清单
        std::string fold = "L1";          // 口径:"L1"(只查事实) | "L1+L2"(事实+修正同列)
        std::string spacetime;            // 时空切片:锚点路径(空 = 不限)
    };
    struct Variant {                       // 角色预设:同一 view_id 的多套观察者配置
        std::vector<std::string> columns;  // 空 = 继承 selects
        std::vector<std::string> emits;    // 空 = 继承视图 emits
    };

    std::string view_id;
    Queries     queries;
    std::vector<std::string> selects;      // 键清单(字典引用;可跨本体、跨粒度)
    std::vector<std::string> rules;        // 规则引用(决定行/列/按钮,与写侧同一份)
    std::vector<std::string> emits;        // 可发起事件类型(②的子集引用)
    std::string render_mode = "终态";      // "终态" | "流水" | "拦截" | "遍历"
                                           // 遍历:以 params.entity 为起点沿 ref 键(关系即属性)
                                           // 与 corrects 因果边双向游走,输出节点/边图(追溯视图)
    std::map<std::string, Variant> variants;  // 角色 → 预设
    std::string status = "active";
    int64_t     version = 1;
    int64_t     registered_by = 0;
};

// ---- 时空锚点条目(时空树子系统的定义侧;层级路径段间以 '/' 分隔) ----
struct AnchorEntry {
    std::string path;          // 全路径,如 "总装车间/总装线/工位03"
    std::string name;          // 显示名(空 = 取路径末段)
    json        meta = json::object();
    int64_t     registered_by = 0;
};

// ---- 四集合版本号:定义事件每次结算使对应集合版本 +1 ----
struct DefVersions {
    int64_t dict = 0, types = 0, rules = 0, views = 0, anchors = 0;
};
void to_json(json& j, const DefVersions& v);
void from_json(const json& j, DefVersions& v);

// ---- 各条目 JSON 序列化(定义事件负载形态) ----
void to_json(json& j, const AttributeEntry& e);
void from_json(const json& j, AttributeEntry& e);
void to_json(json& j, const EventTypeEntry& e);
void from_json(const json& j, EventTypeEntry& e);
void to_json(json& j, const ViewEntry::Variant& v);
void from_json(const json& j, ViewEntry::Variant& v);
void to_json(json& j, const ViewEntry& e);
void from_json(const json& j, ViewEntry& e);
void to_json(json& j, const AnchorEntry& e);
void from_json(const json& j, AnchorEntry& e);

// ----------------------------------------------------------------------------
// DefinitionLayer:四集合的装载、查询与定义候选-结算。
// 定义入口对业务开发暴露为内部方法调用(SDK),非远程 API。
// ----------------------------------------------------------------------------
class DefinitionLayer {
public:
    /// backend 生命周期须长于本对象。构造建表(幂等);load() 重放定义事件流。
    explicit DefinitionLayer(storage::RecordBackend& backend);

    /// 启动加载:重放定义事件流,fold 出四集合(恢复 = 日志重放;快照留作后续)。
    void load();

    // ---- 定义候选-结算(自同构:定义未结算不生效) ----
    /// def_type ∈ {AttributeRegistered, AttributeDeprecated, EventTypeRegistered,
    ///             RuleRegistered, ViewRegistered, AnchorRegistered}。
    /// 校验:schema 完整 + 引用完整性(键已登记/类型已注册/规则已存在)。
    /// 通过则 append 定义事件并热更新四集合(对应版本 +1)。
    /// 返回 Receipt:kSettled 的 event_id 为定义事件 id;kRejected 带原因。
    Receipt settle_definition(const std::string& def_type, const json& payload);

    // ---- 查询(运行侧只读) ----
    const AttributeEntry* find_attr(const std::string& key) const;
    const EventTypeEntry* find_event_type(const std::string& type) const;
    const Rule*           find_rule(const std::string& rule_id) const;
    const ViewEntry*      find_view(const std::string& view_id) const;
    const AnchorEntry*    find_anchor(const std::string& path) const;

    /// 全部已登记属性键(含 deprecated)。
    std::vector<std::string> attr_keys() const;
    /// 全部已注册事件类型名(含 deprecated)。
    std::vector<std::string> event_type_names() const;
    /// 规则集(写侧过滤与读侧渲染消费同一份)。
    const std::map<std::string, Rule>& rules() const;
    /// 锚点表(按路径字典序)。
    const std::map<std::string, AnchorEntry>& anchors() const;

    DefVersions versions() const;

    /// 键是否已登记且 active。
    bool is_registered_key(const std::string& key) const;

private:
    storage::RecordBackend& backend_;

    std::map<std::string, AttributeEntry> dict_;
    std::map<std::string, EventTypeEntry> types_;
    std::map<std::string, Rule>           rules_;
    std::map<std::string, ViewEntry>      views_;
    std::map<std::string, AnchorEntry>    anchors_;
    DefVersions versions_;
    int64_t     def_event_count_ = 0;  // 定义事件 id 递增至该值

    // 单条定义事件的 fold(load 重放与 settle 共用同一份折叠逻辑——投影纪律)
    void fold_definition(int64_t def_event_id, const std::string& def_type, const json& payload);
};

} // namespace mse
