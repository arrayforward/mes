#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace entitytree {

using nlohmann::json;

/// 生成带前缀的随机 ID，如 "ob-3fa9c2e17b04a1c2"（与 eventstore::new_id 同款风格）
std::string new_id(const std::string& prefix);

/// 属性观测：最小证据单元，append-only，永不修改。
struct Observation {
    std::string observation_id;   // "ob-..."
    std::string attribute_key;    // "plate_number" | "face_desc" | "color" | ...
    std::string value;            // 文本化的值（向量/embedding 后续扩展，本期不做）
    double      confidence = 1.0; // 0~1，感知器给出
    std::string source_id;        // 观测来源（摄像头/LLM/人工……）
    std::string anchor_ref;       // 空间锚点 ID
    int64_t     timestamp = 0;    // 物理时间（秒）
    std::string event_ref;        // 溯源事件 ID，可空
    int64_t     seq = 0;          // 存储层分配的自增序号
};

/// 侧写（属性包）：一个时空窗口（anchor_ref × time_bucket）内观测的聚合。
/// 镜像永久保留；status 流转：pool（积累中）→ emerged（达阈值，待解析）→ linked（已挂实体）。
/// level：时间桶分辨率层级（0=最细；>0 为 entity 组件 rollup 生成的粗桶镜像，
/// time_bucket 以该层桶长为单位）。注意与 stmb LOD 相反：这里 0 最细。
struct AttrProfile {
    std::string profile_id;
    std::string anchor_ref;
    int64_t     time_bucket = 0;
    int         level = 0;            // 时间桶分辨率层级（0=最细）
    json        attributes = json::object(); // {key: [{value,confidence,source_id,obs_seq,ts}, ...]}
    double      info_score = 0.0;
    std::string status = "pool";  // "pool" | "emerged" | "linked"
    int64_t     seq = 0;          // 创建它的首条观测的 seq（排序用）
};

/// 实体：侧写的归并视图。当前态可变（upsert），merged view 是缓存，带版本号。
struct EntityNode {
    std::string entity_id;
    std::string name;                 // 可空字符串
    std::string type;                 // "person" | "vehicle" | "thing" | ...
    json        attributes = json::object(); // merged view：{key: [{value,weight}...]}
    int64_t     view_version = 0;
    double      credibility = 0.0;    // 0~1 实体级可信度（基准值，不随时间改写）
    std::string status = "candidate"; // "candidate" | "confirmed" | "disputed"
    int64_t     updated_at = 0;       // credibility 基准时间（秒）：最近一次重建时
                                      // 全部有效侧写中最新的观测时间
};

/// 侧写→实体的绑定（append-only，latest wins）。合并 = 追加 merge 绑定；
/// 拆分 = 把部分侧写追加指向新实体的 split 绑定。全历史保留可审计。
struct EntityBinding {
    std::string binding_id;
    std::string profile_id;
    std::string entity_id;
    double      confidence = 1.0;
    std::string kind;   // "merge" | "split" | "manual"
    std::string note;
    int64_t     seq = 0;  // 存储层分配的自增序号
};

/// 来源可靠性：观测来源（摄像头/LLM/人工……）的可靠度画像。
/// entitytree 与 stmb（voxelstore 观测管线仲裁）共享同一张表——
/// 制造业单库部署时两个系统指向同一数据库文件，来源画像一份数据两方共用。
struct SourceRecord {
    std::string source_id;
    double      reliability = 1.0;  // 0~1（与 stmb SourceRegistry 同形态）
    int64_t     updated_at = 0;     // 最近一次调整时间（秒）
    json        meta = json::object(); // stmb 侧来源元信息等
};

void to_json(json& j, const Observation& o);
void from_json(const json& j, Observation& o);
void to_json(json& j, const AttrProfile& p);
void from_json(const json& j, AttrProfile& p);
void to_json(json& j, const EntityNode& e);
void from_json(const json& j, EntityNode& e);
void to_json(json& j, const EntityBinding& b);
void from_json(const json& j, EntityBinding& b);
void to_json(json& j, const SourceRecord& s);
void from_json(const json& j, SourceRecord& s);

} // namespace entitytree
