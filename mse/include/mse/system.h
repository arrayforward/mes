#pragma once

// ============================================================================
// mse/system.h —— 系统装配:把各组件连成一台可运行的车间世界模型
//
// 依赖单向无环:事件写属性,属性被规则匹配,视图引用三者;
// storage 是所有访问的底层接口(RecordBackend 由调用方选择 memory/sqlite);
// voxel 管车间时空;knowledge/entity 只产出建议与候选,不碰状态。
// ============================================================================

#include <memory>
#include <string>

#include "mse/api.h"
#include "mse/dictionary.h"
#include "mse/entity_bridge.h"
#include "mse/event_log.h"
#include "mse/knowledge_assist.h"
#include "mse/model.h"
#include "mse/pipeline.h"
#include "mse/projection.h"
#include "mse/rules.h"
#include "mse/spacetime.h"
#include "mse/view_engine.h"

namespace storage { class RecordBackend; }
namespace voxelstore { class VoxelStore; }
namespace entity { class Resolver; }
namespace entitytree { class EntityStore; }

namespace mse {

class System {
public:
    /// backend:统一存储后端(所有持久化都走它)。
    /// voxel_store:时空记忆持久化(可空 = 纯内存)。
    /// 构造完成即:定义层 load() 完成、事件日志重放完成、投影与时空重建完成。
    explicit System(storage::RecordBackend& backend, voxelstore::VoxelStore* voxel_store = nullptr);
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    DefinitionLayer& defs();
    EventLog&        log();
    Projection&      projection();
    SpacetimeTree&   spacetime();
    WritePipeline&   pipeline();
    ViewEngine&      views();
    ApiGateway&      api();
    RejectionLog&    rejections();
    KnowledgeAssist& knowledge();

    /// entity 演化桥(可选):挂接 Resolver/EntityStore 后观测流可非人力创建本体。
    void attach_entity_bridge(entity::Resolver& resolver, entitytree::EntityStore& store);
    EntityEvolutionBridge* entity_bridge();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mse
