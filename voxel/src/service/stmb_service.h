// ============================================================================
// 文件: stmb_service.h
// 模块: stmb_service(服务门面,依赖 core / index / store / history)
// 用途: 声明 StmbService —— 对外统一门面,组合 BlockStore(数据)、
//       SpatialGridIndex(空间索引)、TemporalIndex(时间索引)、
//       VersionLog(版本历史)四个部件,并负责在写入 / 确认 / 变更 / 淘汰 /
//       删除 / 过期时保持它们之间的一致性。
// 设计思路:
//   1. 写入:自增生成 BlockId,由 AABB 中心点 + 时间戳计算 BlockKey,
//      先入 store(取回被淘汰块),再清理被淘汰块的两个索引,最后登记新块索引,
//      并把 v1 归档进版本历史;
//   2. 查询:空间索引粗筛候选 -> 时间索引求交集 -> store 取数据并刷新 LRU,
//      再用 AABB::intersects 与 TimeRange::contains 做精确过滤;
//      带 now 的重载在返回副本上套用置信度半衰期衰减(存储基准值不改写);
//   3. 状态机:confirm 推进 Pending->Stable 或 Changing->Stable(后者封存
//      旧版本、候选变更生效并归档新版本);reportChange 把 Stable 块转为
//      Changing 并暂存候选变更,当前生效数据不变;
//   4. 删除/淘汰/过期:清理 store 与两个索引,版本历史默认保留以支持回溯,
//      purgeHistory 提供手动清理;
//   5. 持久化:配置 dataDir 后,构造时从快照 + WAL 恢复状态;所有变更操作
//      成功后追加 WAL;checkpoint() 落全量快照并截断 WAL;LRU 淘汰不单独记
//      WAL(重放 put 序列按容量自然重现淘汰,属于可重建的派生行为);
//   6. 所有公开方法由一把互斥锁串行化(保持简单),store 内部锁不会与之死锁,
//      因为服务层不会在持锁期间回调外部代码。
// 架构角色: 系统最上层,是 app 与 tests 唯一直接使用的业务接口。
// ============================================================================
#pragma once

#include "block_store.h"
#include "dynamic_layer.h"
#include "persistence.h"
#include "shard_manager.h"
#include "spatial_grid.h"
#include "temporal_index.h"
#include "types.h"
#include "version_log.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace stmb {

// ----------------------------------------------------------------------------
// 服务配置:构造 StmbService 时一次性传入
// ----------------------------------------------------------------------------
struct ServiceConfig {
    std::size_t capacity        = 0;  // 存储容量上限(最多存放的块数)
    double      cellSize        = 1.0;  // 空间格子边长
    TimeStamp   timeSlotMs      = 1;  // 时间槽宽度(毫秒)
    double      confirmThreshold = 1.0; // Pending->Stable / 候选生效的加权确认阈值
    std::int64_t decayHalfLifeMs = 0; // 置信度半衰期(毫秒);0 表示不衰减
    std::string  dataDir;             // 持久化数据目录;空字符串 = 关闭持久化
    double      shardCellSize = 0.0;  // 空间分片边长;0 = 旧的单文件模式(默认)
    TimeStamp   shardTimeSpanMs = 0;  // 分片时间桶跨度(毫秒)
    std::size_t maxLoadedShards = 8;  // 内存驻留分片上限(超出按 LRU 换出)
    // LOD 层级格子尺寸(从粗到细,层级 0 最粗);空 = 单尺度(用 cellSize,向后兼容)
    std::vector<double> levelCellSizes;
    std::int64_t stationaryTimeoutMs = 0;  // 动态层:静止超时(毫秒)
    std::int64_t archiveTimeoutMs = 0;     // 动态层:归档超时;0 = 禁用动态层(默认)
    std::int64_t observationWindowMs = 0;  // 观察窗口(毫秒);0 = 候选不自动超时
};

// 服务运行统计:当前内存块数 / 容量上限 / 累计淘汰次数 / 已加载分片数
struct ServiceStats {
    std::size_t   blockCount = 0;
    std::size_t   capacity   = 0;
    std::uint64_t evictCount = 0;
    std::size_t   loadedShards = 0;  // 分片模式下为内存驻留分片数,否则为 0
};

// ----------------------------------------------------------------------------
// 变更报告:submitObservation 的返回载体
// ----------------------------------------------------------------------------
struct ChangeReport {
    ChangeType  type = ChangeType::Transient;  // 分类结果(无变化的一致观测填 Transient)
    bool        accepted = false;              // 是否被接受(生效/计入确认/关闭窗口)
    std::string message;                       // 说明(含空间一致性命中信息)
};

// ----------------------------------------------------------------------------
// 来源登记册:来源可靠性权重(多源仲裁用);未知来源默认 0.5
// ----------------------------------------------------------------------------
class SourceRegistry {
public:
    // 注册来源及可靠性(0~1)
    void registerSource(SourceId id, double reliability) {
        reliability_[id] = reliability;
    }
    // 查权重;未知来源返回默认 0.5
    double reliabilityOf(SourceId id) const {
        const auto it = reliability_.find(id);
        return it == reliability_.end() ? 0.5 : it->second;
    }
    // 全量条目(快照/manifest 落盘用)
    std::vector<std::pair<SourceId, double>> all() const {
        return {reliability_.begin(), reliability_.end()};
    }
private:
    std::unordered_map<SourceId, double> reliability_;
};

class StmbService {
public:
    // 构造:传入完整服务配置
    explicit StmbService(const ServiceConfig& config);

    // 便捷构造:只指定容量/格子/时间槽,确认阈值取 1、置信度不衰减
    StmbService(std::size_t capacity, double cellSize, TimeStamp timeSlotMs);

    // 写入块(调用方填 region / payload / timestamp / confidence,
    // id/key/版本/状态由服务填充);若触发 LRU 淘汰,返回被淘汰块的完整数据
    std::optional<MemoryBlock> put(MemoryBlock block);

    // 按「空间区域 + 时间范围」查询命中的记忆块(命中会刷新 LRU);
    // 缺省查询最细层级;返回块携带存储的基准置信度
    std::vector<MemoryBlock> query(const AABB& region, const TimeRange& range);

    // 同上,但返回副本中的 confidence 已按 now 做半衰期衰减(基准值不改写)
    std::vector<MemoryBlock> query(const AABB& region, const TimeRange& range,
                                   TimeStamp now);

    // ---- LOD(多尺度金字塔)查询 ----

    // 指定层级查询(level 越界自动夹取);LOD 模式下命中块的 hasFinerData
    // 字段指示其区域是否存在更细层级数据
    std::vector<MemoryBlock> queryLevel(const AABB& region, const TimeRange& range,
                                        int level);
    std::vector<MemoryBlock> queryLevel(const AABB& region, const TimeRange& range,
                                        TimeStamp now, int level);

    // 跨层级合并查询:各层级命中结果合并返回
    std::vector<MemoryBlock> queryAllLevels(const AABB& region, const TimeRange& range);

    // 「先粗后细」查询:先查最粗层级,对命中块(骨架/摘要)的覆盖区域
    // 逐级下钻更细层级,合并返回(按需细化的读取路径)
    std::vector<MemoryBlock> queryCoarseToFine(const AABB& region, const TimeRange& range);

    // 未细化检测:该区域是否存在比 level 更细层级的数据
    bool isRefined(const AABB& region, int level);

    // 聚合上卷:把 level+1 层级的块按本层格子分组聚合成摘要块
    // (payload 含子块计数/主要语义/平均置信度,confidence 取均值,
    //  state 取多数),返回生成的摘要块数;旧的同层摘要先清除
    std::size_t buildSummaries(int level);

    // 当前层级数(单尺度为 1)
    int levelCount() const;

    // 按 id 读取单个块(命中刷新 LRU);不带 now 返回基准置信度,
    // 带 now 返回衰减后的有效置信度(副本,不改写存储)
    std::optional<MemoryBlock> get(BlockId id);
    std::optional<MemoryBlock> get(BlockId id, TimeStamp now);

    // 确认:Pending 块累计确认,达到阈值转 Stable;
    // Changing 块确认变更:封存旧版本、候选变更生效、version+1 并归档新版本;
    // 块不存在或 Stable 块返回 false
    // 两参形式 = 系统自确认(权重 1.0);三参形式按来源可靠性加权
    bool confirm(BlockId id, TimeStamp now);
    bool confirm(BlockId id, SourceId source, TimeStamp now);

    // ---- 观测处理管线(变化分类 + 周期模式 + 多源仲裁)----

    // 提交观测:自动分类(Emergence/Vanishing/Mutation/Seasonal/Transient/
    // Conflict),按来源可靠性加权仲裁;未达阈值的候选挂起进入观察窗口
    ChangeReport submitObservation(BlockId id, const Observation& obs);

    // 注册 / 查询来源可靠性(未知来源默认 0.5;v6 起随快照/WAL 落盘,重启不丢)
    void registerSource(SourceId id, double reliability);
    double reliabilityOf(SourceId id) const;

    // 来源变更钩子(可选,默认空):registerSource 成功后回调
    // (source_id, reliability)。用于外部系统(如 entitytree et_sources 桥)
    // 同步来源画像;WAL 重放与水合路径不触发。
    void setSourceChangeHook(std::function<void(SourceId, double)> hook);

    // 注册 / 查询块的周期模式(随块持久化);模式冲突的新稳定值走 Mutation
    bool registerPattern(BlockId id, const PeriodicPattern& pattern);
    std::optional<PeriodicPattern> patternOf(BlockId id) const;

    // 观察窗口扫描:窗口超时仍未达阈值的挂起候选被丢弃(Transient/噪声),
    // 恢复窗口前状态;返回丢弃的候选数
    std::size_t sweepWindows(TimeStamp now);

    // 空间一致性校验(轻量):① 地面语义块上方紧邻为天空/空块 -> 冲突;
    // ② 非地面块下方无支撑 -> 标记 suspect(仅标记不拒绝);
    // 返回命中的规则说明列表;关键词集合可用 setGroundKeywords 配置
    std::vector<std::string> validateSpatial(BlockId id);
    void setGroundKeywords(std::vector<std::string> keywords);

    // 报告矛盾观测:Stable 块转 Changing,新 payload/confidence 暂存为候选
    // 变更,当前生效数据不变;非 Stable 块或不存在返回 false
    bool reportChange(BlockId id, std::string newPayload, double newConfidence,
                      TimeStamp now);

    // 版本历史:返回某块全部历史版本(版本升序) / 时刻 t 生效的版本
    std::vector<BlockVersion> historyOf(BlockId id) const;
    std::optional<BlockVersion> getAt(BlockId id, TimeStamp t) const;

    // 手动清理某块的版本历史(块消亡时历史默认保留以支持回溯)
    void purgeHistory(BlockId id);

    // 按 id 删除块(同步清理两个索引与存储,历史保留),返回是否删除成功
    bool remove(BlockId id);

    // 过期清理:删除时间戳严格小于 ts 的全部块(历史保留),返回被清理的 id 列表
    std::vector<BlockId> expireBefore(TimeStamp ts);

    // 返回当前统计信息
    ServiceStats stats() const;

    // 检查点:把当前内存状态(store 全部块 + 版本历史 + nextId)写成快照
    // 并截断 WAL;未开启持久化(dataDir 为空)返回 false
    bool checkpoint();

    // ---- 动静分离:动态实例轨迹层(archiveTimeoutMs > 0 时启用)----

    // 按 id 上报运动观测(id 传 0 自动分配);速度传零向量自动差分估计;
    // 返回实例 id;未启用动态层返回 0
    InstanceId reportMoving(InstanceId id, const std::string& classLabel,
                            const AABB& bounds, const std::array<double, 3>& velocity,
                            TimeStamp now, SourceId source);
    // 跨源关联上报:按「空间重叠 + 类别一致 + 时间接近」匹配既有实例,
    // 匹配不到才新建;返回实例 id
    InstanceId reportMoving(const std::string& classLabel, const AABB& bounds,
                            const std::array<double, 3>& velocity,
                            TimeStamp now, SourceId source);

    // 查询活跃动态实例(包围盒相交且最新观测落在时间范围)
    std::vector<DynamicInstance> queryDynamic(const AABB& region,
                                              const TimeRange& range);

    // 实例轨迹回溯(含已归档);不存在返回空
    std::vector<TrackPoint> trajectoryOf(InstanceId id) const;

    // 推进动态层状态(静止/归档超时判定)
    void updateDynamic(TimeStamp now);

    // 沉淀:把 Stationary 实例转为静态层 MemoryBlock(payload=类别,
    // state=Pending 走确认流程),实例本身归档;返回新建的静态块 id 列表
    std::vector<BlockId> promoteStationary(TimeStamp now);

    // 动态层统计(活跃/静止/归档)
    DynamicStats dynamicStats() const;

private:
    // 对查询结果副本套用置信度衰减(decayHalfLifeMs 为 0 时原样返回)
    MemoryBlock applyDecay(MemoryBlock block, TimeStamp now) const;

    // 持久化:构造时从 dataDir 恢复快照并重放 WAL(快照损坏则抛异常)
    void restore();
    // 内部变更路径(不写 WAL,供公开方法与 WAL 重放共用)
    std::optional<MemoryBlock> applyPut(const MemoryBlock& block);
    bool applyConfirm(BlockId id, TimeStamp now);
    bool applyReportChange(BlockId id, std::string newPayload,
                           double newConfidence, TimeStamp now);
    bool applyRemove(BlockId id);
    std::vector<BlockId> applyExpireBefore(TimeStamp ts);
    // 追加一条 WAL 记录(未开启持久化时为空操作;失败仅忽略,尽力而为)
    void logWal(const WalRecord& record);

    // ---- 分片模式(shardCellSize > 0 且 dataDir 非空时启用)----
    bool sharded() const;  // 是否处于分片模式
    // 分片回调:数据注入内存 / 内存现状与盘上旧数据归并 / 从内存移除
    void onShardLoad(const ShardKey& key, const ShardData& data);
    ShardData onShardMerge(const ShardKey& key, ShardData old);
    void onShardUnload(const ShardKey& key);
    // 处理全局 LRU 淘汰块:清索引;分片模式下标记下沉与脏
    void handleEvicted(const MemoryBlock& evicted);
    // 按 id 定位分片并确保加载;分片不存在返回 false
    bool ensureLoadedForId(BlockId id);
    // query 前按「扩边区域 x 时间桶」计算触达分片,逐一加载并钉住,
    // 返回已钉住的分片键列表(查询结束后由调用方解除,防加载窗口期抖动)
    std::vector<ShardKey> loadTouchedShards(const AABB& region, const TimeRange& range,
                                            int level);
    // 分片模式恢复:manifest + WAL 重放
    void restoreSharded();

    // ---- LOD 内部辅助 ----
    bool lodMode() const;                    // 是否启用多尺度( levelCellSizes 非空)
    int clampLevel(int level) const;         // 层级夹取到 [0, levelCount-1]
    double cellSizeOf(int level) const;      // 某层级的格子边长
    SpatialGridIndex& spatialOf(int level);  // 某层级的空间索引
    int detectLevel(const AABB& region) const; // 按 AABB 尺寸匹配最近层级
    // 判定区域内是否存在比 fromLevel 更细层级的数据(调用方须持锁)
    bool hasFinerData(const AABB& region, int fromLevel);

    // put 的内部实现(调用方须持锁),供公开 put 与 promoteStationary 共用
    std::optional<MemoryBlock> putInternal(MemoryBlock block);

    // ---- 观测管线内部辅助(调用方须持锁)----
    // 加权确认的内部实现(weight:系统自确认 1.0 / 来源可靠性)
    bool applyConfirmWeighted(BlockId id, TimeStamp now, double weight);
    // confirm 的锁内实现(含 WAL 记录)
    bool confirmLocked(BlockId id, TimeStamp now, double weight);
    // submitObservation 的锁内实现(公开方法与 WAL 重放共用)
    ChangeReport submitObservationLocked(BlockId id, const Observation& obs);
    // 来源注册/变更的内部实现(不写 WAL、不触发钩子,供重放与水合共用)
    void applyRegisterSource(SourceId id, double reliability);
    // 让候选变更立即生效(封存旧版本 + 归档新版本),返回是否为 Emergence
    void applyCandidate(MemoryBlock& block, TimeStamp now);
    // 空间一致性校验的锁内实现
    std::vector<std::string> validateSpatialLocked(BlockId id);
    // 分片模式下按块 id 标脏所属分片
    void markDirtyOf(BlockId id);

    BlockStore        store_;      // 数据唯一存放点(自带内部锁)
    SpatialGridIndex  spatial_;    // 空间索引(单尺度 / 兼容路径使用)
    TemporalIndex     temporal_;   // 时间索引(只存 BlockId)
    VersionLog        history_;    // 版本历史(块消亡后默认保留)
    // LOD:每层级一套空间索引(仅 lodMode 时使用,spatial_ 闲置)
    std::vector<SpatialGridIndex> spatialByLevel_;
    // 各层级当前摘要块 id(buildSummaries 重建时先清除旧摘要)
    std::unordered_map<int, std::vector<BlockId>> summaryIds_;
    ServiceConfig     config_;     // 服务配置
    BlockId           nextId_ = 1; // 自增 id 发生器(从 1 开始)
    double            maxBlockRadius_ = 0.0; // 历史最大块半径(分片查询扩边用)
    std::unique_ptr<ShardManager> shardManager_; // 分片管理器(仅分片模式非空)
    std::unique_ptr<DynamicLayer> dynamic_;      // 动态实例层(仅配置超时后非空)
    SourceRegistry sources_;         // 来源可靠性登记册(多源仲裁)
    std::function<void(SourceId, double)> sourceChangeHook_;  // 来源变更钩子(可空)
    std::vector<std::string> groundKeywords_{"road", "floor", "ground"};  // 地面语义关键词
    mutable std::mutex mutex_;     // 服务层统一互斥锁
};

}  // namespace stmb
