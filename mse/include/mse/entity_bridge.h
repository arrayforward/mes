#pragma once

// ============================================================================
// mse/entity_bridge.h —— entity 演化桥:非人力的实体创建(系统演化)
//
// 用户约定:"entity 是协助非人力创建新的实体,进行系统演化用的"。
//
// 机制:适配器把物理信号(RFID 过点/扫码/传感器)文本化为 entitytree
// 观测喂给 entity::Resolver;Resolver 自动浮现→合并/新建/存疑。当浮现出
// 一个系统从未见过的实体时,桥把它翻译成一条候选变化描述(首个携带该 id
// 键的事件 = 本体诞生,系统无独立 create),经回调交给写侧管线——四层校验
// 依然是唯一的结算入口,AI/算法产物同样不碰状态。
// ============================================================================

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "mse/model.h"

namespace entity { class Resolver; }
namespace entitytree { class EntityStore; struct Observation; }

namespace mse {

/// 浮现实体:Resolver 从观测流中新归并出的实体(非人力创建)。
struct EmergedEntity {
    std::string entity_id;
    std::string anchor;                    // 浮现位置的锚点
    std::map<std::string, json> attributes; // 归并出的属性(文本化观测值)
};

class EntityEvolutionBridge {
public:
    /// resolver 与 store 生命周期须长于本对象(store 用于读回浮现结果)。
    EntityEvolutionBridge(entity::Resolver& resolver, entitytree::EntityStore& store);

    /// 新实体浮现时的回调:通常把 EmergedEntity 翻译成 Candidate 提交写侧管线。
    void set_emergence_callback(std::function<void(const EmergedEntity&)> cb);

    /// 喂入一条观测。返回本次新浮现的实体(可能为空:合并进既有实体/存疑/
    /// 未达浮现阈值都不是"新实体")。
    std::vector<EmergedEntity> ingest_observation(const entitytree::Observation& obs);

    /// 标记某实体 id 系统已知(如事件树中已有该 id 的本体),避免重复上报。
    void mark_known(const std::string& entity_id);

private:
    entity::Resolver&       resolver_;
    entitytree::EntityStore& store_;
    std::set<std::string>   known_;
    std::function<void(const EmergedEntity&)> on_emerge_;
};

} // namespace mse
