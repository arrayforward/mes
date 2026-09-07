// ============================================================================
// mse/system.cpp —— 系统装配:把各组件连成一台可运行的车间世界模型
//
// 构造即完成:定义层 load() → 事件日志重放 → 投影与时空重建 → 拦截记录挂接。
// entity 演化桥可选:浮现实体翻译成 EntityObserved 候选回到写侧管线——
// 四层校验依然是唯一结算入口,被拒也无妨(进 RejectionLog)。
// ============================================================================

#include "mse/system.h"

#include <exception>

#include "entity/resolver.h"
#include "entitytree/entity_store.h"

namespace mse {

struct System::Impl {
    // 成员声明顺序即构造顺序(依赖单向无环)
    storage::RecordBackend& backend_;
    SnapshotStore           snapshots_;  // 定距快照库(同一 backend;崩溃恢复基座)
    DefinitionLayer         defs_;
    EventLog                log_;
    Projection              proj_;
    SpacetimeTree           spacetime_;
    RuleEngine              rules_;
    RejectionLog            rejections_;
    WritePipeline           pipeline_;
    ViewEngine              views_;
    ApiGateway              api_;
    KnowledgeAssist         knowledge_;
    std::unique_ptr<EntityEvolutionBridge> entity_bridge_;

    explicit Impl(storage::RecordBackend& backend, voxelstore::VoxelStore* voxel_store)
        : backend_(backend),
          snapshots_(backend),
          defs_(backend),
          log_(backend),
          spacetime_(defs_, voxel_store),
          pipeline_(defs_, log_, proj_, spacetime_, rules_),
          views_(defs_, proj_, log_, rules_, spacetime_),
          api_(pipeline_, views_, defs_) {
        defs_.load();
        // 事件日志已有事件:投影与时空记忆重建(投影可丢弃可重建)。
        // 投影优先走 最近快照 + 增量重放(快照加载失败退化为全量重放);
        // 派生钩子由 pipeline_ 构造时挂接,快照/重放/在线同一条 fold 路径。
        // 时空记忆保持全量重放。
        if (log_.size() > 0) {
            bool restored = false;
            if (auto snap = snapshots_.latest()) {
                try {
                    proj_.load_snapshot(snap->second);
                    for (const Event& e : log_.range(snap->first + 1, log_.size()))
                        proj_.apply(e);
                    restored = true;
                } catch (const std::exception&) {
                    proj_.clear();  // 快照损坏:退化为全量重放
                }
            }
            if (!restored) proj_.rebuild(log_);
            spacetime_.replay(log_);
        }
        pipeline_.set_rejection_log(&rejections_);
        pipeline_.set_snapshots(&snapshots_, 0);        // 默认关闭,调用方可开
        pipeline_.set_idempotency_store(&backend_);     // 幂等键跨进程重启有效
        views_.set_rejection_log(&rejections_);
        views_.set_snapshot_store(&snapshots_);         // AS OF 快照加速
        // AS OF 重放口径与在线一致(含派生属性):经写侧管线同一 fold 路径
        views_.set_replay_provider([this](Projection& p, int64_t from, int64_t to) {
            pipeline_.replay_range(p, log_, from, to);
        });
    }
};

System::System(storage::RecordBackend& backend, voxelstore::VoxelStore* voxel_store)
    : impl_(std::make_unique<Impl>(backend, voxel_store)) {}

System::~System() = default;

DefinitionLayer& System::defs()          { return impl_->defs_; }
EventLog&        System::log()           { return impl_->log_; }
Projection&      System::projection()    { return impl_->proj_; }
SpacetimeTree&   System::spacetime()     { return impl_->spacetime_; }
WritePipeline&   System::pipeline()      { return impl_->pipeline_; }
ViewEngine&      System::views()         { return impl_->views_; }
ApiGateway&      System::api()           { return impl_->api_; }
RejectionLog&    System::rejections()    { return impl_->rejections_; }
KnowledgeAssist& System::knowledge()     { return impl_->knowledge_; }

void System::attach_entity_bridge(entity::Resolver& resolver,
                                  entitytree::EntityStore& store) {
    impl_->entity_bridge_ = std::make_unique<EntityEvolutionBridge>(resolver, store);
    impl_->entity_bridge_->set_emergence_callback([this](const EmergedEntity& e) {
        // 浮现实体 → EntityObserved 候选(首个携带该 id 键的事件 = 本体诞生)。
        // 仍走写侧管线四层校验:被拒无妨,进 RejectionLog。
        Candidate c;
        c.type = "EntityObserved";
        c.actor = "entity-resolver";
        c.writes[e.entity_id] = e.attributes;
        if (!e.anchor.empty()) c.space = SpaceRef::anchor_ref(e.anchor);
        c.occur_time = "";
        (void)impl_->pipeline_.submit(c);
    });
}

EntityEvolutionBridge* System::entity_bridge() { return impl_->entity_bridge_.get(); }

} // namespace mse
