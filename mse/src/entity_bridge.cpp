// ============================================================================
// mse/entity_bridge.cpp —— entity 演化桥:非人力的实体创建(系统演化)
//
// 观测 → Resolver 浮现/合并 → 读回浮现结果 → EmergedEntity → 回调。
// 未达阈值/合并进既有实体/存疑都不是"新实体",返回空;任何存储异常
// (如 get_profile 抛错)同样返回空——桥不因为自己的失败阻断观测流。
// ============================================================================

#include "mse/entity_bridge.h"

#include "entity/resolver.h"
#include "entitytree/entity_store.h"
#include "entitytree/model.h"

namespace mse {

EntityEvolutionBridge::EntityEvolutionBridge(entity::Resolver& resolver,
                                             entitytree::EntityStore& store)
    : resolver_(resolver), store_(store) {}

void EntityEvolutionBridge::set_emergence_callback(
    std::function<void(const EmergedEntity&)> cb) {
    on_emerge_ = std::move(cb);
}

void EntityEvolutionBridge::mark_known(const std::string& entity_id) {
    known_.insert(entity_id);
}

std::vector<EmergedEntity> EntityEvolutionBridge::ingest_observation(
    const entitytree::Observation& obs) {
    std::vector<EmergedEntity> out;
    try {
        // Resolver 内部达阈值会自动 resolve;未达阈值时侧写仍在 pool,不算新实体。
        const std::string profile_id = resolver_.ingest(obs);

        const entitytree::AttrProfile profile = store_.get_profile(profile_id);
        if (profile.status != "emerged" && profile.status != "linked") return out;

        const auto binding = store_.effective_binding(profile_id);
        if (!binding.has_value()) return out;
        const std::string& entity_id = binding->entity_id;
        if (entity_id.empty() || known_.count(entity_id) != 0) return out;

        // 新实体浮现:读回归并视图。merged view 形态 {key: [{value, weight}...]},
        // 取每键第一个 value 转成 json 字符串值(观测值本就是文本化的)。
        const entitytree::EntityNode node = store_.get_entity(entity_id);
        EmergedEntity e;
        e.entity_id = entity_id;
        e.anchor = profile.anchor_ref;
        for (const auto& [key, arr] : node.attributes.items()) {
            if (!arr.is_array() || arr.empty()) continue;
            const json& first = arr.front();
            if (!first.is_object() || !first.contains("value")) continue;
            const json& v = first["value"];
            e.attributes[key] = v.is_string() ? v : json(v.dump());
        }

        known_.insert(entity_id);
        if (on_emerge_) on_emerge_(e);
        out.push_back(std::move(e));
    } catch (const std::exception&) {
        return {};
    }
    return out;
}

} // namespace mse
