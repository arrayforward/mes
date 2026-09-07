#pragma once

// ============================================================================
// mse/model.h —— 核心数据形态(存在层:本体与事件)
//
// 依据 docs/mes《系统API设计_事件即接口》v3 与《车间世界模型_系统实现方案》v1:
//   事件 = 某本体的某属性,在某时空,变成了某值。
//   一次发生可写多个 id 下的属性("一次发生、多处变化")。
//   无特权字段:type/id/actor/evidence/corrects 全是普通键(在字典中登记为
//   系统键);结算层校验"可聚合性"(变化集含 id 键),不是引用关系。
//   append-only:已结算事件永不修改;纠错 = 携带 corrects 键的新事件。
// ============================================================================

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mse {

using nlohmann::json;

// ----------------------------------------------------------------------------
// SpaceRef:时空参照三形态,精度分级(API 文档第五章)
//   绝对参照(坐标)→ 相对参照(锚点:"2 号工位东侧""左前门")→ 模糊文本
//   模糊纪律:原文照存(append-only,作为证据),永不改写。
// ----------------------------------------------------------------------------
struct SpaceRef {
    enum class Kind { kAnchor, kCoord, kRawText };

    Kind        kind = Kind::kAnchor;
    std::string anchor;    // kAnchor: 锚点路径,段间以 '/' 分隔,如 "总装车间/总装线/工位03"
    json        coord;     // kCoord: [x, y, z]
    std::string raw_text;  // kRawText: 模糊原文(永不改写)
    int         precision = 0;  // 精度等级:0=精确锚点 1=区域级 2=模糊文本

    static SpaceRef anchor_ref(std::string path, int precision = 0);
    static SpaceRef coordinate(json xyz);
    static SpaceRef fuzzy(std::string raw_text);  // precision 恒为 2
};

// ----------------------------------------------------------------------------
// ChangeSet:一次"发生"的完整描述。Candidate(候选,L0)与 Event(事实,L1/L2)
// 共享同一内核;Event 只是结算后的 ChangeSet + 结算元数据。
// ----------------------------------------------------------------------------
struct ChangeSet {
    std::string type;                    // 事件类型(普通键,注册表引用)
    std::map<std::string, json> writes;  // {本体id: {属性键: 新值, ...}, ...}
                                         // 键即本体句柄——含 id 键即可聚合(可聚合性)
    std::string actor;                   // 发起者本体 id(普通键)
    SpaceRef    space;                   // 时空戳:空间
    std::string occur_time;              // 时空戳:发生时间 ISO(可模糊,原文照存)
    std::optional<json>     evidence;    // 依据(扫描原始值/图片哈希/外部单号)
    std::optional<int64_t>  corrects;    // 因果引用:被修正的原事件 id(普通键)
    std::optional<std::string> idempotency_key;  // 候选层幂等键(来源方生成)
    int         trust = 0;               // 信任级(凭证元数据,由 API 网关按入口凭证注入;
                                         // 不序列化进事件——它不是事实的一部分)
};

using Candidate = ChangeSet;  // 候选 = 未结算的变化描述(L0)

// ----------------------------------------------------------------------------
// Event:已结算事实(L1;携带 corrects 的为 L2 修正)。append-only,永不修改。
// ----------------------------------------------------------------------------
struct Event : ChangeSet {
    int64_t     event_id = 0;     // 结算时分配,域内单调(== settle_seq)
    int64_t     settle_seq = 0;   // 逻辑时钟戳(单写者结算器保证域内全序)
    std::string settle_time;      // 结算时间(系统落账时刻,与发生时间互不冒充)
    json        def_versions;     // 结算时四集合版本快照 {"dict":n,"types":n,"rules":n,"views":n,"anchors":n}
                                  // ——审计重放的锚:同输入+同定义版本 = 逐比特重放
};

// ----------------------------------------------------------------------------
// Receipt:候选回执。结算成功返回事件 id;被拒返回拦截层号与原因清单。
// 四层校验:0=字典登记 1=可聚合性 2=类型schema 3=规则过滤。
// 被拒的候选零事件、零补偿、零污染(不进事件树)。
// ----------------------------------------------------------------------------
struct Receipt {
    enum class Status { kSettled, kRejected, kAccepted };

    Status status = Status::kRejected;
    std::optional<int64_t> event_id;      // kSettled 时有值
    std::optional<int64_t> queue_seq;     // kAccepted 时有值(异步队列序号)
    int                     layer = -1;   // kRejected:拦截发生在第几层(0..3)
    std::vector<std::string> violations;  // 拒绝原因/违反的规则清单

    static Receipt settled(int64_t event_id);
    static Receipt rejected(int layer, std::vector<std::string> violations);
    /// kAccepted:异步结算类型已入队(队列序号;结算结果由队列 drain 后落日志)。
    static Receipt accepted(int64_t queue_seq);
};

// ---- JSON 序列化(事件落盘/落日志用;字段序固定以保证序列化确定性) ----
void to_json(json& j, const SpaceRef& s);
void from_json(const json& j, SpaceRef& s);
void to_json(json& j, const ChangeSet& c);
void from_json(const json& j, ChangeSet& c);
void to_json(json& j, const Event& e);
void from_json(const json& j, Event& e);
void to_json(json& j, const Receipt& r);
void from_json(const json& j, Receipt& r);

} // namespace mse
