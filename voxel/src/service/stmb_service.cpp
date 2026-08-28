// ============================================================================
// 文件: stmb_service.cpp
// 模块: stmb_service(服务门面,依赖 core / index / store / history / persistence)
// 用途: 实现 StmbService 的写入、联合查询、状态机、置信度衰减、版本历史、
//       删除、过期清理、统计与本地持久化(快照 + WAL)。
// 设计思路:
//   1. 一致性是本文件的核心:任何让 store 失去一个块的路径(put 触发淘汰、
//      remove、expireBefore)都必须同步把该块从空间索引和时间索引中摘除,
//      因此所有公开方法都在服务层互斥锁下按固定顺序操作各部件;
//   2. 每个变更操作拆成「公开方法 = 内部 apply(不写日志)+ 追加 WAL」两层,
//      WAL 重放直接复用 apply 路径,避免重复写日志,也保证正常执行与恢复
//      执行的是同一份逻辑;
//   3. 恢复流程:loadSnapshot 重建 store / 索引 / 版本历史与 nextId,
//      再 replayWal 依次 apply 快照之后的操作;LRU 淘汰不单独记 WAL ——
//      重放 put 序列在相同容量下自然重现淘汰结果(可重建的派生行为);
//   4. 查询采用「粗筛 -> 交集 -> 精过滤」三级漏斗,只有最终命中的块才通过
//      store_.get 刷新 LRU;带 now 的重载在最后对返回副本套用衰减;
//   5. BlockId 从 1 开始自增,0 保留为「无效 id」语义。
// 架构角色: StmbService 的唯一实现文件。
// ============================================================================
#include "stmb_service.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace stmb {

// 伪代码:
//   1. 用 config.capacity 构造 store_,用 config.cellSize 构造 spatial_;
//   2. 保存完整配置到 config_;
//   3. 分片模式(shardCellSize>0 且 dataDir 非空):创建 ShardManager 并绑定
//      三个协作回调(数据注入 / 归并收集 / 内存移除);
//   4. 若配置了 dataDir,按模式选择 restore / restoreSharded 恢复状态
//      (此时尚无并发,无需加锁)。
StmbService::StmbService(const ServiceConfig& config)
    : store_(config.capacity), spatial_(config.cellSize), config_(config) {
    if (lodMode()) {
        // LOD 模式:为每个层级建立一套独立的空间索引(各自的格子边长)
        spatialByLevel_.reserve(config_.levelCellSizes.size());
        for (double cell : config_.levelCellSizes) {
            spatialByLevel_.emplace_back(cell);
        }
    }
    if (sharded()) {
        ShardManager::Callbacks cbs;
        cbs.onLoad = [this](const ShardKey& key, const ShardData& data) {
            onShardLoad(key, data);
        };
        cbs.onMerge = [this](const ShardKey& key, ShardData old) {
            return onShardMerge(key, std::move(old));
        };
        cbs.onUnload = [this](const ShardKey& key) { onShardUnload(key); };
        shardManager_ = std::make_unique<ShardManager>(
            config_.dataDir, config_.maxLoadedShards, std::move(cbs));
    }
    if (config_.archiveTimeoutMs > 0) {
        // 动静分离:配置归档超时即启用动态实例轨迹层
        dynamic_ = std::make_unique<DynamicLayer>(config_.stationaryTimeoutMs,
                                                  config_.archiveTimeoutMs);
    }
    if (!config_.dataDir.empty()) {
        if (sharded()) {
            restoreSharded();
        } else {
            restore();
        }
    }
}

// 伪代码:
//   1. 把三个参数包成 ServiceConfig(确认阈值默认 1、半衰期默认 0、
//      dataDir 为空即关闭持久化、分片参数默认 0 即单文件模式);
//   2. 委托给主构造函数。
StmbService::StmbService(std::size_t capacity, double cellSize, TimeStamp timeSlotMs)
    : StmbService(ServiceConfig{capacity, cellSize, timeSlotMs, 1, 0, "",
                                0.0, 0, 0, {}, 0, 0, 0}) {}

// 伪代码:
//   1. 加锁;
//   2. 为新块分配自增 id,生成 BlockKey(区域中心格子 + 时间槽),
//      填充版本号(v1)、初始状态(Pending)、确认计数(0)与 lastUpdate;
//   3. 调用 applyPut 落 store/索引/版本历史,取回可能被淘汰的块;
//   4. 追加 PutBlock 型 WAL 记录(含填充后的完整块);
//   5. 返回被淘汰块(或空)。
// 伪代码:
//   1. 加锁,委托 putInternal(锁内执行完整写入路径)。
std::optional<MemoryBlock> StmbService::put(MemoryBlock block) {
    std::lock_guard<std::mutex> lock(mutex_);
    return putInternal(std::move(block));
}

// 伪代码(锁内写入路径,公开 put 与 promoteStationary 共用):
//   1. 分配自增 id;LOD 层级解析(显式夹取 / 自动匹配);按层级格子边长
//      生成 BlockKey;填充版本/状态/确认计数/时间字段;
//   2. 分片模式:按层级计算分片键,登记清单并确保加载;
//   3. applyPut 落 store/索引/版本历史;追加 PutBlock 型 WAL 记录;
//   4. 返回被淘汰块(或空)。
std::optional<MemoryBlock> StmbService::putInternal(MemoryBlock block) {
    block.id = nextId_++;
    // LOD:显式指定的层级越界则夹取;未指定(-1)按 AABB 尺寸自动匹配最近层级
    if (block.level < 0) {
        block.level = detectLevel(block.region);
    }
    block.level = clampLevel(block.level);
    block.key = makeBlockKey(block.region, block.timestamp,
                             cellSizeOf(block.level), config_.timeSlotMs);
    block.version = 1;
    block.state = BlockState::Pending;
    block.confirmations = 0;
    block.lastUpdate = block.timestamp;
    block.lastAccess = block.timestamp;
    block.pendingPayload.reset();
    block.pendingConfidence.reset();

    WalRecord rec;
    rec.type = RecordType::PutBlock;
    rec.block = block;
    if (sharded()) {
        // 分片模式:按所属层级计算分片键(含 level 维度),登记清单并确保加载
        const ShardKey key = makeShardKey(block.region, block.timestamp,
                                          config_.shardCellSize,
                                          config_.shardTimeSpanMs, block.level);
        shardManager_->addKnown(key);
        shardManager_->ensureLoaded(key);
        rec.hasShardKey = true;
        rec.shard = key;
    }
    std::optional<MemoryBlock> evicted = applyPut(block);
    logWal(rec);
    return evicted;
}

// 伪代码:
//   1. 委托 queryLevel,层级取最细层级(levelCount-1;单尺度为 0)。
std::vector<MemoryBlock> StmbService::query(const AABB& region, const TimeRange& range) {
    return queryLevel(region, range, levelCount() - 1);
}

// 伪代码:
//   1. 委托带 now 的 queryLevel(最细层级),返回衰减后的有效置信度副本。
std::vector<MemoryBlock> StmbService::query(const AABB& region, const TimeRange& range,
                                            TimeStamp now) {
    return queryLevel(region, range, now, levelCount() - 1);
}

// 伪代码:
//   1. 加锁,层级夹取;
//   2. 分片模式:按「扩边区域 x 时间桶 x 层级」加载并钉住触达分片;
//   3. 空间索引粗筛(该层级的索引)-> 时间索引交集 -> store 取数据刷新 LRU
//      -> AABB::intersects 与 TimeRange::contains 精过滤;
//   4. LOD 模式:为命中块填充 hasFinerData(区域内是否存在更细层级数据);
//   5. 解除钉住并收缩驻留,返回结果(基准置信度)。
std::vector<MemoryBlock> StmbService::queryLevel(const AABB& region,
                                                 const TimeRange& range, int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    level = clampLevel(level);
    std::vector<ShardKey> pinned;
    if (sharded()) {
        pinned = loadTouchedShards(region, range, level);
    }
    const std::unordered_set<BlockId> candidates =
        spatialOf(level).queryRegion(region);
    const std::vector<BlockId> inRange = temporal_.queryRange(range);
    const std::unordered_set<BlockId> timeSet(inRange.begin(), inRange.end());

    std::vector<MemoryBlock> result;
    for (BlockId id : candidates) {
        if (timeSet.find(id) == timeSet.end()) {
            continue;
        }
        std::optional<MemoryBlock> block = store_.get(id);
        if (!block.has_value()) {
            continue;
        }
        if (block->region.intersects(region) && range.contains(block->timestamp)) {
            if (lodMode()) {
                block->hasFinerData = hasFinerData(block->region, block->level);
            }
            if (dynamic_) {
                // 临时占用标记:与块区域相交的活跃动态实例(查询期计算,不落盘)
                block->temporarilyOccupiedBy = dynamic_->occupantsOf(block->region);
            }
            result.push_back(*block);
        }
    }
    for (const ShardKey& key : pinned) {
        shardManager_->unpin(key);
    }
    if (sharded()) {
        shardManager_->trimToLimit();
    }
    return result;
}

// 伪代码:
//   1. 调用不带 now 的 queryLevel 取得命中块;
//   2. 对每个返回副本用 applyDecay 替换为 now 时刻有效置信度;
//   3. 返回结果(存储基准值不改写)。
std::vector<MemoryBlock> StmbService::queryLevel(const AABB& region,
                                                 const TimeRange& range,
                                                 TimeStamp now, int level) {
    std::vector<MemoryBlock> result = queryLevel(region, range, level);
    for (MemoryBlock& block : result) {
        block = applyDecay(std::move(block), now);
    }
    return result;
}

// 伪代码:
//   1. 从最粗到最细逐层调用 queryLevel;
//   2. 把各层命中结果依次合并到一个列表返回(各层索引互斥,无需去重)。
std::vector<MemoryBlock> StmbService::queryAllLevels(const AABB& region,
                                                     const TimeRange& range) {
    std::vector<MemoryBlock> result;
    for (int level = 0; level < levelCount(); ++level) {
        std::vector<MemoryBlock> hits = queryLevel(region, range, level);
        result.insert(result.end(), hits.begin(), hits.end());
    }
    return result;
}

// 伪代码:
//   1. 先查最粗层级(level 0)得到骨架/摘要命中;
//   2. 对每个命中块:求其区域与查询区域的交集,在更细层级(1..L-1)逐级
//      下钻查询该子区域,把命中合并进来;
//   3. 返回「粗层命中 + 命中最区域下的细层数据」的合并列表。
std::vector<MemoryBlock> StmbService::queryCoarseToFine(const AABB& region,
                                                        const TimeRange& range) {
    std::vector<MemoryBlock> result = queryLevel(region, range, 0);
    const std::vector<MemoryBlock> coarse = result;
    for (const MemoryBlock& hit : coarse) {
        AABB sub;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            sub.min[axis] = std::max(region.min[axis], hit.region.min[axis]);
            sub.max[axis] = std::min(region.max[axis], hit.region.max[axis]);
        }
        for (int level = 1; level < levelCount(); ++level) {
            std::vector<MemoryBlock> finer = queryLevel(sub, range, level);
            result.insert(result.end(), finer.begin(), finer.end());
        }
    }
    return result;
}

// 伪代码:
//   1. 加锁,直接调用 hasFinerData(区域 + 指定层级)。
bool StmbService::isRefined(const AABB& region, int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasFinerData(region, clampLevel(level));
}

// 伪代码(聚合上卷):
//   1. 加锁;仅 LOD 模式且 level 非最细层时工作,否则返回 0;
//   2. 清除该层级旧摘要块(删数据 + 清历史 + 分片路由移除);
//   3. 分片模式:先加载 level+1 层的全部已知分片(上卷需看到全量细层数据);
//   4. 收集 level+1 层的非摘要块,按本层格子(中心/本层 cellSize)分组;
//   5. 每组生成一个摘要块:区域 = 粗格子包围盒,payload =
//      "N 个子块, 主要语义 X, 平均置信度 Y",confidence 取均值,state 取
//      多数(并列取枚举序小者),timestamp 取子块最大时间戳,标记
//      isSummary/sourceLevel,走 applyPut 落 store/索引/历史 + WAL;
//   6. 记录摘要 id 到 summaryIds_,返回生成数量。
std::size_t StmbService::buildSummaries(int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!lodMode() || level < 0 || level >= levelCount() - 1) {
        return 0;
    }
    for (BlockId oldId : summaryIds_[level]) {
        applyRemove(oldId);
        history_.drop(oldId);
        if (sharded()) {
            shardManager_->untrack(oldId);
        }
    }
    summaryIds_[level].clear();

    if (sharded()) {
        for (const ShardKey& key : shardManager_->knownShards()) {
            if (key.level == level + 1) {
                shardManager_->ensureLoaded(key);
            }
        }
    }

    const int childLevel = level + 1;
    std::unordered_map<CellCoord, std::vector<MemoryBlock>, CellCoordHash> groups;
    for (const MemoryBlock& b : store_.blocks()) {
        if (b.level != childLevel || b.isSummary) {
            continue;
        }
        const std::array<double, 3> c = b.region.center();
        const double coarseCell = cellSizeOf(level);
        CellCoord cell{
            static_cast<std::int64_t>(std::floor(c[0] / coarseCell)),
            static_cast<std::int64_t>(std::floor(c[1] / coarseCell)),
            static_cast<std::int64_t>(std::floor(c[2] / coarseCell))};
        groups[cell].push_back(b);
    }

    for (const auto& [cell, children] : groups) {
        // 统计:计数 / 均值置信度 / 多数负载 / 多数状态 / 最大时间戳
        double confSum = 0.0;
        TimeStamp maxTs = children.front().timestamp;
        std::unordered_map<std::string, int> payloadVotes;
        std::unordered_map<int, int> stateVotes;
        for (const MemoryBlock& child : children) {
            confSum += child.confidence;
            maxTs = std::max(maxTs, child.timestamp);
            ++payloadVotes[child.payload];
            ++stateVotes[static_cast<int>(child.state)];
        }
        std::string majorityPayload;
        int bestPayloadVotes = -1;
        for (const auto& [p, n] : payloadVotes) {
            if (n > bestPayloadVotes) {
                bestPayloadVotes = n;
                majorityPayload = p;
            }
        }
        BlockState majorityState = BlockState::Pending;
        int bestStateVotes = -1;
        for (int s = 0; s <= 2; ++s) {
            if (stateVotes[s] > bestStateVotes) {
                bestStateVotes = stateVotes[s];
                majorityState = static_cast<BlockState>(s);
            }
        }

        const double coarseCell = cellSizeOf(level);
        MemoryBlock summary;
        summary.region.min = {static_cast<double>(cell.x) * coarseCell,
                              static_cast<double>(cell.y) * coarseCell,
                              static_cast<double>(cell.z) * coarseCell};
        summary.region.max = {summary.region.min[0] + coarseCell,
                              summary.region.min[1] + coarseCell,
                              summary.region.min[2] + coarseCell};
        summary.timestamp = maxTs;
        summary.confidence = confSum / static_cast<double>(children.size());
        summary.state = majorityState;
        char text[256];
        std::snprintf(text, sizeof(text), "%zu 个子块, 主要语义 %s, 平均置信度 %.2f",
                      children.size(), majorityPayload.c_str(), summary.confidence);
        summary.payload = text;
        summary.id = nextId_++;
        summary.level = level;
        summary.isSummary = true;
        summary.sourceLevel = childLevel;
        summary.key = makeBlockKey(summary.region, summary.timestamp,
                                   coarseCell, config_.timeSlotMs);
        summary.version = 1;
        summary.confirmations = 0;
        summary.lastUpdate = maxTs;
        summary.lastAccess = maxTs;

        WalRecord rec;
        rec.type = RecordType::PutBlock;
        rec.block = summary;
        if (sharded()) {
            const ShardKey key = makeShardKey(summary.region, summary.timestamp,
                                              config_.shardCellSize,
                                              config_.shardTimeSpanMs, level);
            shardManager_->addKnown(key);
            shardManager_->ensureLoaded(key);
            rec.hasShardKey = true;
            rec.shard = key;
        }
        applyPut(summary);
        logWal(rec);
        summaryIds_[level].push_back(summary.id);
    }
    return groups.size();
}

// 伪代码:
//   1. LOD 模式返回 levelCellSizes 的大小;单尺度返回 1。
int StmbService::levelCount() const {
    return lodMode() ? static_cast<int>(config_.levelCellSizes.size()) : 1;
}

// 伪代码:
//   1. 加锁;分片模式下先按 id 定位分片并确保加载(块可能已被换出);
//   2. 调用 store_.get(命中即刷新 LRU),返回块副本(基准置信度)。
std::optional<MemoryBlock> StmbService::get(BlockId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sharded() && !ensureLoadedForId(id)) {
        return std::nullopt;
    }
    return store_.get(id);
}

// 伪代码:
//   1. 调用不带 now 的 get 取得块副本;
//   2. 命中则用 applyDecay 把 confidence 替换为 now 时刻的有效值后返回。
std::optional<MemoryBlock> StmbService::get(BlockId id, TimeStamp now) {
    std::optional<MemoryBlock> block = get(id);
    if (block.has_value()) {
        block = applyDecay(std::move(*block), now);
    }
    return block;
}

// 伪代码:
//   1. 加锁,调用 applyConfirm 执行状态机迁移;
//   2. 迁移成功(返回 true)则追加 Confirm 型 WAL 记录;
//   3. 返回迁移结果。
// 伪代码:
//   1. 系统自确认:权重 1.0,走锁内路径。
bool StmbService::confirm(BlockId id, TimeStamp now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return confirmLocked(id, now, 1.0);
}

// 伪代码:
//   1. 按来源可靠性取权重(未知来源 0.5),走锁内路径。
bool StmbService::confirm(BlockId id, SourceId source, TimeStamp now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return confirmLocked(id, now, sources_.reliabilityOf(source));
}

// 伪代码:
//   1. 分片模式下先按 id 定位分片并确保加载;
//   2. applyConfirmWeighted 执行加权状态机迁移;
//   3. 迁移成功则追加 Confirm 型 WAL 记录(携带权重,重放精确还原);
//   4. 返回迁移结果。
bool StmbService::confirmLocked(BlockId id, TimeStamp now, double weight) {
    if (sharded() && !ensureLoadedForId(id)) {
        return false;
    }
    const bool ok = applyConfirmWeighted(id, now, weight);
    if (ok) {
        WalRecord rec;
        rec.type = RecordType::Confirm;
        rec.id = id;
        rec.ts = now;
        rec.weight = weight;
        if (sharded()) {
            rec.hasShardKey = true;
            rec.shard = *shardManager_->shardOf(id);
        }
        logWal(rec);
    }
    return ok;
}

// 伪代码:
//   1. 加锁,调用 applyReportChange 暂存候选变更并转 Changing;
//   2. 成功则追加 ReportChange 型 WAL 记录;
//   3. 返回结果。
bool StmbService::reportChange(BlockId id, std::string newPayload,
                               double newConfidence, TimeStamp now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sharded() && !ensureLoadedForId(id)) {
        return false;
    }
    const bool ok = applyReportChange(id, newPayload, newConfidence, now);
    if (ok) {
        WalRecord rec;
        rec.type = RecordType::ReportChange;
        rec.id = id;
        rec.payload = std::move(newPayload);
        rec.confidence = newConfidence;
        rec.ts = now;
        if (sharded()) {
            rec.hasShardKey = true;
            rec.shard = *shardManager_->shardOf(id);
        }
        logWal(rec);
    }
    return ok;
}

// 伪代码:
//   1. 加锁;分片模式下先确保该块所在分片已加载(版本历史随分片驻留内存);
//   2. 转发给 VersionLog::historyOf。
std::vector<BlockVersion> StmbService::historyOf(BlockId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sharded() && shardManager_->shardOf(id).has_value()) {
        shardManager_->ensureLoaded(*shardManager_->shardOf(id));
    }
    return history_.historyOf(id);
}

// 伪代码:
//   1. 加锁;分片模式下先确保该块所在分片已加载;
//   2. 转发给 VersionLog::getAt(时间回溯)。
std::optional<BlockVersion> StmbService::getAt(BlockId id, TimeStamp t) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sharded() && shardManager_->shardOf(id).has_value()) {
        shardManager_->ensureLoaded(*shardManager_->shardOf(id));
    }
    return history_.getAt(id, t);
}

// 伪代码:
//   1. 加锁;分片模式下:定位分片并确保加载(盘上版本记录需要随之清除),
//      从 VersionLog 清除历史、从路由目录移除该 id、标记分片为脏;
//   2. 非分片模式:仅清除内存中的版本历史。
void StmbService::purgeHistory(BlockId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sharded()) {
        const std::optional<ShardKey> key = shardManager_->shardOf(id);
        if (key.has_value()) {
            shardManager_->ensureLoaded(*key);
            history_.drop(id);
            shardManager_->untrack(id);
            shardManager_->markDirty(*key);
            return;
        }
    }
    history_.drop(id);
}

// 伪代码:
//   1. 加锁,调用 applyRemove 同步清理两个索引与 store;
//   2. 删除成功则追加 Remove 型 WAL 记录;
//   3. 返回是否删除成功。
bool StmbService::remove(BlockId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sharded() && !ensureLoadedForId(id)) {
        return false;
    }
    const bool ok = applyRemove(id);
    if (ok) {
        WalRecord rec;
        rec.type = RecordType::Remove;
        rec.id = id;
        if (sharded()) {
            rec.hasShardKey = true;
            rec.shard = *shardManager_->shardOf(id);
        }
        logWal(rec);
    }
    return ok;
}

// 伪代码:
//   1. 加锁,调用 applyExpireBefore 截断时间索引并清理 store/空间索引;
//   2. 若有块被清理,追加 ExpireBefore 型 WAL 记录;
//   3. 返回被清理的 id 列表。
std::vector<BlockId> StmbService::expireBefore(TimeStamp ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<BlockId> expired = applyExpireBefore(ts);
    if (!expired.empty()) {
        WalRecord rec;
        rec.type = RecordType::ExpireBefore;
        rec.ts = ts;
        logWal(rec);
    }
    return expired;
}

// 伪代码:
//   1. 加锁;
//   2. 分别从 store 读取当前块数、容量、累计淘汰次数,组装成 ServiceStats 返回。
ServiceStats StmbService::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ServiceStats s;
    s.blockCount = store_.size();
    s.capacity = store_.capacity();
    s.evictCount = store_.evictCount();
    s.loadedShards = sharded() ? shardManager_->loadedCount() : 0;
    return s;
}

// 伪代码:
//   1. 未开启持久化(dataDir 为空)直接返回 false;
//   2. 加锁;分片模式:flushAll 写全部脏分片 -> 生成并写 manifest
//      (全局参数 + nextId + maxBlockRadius + 分片清单)-> 截断 WAL;
//   3. 单文件模式:从 store 取全部块、从 VersionLog 取全部版本记录,
//      调用 PersistenceManager::checkpoint 落快照并截断 WAL;
//   4. 返回成败。
bool StmbService::checkpoint() {
    if (config_.dataDir.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (sharded()) {
        if (!shardManager_->flushAll()) {
            return false;
        }
        ManifestData manifest;
        manifest.capacity = store_.capacity();
        manifest.cellSize = config_.cellSize;
        manifest.timeSlotMs = config_.timeSlotMs;
        manifest.confirmThreshold = config_.confirmThreshold;
        manifest.decayHalfLifeMs = config_.decayHalfLifeMs;
        manifest.shardCellSize = config_.shardCellSize;
        manifest.shardTimeSpanMs = config_.shardTimeSpanMs;
        manifest.maxLoadedShards = config_.maxLoadedShards;
        manifest.nextId = nextId_;
        manifest.maxBlockRadius = maxBlockRadius_;
        manifest.levelCount = static_cast<std::uint64_t>(levelCount());
        manifest.sources = sources_.all();  // v6:来源注册表随 manifest 落盘
        for (const ShardKey& key : shardManager_->knownShards()) {
            ManifestShardEntry entry;
            entry.key = key;
            entry.blockIds = shardManager_->idsOf(key);
            entry.versionCount = shardManager_->versionCountOf(key);
            manifest.shards.push_back(std::move(entry));
        }
        if (!PersistenceManager::saveManifest(config_.dataDir, manifest)) {
            return false;
        }
        // 动态层单独落盘 dynamic.stmb(实例位置漂移,不归属固定分片)
        if (dynamic_ &&
            !PersistenceManager::saveDynamic(config_.dataDir,
                                             dynamic_->allInstances(),
                                             dynamic_->nextInstanceId())) {
            return false;
        }
        return PersistenceManager::truncateWal(config_.dataDir);
    }
    return PersistenceManager::checkpoint(
        config_.dataDir, store_.blocks(), history_.allVersions(), nextId_,
        dynamic_ ? dynamic_->allInstances() : std::vector<DynamicInstance>{},
        dynamic_ ? dynamic_->nextInstanceId() : InstanceId{1},
        sources_.all());  // v6:来源注册表随快照落盘
}

// 伪代码:
//   1. 若配置半衰期为 0(不衰减),原样返回块;
//   2. 否则用 decayedConfidence 以块的 lastUpdate 为起算点计算 now 时刻的
//      有效置信度,替换副本上的 confidence 字段后返回。
MemoryBlock StmbService::applyDecay(MemoryBlock block, TimeStamp now) const {
    if (config_.decayHalfLifeMs > 0) {
        block.confidence = decayedConfidence(block.confidence, block.lastUpdate,
                                             now, config_.decayHalfLifeMs);
    }
    return block;
}

// 伪代码:
//   1. loadSnapshot:快照损坏(magic/version/CRC)抛 runtime_error;
//   2. 逐块重建:store_.put 写入,同时登记空间索引与时间索引
//      (快照内容即 store 现状,块数 <= 容量,不会触发淘汰);
//   3. 逐条归档版本记录,恢复 nextId;
//   4. replayWal:WAL 头部损坏抛 runtime_error;逐条记录按类型分发到
//      对应的 apply 路径(不写日志),重现快照之后的全部变更。
void StmbService::restore() {
    std::optional<SnapshotData> snap = PersistenceManager::loadSnapshot(config_.dataDir);
    if (!snap.has_value()) {
        throw std::runtime_error("stmb: 快照损坏或格式版本不兼容: " + config_.dataDir);
    }
    for (const MemoryBlock& block : snap->blocks) {
        store_.put(block);
        spatial_.insert(block.id, block.region);
        temporal_.insert(block.timestamp, block.id);
    }
    for (const auto& [id, version] : snap->versions) {
        history_.archive(id, version);
    }
    nextId_ = snap->nextId;
    if (dynamic_) {
        dynamic_->restore(snap->instances, snap->instanceNextId);
    }
    // v6:恢复快照中的来源注册表
    for (const auto& [src, reliability] : snap->sources) {
        applyRegisterSource(src, reliability);
    }

    std::optional<std::vector<WalRecord>> records =
        PersistenceManager::replayWal(config_.dataDir);
    if (!records.has_value()) {
        throw std::runtime_error("stmb: WAL 损坏或格式版本不兼容: " + config_.dataDir);
    }
    for (const WalRecord& rec : *records) {
        switch (rec.type) {
            case RecordType::PutBlock:
                applyPut(rec.block);
                break;
            case RecordType::Confirm:
                applyConfirmWeighted(rec.id, rec.ts, rec.weight);
                break;
            case RecordType::Observe: {
                Observation obs;
                obs.payload = rec.payload;
                obs.confidence = rec.confidence;
                obs.source = rec.source;
                obs.t = rec.ts;
                submitObservationLocked(rec.id, obs);
                break;
            }
            case RecordType::RegisterSource:
                applyRegisterSource(rec.source, rec.weight);
                break;
            case RecordType::ReportChange:
                applyReportChange(rec.id, rec.payload, rec.confidence, rec.ts);
                break;
            case RecordType::Remove:
                applyRemove(rec.id);
                break;
            case RecordType::ExpireBefore:
                applyExpireBefore(rec.ts);
                break;
            default:
                break;
        }
    }
}

// 伪代码:
//   1. 记录写入前该 id 是否已存在(重放重复 PutBlock 时避免重复归档 v1);
//   2. store_.put 写入,取回可能被淘汰的块;
//   3. 若有被淘汰块:按其 region / timestamp 从两个索引中摘除(历史保留);
//   4. 把新块登记进空间索引与时间索引;
//   5. 首次写入(此前不存在)才把 v1 快照归档进版本历史;
//   6. 推进 nextId_ 使其始终大于已见过的最大 id(重放路径必须保持);
//   7. 返回被淘汰块(或空)。
std::optional<MemoryBlock> StmbService::applyPut(const MemoryBlock& block) {
    const bool existed = store_.contains(block.id);
    std::optional<MemoryBlock> evicted = store_.put(block);
    if (evicted.has_value()) {
        handleEvicted(*evicted);  // 清索引;分片模式下标记下沉与脏
    }
    spatialOf(block.level).insert(block.id, block.region);
    temporal_.insert(block.timestamp, block.id);
    if (!existed) {
        history_.archive(block.id, BlockVersion{block.version, block.payload,
                                                block.confidence, block.state,
                                                block.timestamp, std::nullopt});
    }
    if (block.id >= nextId_) {
        nextId_ = block.id + 1;
    }
    if (sharded()) {
        const ShardKey key = makeShardKey(block.region, block.timestamp,
                                          config_.shardCellSize,
                                          config_.shardTimeSpanMs, block.level);
        shardManager_->track(block.id, key);
        shardManager_->markDirty(key);
        double radius = 0.0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            radius = std::max(radius,
                              (block.region.max[axis] - block.region.min[axis]) / 2.0);
        }
        maxBlockRadius_ = std::max(maxBlockRadius_, radius);
    }
    return evicted;
}

// 伪代码:
//   1. 用 store_.get 取块,不存在返回 false;
//   2. Pending 分支:确认计数按 weight 累加,lastUpdate = now;达到加权阈值
//      转 Stable;写回 store;分片模式标脏;
//   3. Changing 分支:封存版本链当前版本(validTo = now);候选变更生效
//      (payload/confidence 替换、清空候选与观察窗口字段)、version +1、
//      状态转 Stable、lastUpdate = now;写回 store 并把新版本归档进历史;
//      分片模式标脏;
//   4. Stable 分支:无事可确认,返回 false。
bool StmbService::applyConfirmWeighted(BlockId id, TimeStamp now, double weight) {
    std::optional<MemoryBlock> found = store_.get(id);
    if (!found.has_value()) {
        return false;
    }
    MemoryBlock block = *found;
    if (block.state == BlockState::Pending) {
        block.confirmations += weight;
        block.lastUpdate = now;
        if (block.confirmations >= config_.confirmThreshold) {
            block.state = BlockState::Stable;
        }
        store_.put(block);
        markDirtyOf(id);
        return true;
    }
    if (block.state == BlockState::Changing) {
        history_.seal(id, now);
        block.payload = block.pendingPayload.value_or(block.payload);
        block.confidence = block.pendingConfidence.value_or(block.confidence);
        block.pendingPayload.reset();
        block.pendingConfidence.reset();
        block.windowStart = -1;
        block.windowWeight = 0.0;
        block.windowSources.clear();
        ++block.version;
        block.state = BlockState::Stable;
        block.lastUpdate = now;
        store_.put(block);
        history_.archive(id, BlockVersion{block.version, block.payload,
                                          block.confidence, block.state,
                                          now, std::nullopt});
        markDirtyOf(id);
        return true;
    }
    return false;
}

// 伪代码:
//   1. 用 store_.get 取块,不存在或非 Stable 返回 false;
//   2. 把新 payload/confidence 存入候选字段,状态转 Changing,
//      lastUpdate = now,写回 store,返回 true(当前生效数据不变)。
bool StmbService::applyReportChange(BlockId id, std::string newPayload,
                                    double newConfidence, TimeStamp now) {
    std::optional<MemoryBlock> found = store_.get(id);
    if (!found.has_value() || found->state != BlockState::Stable) {
        return false;
    }
    MemoryBlock block = *found;
    block.pendingPayload = std::move(newPayload);
    block.pendingConfidence = newConfidence;
    block.state = BlockState::Changing;
    block.lastUpdate = now;
    store_.put(block);
    if (sharded()) {
        const std::optional<ShardKey> key = shardManager_->shardOf(id);
        if (key.has_value()) {
            shardManager_->markDirty(*key);
        }
    }
    return true;
}

// 伪代码:
//   1. 用 store_.get 取出块数据;命中则按 region/timestamp 清理两个索引并
//      从 store 删除,分片模式下标记分片为脏,返回 true;
//   2. 未命中且为分片模式:若块已下沉(只在盘上),从下沉集合移除并标记脏
//      (归并写盘时不再包含它),返回 true —— 历史仍保留;
//   3. 其他情况返回 false。
bool StmbService::applyRemove(BlockId id) {
    std::optional<MemoryBlock> block = store_.get(id);
    if (block.has_value()) {
        spatialOf(block->level < 0 ? 0 : block->level).remove(id, block->region);
        temporal_.remove(block->timestamp, id);
        const bool ok = store_.remove(id);
        if (ok && sharded()) {
            const std::optional<ShardKey> key = shardManager_->shardOf(id);
            if (key.has_value()) {
                shardManager_->markDirty(*key);
            }
        }
        return ok;
    }
    if (sharded()) {
        const std::optional<ShardKey> key = shardManager_->shardOf(id);
        if (key.has_value() && shardManager_->isLoaded(*key) &&
            shardManager_->isSunk(*key, id)) {
            shardManager_->clearSunk(*key, id);
            shardManager_->markDirty(*key);
            return true;
        }
    }
    return false;
}

// 伪代码:
//   1. 分片模式:遍历已知分片,跳过桶起点 >= ts 的分片(不可能含过期块),
//      其余逐 ensureLoaded 后按 id 检查时间戳清理(同步索引与 store),
//      有清理的分片标记脏;收集并返回被清理的 id 列表;
//   2. 单文件模式:调用 temporal_.expireBefore 截断时间索引前缀,对每个 id
//      取块数据清理空间索引并从 store 删除(版本历史保留);
//   3. 返回被清理的 id 列表。
std::vector<BlockId> StmbService::applyExpireBefore(TimeStamp ts) {
    if (sharded()) {
        std::vector<BlockId> expired;
        for (const ShardKey& key : shardManager_->knownShards()) {
            if (key.tBucket * config_.shardTimeSpanMs >= ts) {
                continue;
            }
            shardManager_->ensureLoaded(key);
            const std::vector<BlockId> ids = shardManager_->idsOf(key);
            bool shardChanged = false;
            for (BlockId id : ids) {
                std::optional<MemoryBlock> block = store_.get(id);
                if (block.has_value() && block->timestamp < ts) {
                    spatialOf(block->level < 0 ? 0 : block->level)
                        .remove(id, block->region);
                    temporal_.remove(block->timestamp, id);
                    store_.remove(id);
                    expired.push_back(id);
                    shardChanged = true;
                }
            }
            if (shardChanged) {
                shardManager_->markDirty(key);
            }
        }
        return expired;
    }
    const std::vector<BlockId> expired = temporal_.expireBefore(ts);
    for (BlockId id : expired) {
        std::optional<MemoryBlock> block = store_.get(id);
        if (block.has_value()) {
            spatialOf(block->level < 0 ? 0 : block->level).remove(id, block->region);
        }
        store_.remove(id);
    }
    return expired;
}

// 伪代码:
//   1. 未开启持久化直接返回(空操作);
//   2. 调用 PersistenceManager::appendWal 追加记录;失败仅忽略(尽力而为,
//      不影响内存态的正确性,下次 checkpoint 仍可收敛)。
void StmbService::logWal(const WalRecord& record) {
    if (config_.dataDir.empty()) {
        return;
    }
    (void)PersistenceManager::appendWal(config_.dataDir, record);
}

// 伪代码:
//   1. 返回是否处于分片模式:配置了 dataDir 且 shardCellSize > 0。
bool StmbService::sharded() const {
    return !config_.dataDir.empty() && config_.shardCellSize > 0.0;
}

// 伪代码(分片回调:数据注入内存):
//   1. 先把版本记录逐条归档进 VersionLog(历史随分片驻留内存);
//   2. 逐块写入 store:若触发全局 LRU 淘汰,用 handleEvicted 处理
//      (清索引 + 下沉标记);
//   3. 把块登记进空间索引与时间索引。
void StmbService::onShardLoad(const ShardKey& /*key*/, const ShardData& data) {
    for (const auto& [id, version] : data.versions) {
        history_.archive(id, version);
    }
    for (const MemoryBlock& block : data.blocks) {
        std::optional<MemoryBlock> evicted = store_.put(block);
        if (evicted.has_value()) {
            handleEvicted(*evicted);
        }
        spatialOf(block.level < 0 ? 0 : block.level).insert(block.id, block.region);
        temporal_.insert(block.timestamp, block.id);
    }
}

// 伪代码(分片回调:归并收集):
//   1. 以盘上旧数据为基础,建立 id -> 旧块的映射(用于取回下沉块);
//   2. 遍历该分片全部 id:内存 store 中有的取内存现状(最新);
//      已下沉的取盘上旧块;其余(已删除/过期)不再写入;
//   3. 版本记录全部从内存 VersionLog 现取(内存为最新);
//   4. 返回归并结果,由 ShardManager 落盘。
ShardData StmbService::onShardMerge(const ShardKey& key, ShardData old) {
    std::unordered_map<BlockId, const MemoryBlock*> oldBlocks;
    for (const MemoryBlock& b : old.blocks) {
        oldBlocks[b.id] = &b;
    }
    ShardData out;
    for (BlockId id : shardManager_->idsOf(key)) {
        std::optional<MemoryBlock> current = store_.get(id);
        if (current.has_value()) {
            out.blocks.push_back(*current);
        } else if (shardManager_->isSunk(key, id)) {
            auto it = oldBlocks.find(id);
            if (it != oldBlocks.end()) {
                out.blocks.push_back(*it->second);
            }
        }
    }
    for (BlockId id : shardManager_->idsOf(key)) {
        for (const BlockVersion& v : history_.historyOf(id)) {
            out.versions.emplace_back(id, v);
        }
    }
    return out;
}

// 伪代码(分片回调:从内存移除):
//   1. 遍历该分片全部 id:在 store 中的块同步清理两个索引并从 store 删除;
//   2. 该分片所有 id 的版本历史从内存 VersionLog 清除(已随分片落盘)。
void StmbService::onShardUnload(const ShardKey& key) {
    for (BlockId id : shardManager_->idsOf(key)) {
        std::optional<MemoryBlock> block = store_.get(id);
        if (block.has_value()) {
            spatialOf(block->level < 0 ? 0 : block->level).remove(id, block->region);
            temporal_.remove(block->timestamp, id);
            store_.remove(id);
        }
        history_.drop(id);
    }
}

// 伪代码:
//   1. 按 region/timestamp 清理被淘汰块的两个索引;
//   2. 分片模式下:定位其分片,标记「已下沉」并置脏(数据等归并时落盘)。
void StmbService::handleEvicted(const MemoryBlock& evicted) {
    spatialOf(evicted.level < 0 ? 0 : evicted.level).remove(evicted.id, evicted.region);
    temporal_.remove(evicted.timestamp, evicted.id);
    if (sharded()) {
        const std::optional<ShardKey> key = shardManager_->shardOf(evicted.id);
        if (key.has_value()) {
            shardManager_->markSunk(*key, evicted.id);
            shardManager_->markDirty(*key);
        }
    }
}

// 伪代码:
//   1. 查路由目录定位 id 所属分片,不存在返回 false;
//   2. 调用 ShardManager::ensureLoaded 加载该分片,返回结果。
bool StmbService::ensureLoadedForId(BlockId id) {
    const std::optional<ShardKey> key = shardManager_->shardOf(id);
    if (!key.has_value()) {
        return false;
    }
    return shardManager_->ensureLoaded(*key);
}

// 伪代码:
//   1. 把查询区域按 maxBlockRadius_ 向外扩边(块按中心分片,相交块的中心
//      可能落在区域外最多一个半径的距离);
//   2. 计算扩边区域覆盖的空间分片坐标区间与时间桶区间;
//   3. 枚举四维组合,仅对「已知分片」调用 ensureLoaded;
//   4. 加载成功的分片立即 pin 钉住并收入返回列表,防止一次查询触达的分片数
//      超过驻留上限时,先加载的分片被后加载的挤出去导致查询不完整
//     (调用方在查询结束后逐一 unpin)。
std::vector<ShardKey> StmbService::loadTouchedShards(const AABB& region,
                                                     const TimeRange& range,
                                                     int level) {
    std::vector<ShardKey> pinned;
    AABB expanded = region;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        expanded.min[axis] -= maxBlockRadius_;
        expanded.max[axis] += maxBlockRadius_;
    }
    std::int64_t lo[3];
    std::int64_t hi[3];
    for (std::size_t axis = 0; axis < 3; ++axis) {
        lo[axis] = static_cast<std::int64_t>(
            std::floor(expanded.min[axis] / config_.shardCellSize));
        hi[axis] = static_cast<std::int64_t>(
            std::floor(expanded.max[axis] / config_.shardCellSize));
    }
    const std::int64_t tb0 = static_cast<std::int64_t>(std::floor(
        static_cast<double>(range.start) / static_cast<double>(config_.shardTimeSpanMs)));
    const std::int64_t tb1 = static_cast<std::int64_t>(std::floor(
        static_cast<double>(range.end) / static_cast<double>(config_.shardTimeSpanMs)));
    for (std::int64_t x = lo[0]; x <= hi[0]; ++x) {
        for (std::int64_t y = lo[1]; y <= hi[1]; ++y) {
            for (std::int64_t z = lo[2]; z <= hi[2]; ++z) {
                for (std::int64_t tb = tb0; tb <= tb1; ++tb) {
                    const ShardKey key{x, y, z, tb, level};
                    if (shardManager_->isKnown(key) &&
                        shardManager_->ensureLoaded(key)) {
                        shardManager_->pin(key);
                        pinned.push_back(key);
                    }
                }
            }
        }
    }
    return pinned;
}

// 伪代码(分片模式恢复):
//   1. loadManifest:损坏抛异常;存在则校验分片参数与当前配置一致,
//      恢复 nextId 与 maxBlockRadius,并把分片清单与路由目录交给 ShardManager;
//   2. replayWal:WAL 头部损坏抛异常;逐条记录:带分片键的先登记并加载该
//      分片,再按类型分发到对应 apply 路径(不写日志);
//   3. 重放结束后内存状态 = 落盘状态 + WAL 增量。
void StmbService::restoreSharded() {
    std::optional<ManifestData> manifest =
        PersistenceManager::loadManifest(config_.dataDir);
    if (!manifest.has_value()) {
        throw std::runtime_error("stmb: manifest 损坏或格式版本不兼容: " +
                                 config_.dataDir);
    }
    if (manifest->present) {
        if (manifest->shardCellSize != config_.shardCellSize ||
            manifest->shardTimeSpanMs != config_.shardTimeSpanMs ||
            static_cast<int>(manifest->levelCount) != levelCount()) {
            throw std::runtime_error("stmb: 分片/层级参数与 manifest 不一致: " +
                                     config_.dataDir);
        }
        nextId_ = manifest->nextId;
        maxBlockRadius_ = manifest->maxBlockRadius;
        shardManager_->loadManifest(*manifest);
    }
    if (dynamic_) {
        std::optional<std::pair<std::vector<DynamicInstance>, InstanceId>> dyn =
            PersistenceManager::loadDynamic(config_.dataDir);
        if (!dyn.has_value()) {
            throw std::runtime_error("stmb: 动态层文件损坏或格式版本不兼容: " +
                                     config_.dataDir);
        }
        dynamic_->restore(dyn->first, dyn->second);
    }
    // v6:恢复 manifest 中的来源注册表
    if (manifest->present) {
        for (const auto& [src, reliability] : manifest->sources) {
            applyRegisterSource(src, reliability);
        }
    }

    std::optional<std::vector<WalRecord>> records =
        PersistenceManager::replayWal(config_.dataDir);
    if (!records.has_value()) {
        throw std::runtime_error("stmb: WAL 损坏或格式版本不兼容: " + config_.dataDir);
    }
    for (const WalRecord& rec : *records) {
        if (rec.hasShardKey) {
            shardManager_->addKnown(rec.shard);
            shardManager_->ensureLoaded(rec.shard);
        }
        switch (rec.type) {
            case RecordType::PutBlock:
                applyPut(rec.block);
                break;
            case RecordType::Confirm:
                applyConfirmWeighted(rec.id, rec.ts, rec.weight);
                break;
            case RecordType::Observe: {
                Observation obs;
                obs.payload = rec.payload;
                obs.confidence = rec.confidence;
                obs.source = rec.source;
                obs.t = rec.ts;
                submitObservationLocked(rec.id, obs);
                break;
            }
            case RecordType::RegisterSource:
                applyRegisterSource(rec.source, rec.weight);
                break;
            case RecordType::ReportChange:
                applyReportChange(rec.id, rec.payload, rec.confidence, rec.ts);
                break;
            case RecordType::Remove:
                applyRemove(rec.id);
                break;
            case RecordType::ExpireBefore:
                applyExpireBefore(rec.ts);
                break;
            default:
                break;
        }
    }
}

// 伪代码:
//   1. levelCellSizes 非空即 LOD 模式。
bool StmbService::lodMode() const {
    return !config_.levelCellSizes.empty();
}

// 伪代码:
//   1. 把 level 夹取到 [0, levelCount-1] 区间返回。
int StmbService::clampLevel(int level) const {
    if (level < 0) {
        return 0;
    }
    return std::min(level, levelCount() - 1);
}

// 伪代码:
//   1. LOD 模式返回 levelCellSizes[level](先夹取);单尺度返回 cellSize。
double StmbService::cellSizeOf(int level) const {
    if (!lodMode()) {
        return config_.cellSize;
    }
    return config_.levelCellSizes[static_cast<std::size_t>(clampLevel(level))];
}

// 伪代码:
//   1. LOD 模式返回该层级的独立空间索引;单尺度返回兼容路径的 spatial_。
SpatialGridIndex& StmbService::spatialOf(int level) {
    if (!lodMode()) {
        return spatial_;
    }
    return spatialByLevel_[static_cast<std::size_t>(clampLevel(level))];
}

// 伪代码:
//   1. 单尺度直接返回 0;
//   2. 计算区域最大轴长度 extent;
//   3. 选 |cellSize - extent| 最小的层级;并列时取更细层级(用 <= 更新)。
int StmbService::detectLevel(const AABB& region) const {
    if (!lodMode()) {
        return 0;
    }
    double extent = 0.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        extent = std::max(extent, region.max[axis] - region.min[axis]);
    }
    int best = 0;
    double bestDiff = std::abs(config_.levelCellSizes[0] - extent);
    for (int level = 1; level < levelCount(); ++level) {
        const double diff =
            std::abs(config_.levelCellSizes[static_cast<std::size_t>(level)] - extent);
        if (diff <= bestDiff) {
            bestDiff = diff;
            best = level;
        }
    }
    return best;
}

// 伪代码:
//   1. 逐层检查比 fromLevel 更细的层级:
//   2. 分片模式:对该层已知分片做「分片包围盒与区域相交」的粗判,可能相交
//      的先 ensureLoaded 再查索引;
//   3. 用该层空间索引粗筛候选,逐块精过滤(区域相交即算有细层数据);
//   4. 任一层级找到即返回 true,全部没有返回 false。
bool StmbService::hasFinerData(const AABB& region, int fromLevel) {
    for (int level = fromLevel + 1; level < levelCount(); ++level) {
        if (sharded()) {
            for (const ShardKey& key : shardManager_->knownShards()) {
                if (key.level != level) {
                    continue;
                }
                const double scs = config_.shardCellSize;
                const AABB shardBox{{static_cast<double>(key.sx) * scs,
                                    static_cast<double>(key.sy) * scs,
                                    static_cast<double>(key.sz) * scs},
                                    {static_cast<double>(key.sx + 1) * scs,
                                     static_cast<double>(key.sy + 1) * scs,
                                     static_cast<double>(key.sz + 1) * scs}};
                if (shardBox.intersects(region)) {
                    shardManager_->ensureLoaded(key);
                }
            }
        }
        const auto candidates = spatialOf(level).queryRegion(region);
        for (BlockId id : candidates) {
            std::optional<MemoryBlock> block = store_.get(id);
            if (block.has_value() && block->region.intersects(region)) {
                return true;
            }
        }
    }
    return false;
}

// 伪代码:
//   1. 加锁;未启用动态层返回 0;
//   2. 透传 DynamicLayer::report(按 id 上报,id 为 0 自动分配)。
InstanceId StmbService::reportMoving(InstanceId id, const std::string& classLabel,
                                     const AABB& bounds,
                                     const std::array<double, 3>& velocity,
                                     TimeStamp now, SourceId source) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dynamic_) {
        return 0;
    }
    return dynamic_->report(id, classLabel, bounds, velocity, now, source);
}

// 伪代码:
//   1. 加锁;未启用动态层返回 0;
//   2. 透传 DynamicLayer::reportAuto(跨源关联,匹配不到才新建)。
InstanceId StmbService::reportMoving(const std::string& classLabel,
                                     const AABB& bounds,
                                     const std::array<double, 3>& velocity,
                                     TimeStamp now, SourceId source) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dynamic_) {
        return 0;
    }
    return dynamic_->reportAuto(classLabel, bounds, velocity, now, source);
}

// 伪代码:
//   1. 加锁;未启用动态层返回空列表;
//   2. 透传 DynamicLayer::queryActive。
std::vector<DynamicInstance> StmbService::queryDynamic(const AABB& region,
                                                       const TimeRange& range) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dynamic_) {
        return {};
    }
    return dynamic_->queryActive(region, range);
}

// 伪代码:
//   1. 加锁;未启用动态层返回空列表;
//   2. 透传 DynamicLayer::trajectoryOf(含已归档实例)。
std::vector<TrackPoint> StmbService::trajectoryOf(InstanceId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dynamic_) {
        return {};
    }
    return dynamic_->trajectoryOf(id);
}

// 伪代码:
//   1. 加锁;未启用动态层直接返回;
//   2. 透传 DynamicLayer::update 推进状态机。
void StmbService::updateDynamic(TimeStamp now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dynamic_) {
        dynamic_->update(now);
    }
}

// 伪代码(沉淀:动态 -> 准静态 -> 静态):
//   1. 加锁;未启用动态层返回空列表;
//   2. 先 update(now) 推进状态,把超时静止的实例推进到 Stationary;
//   3. 遍历实例:对 Stationary 者,以其包围盒/类别/置信度/最新时刻构造
//      MemoryBlock,走 putInternal 写入静态层(状态 Pending,待确认);
//   4. 实例本身标记 Archived(轨迹保留供回溯);
//   5. 返回新建的静态块 id 列表。
std::vector<BlockId> StmbService::promoteStationary(TimeStamp now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dynamic_) {
        return {};
    }
    dynamic_->update(now);
    std::vector<BlockId> created;
    for (const DynamicInstance& inst : dynamic_->allInstances()) {
        if (inst.state != InstanceState::Stationary) {
            continue;
        }
        MemoryBlock block;
        block.region = inst.bounds;
        block.payload = inst.classLabel;
        block.timestamp = inst.latest.t;
        block.confidence = inst.confidence;
        putInternal(std::move(block));
        created.push_back(nextId_ - 1);  // putInternal 刚分配的 id
        dynamic_->markArchived(inst.id);
    }
    return created;
}

// 伪代码:
//   1. 加锁;未启用动态层返回全零统计;
//   2. 透传 DynamicLayer::stats。
DynamicStats StmbService::dynamicStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dynamic_) {
        return DynamicStats{};
    }
    return dynamic_->stats();
}

// 伪代码:
//   1. 加锁,委托 submitObservationLocked;
//   2. 无论是否接受都追加 Observe 型 WAL 记录(重放需复现完整管线序列)。
ChangeReport StmbService::submitObservation(BlockId id, const Observation& obs) {
    std::lock_guard<std::mutex> lock(mutex_);
    ChangeReport report = submitObservationLocked(id, obs);
    WalRecord rec;
    rec.type = RecordType::Observe;
    rec.id = id;
    rec.payload = obs.payload;
    rec.confidence = obs.confidence;
    rec.source = obs.source;
    rec.ts = obs.t;
    if (sharded() && shardManager_->shardOf(id).has_value()) {
        rec.hasShardKey = true;
        rec.shard = *shardManager_->shardOf(id);
    }
    logWal(rec);
    return report;
}

// 伪代码(观测管线核心,调用方须持锁):
//   1. 分片模式先按 id 确保加载;取块副本;
//   2. 块不存在:空观测 -> 无操作;非空观测 -> Emergence 但缺少区域信息,
//      无法接受(提示先 put 空地块);
//   3. 周期模式判定:观测等于模式在当前相位的预测 -> Seasonal,按加权确认
//      处理,不进 Changing、不产生版本;
//   4. 观测与当前生效一致(且无挂起候选)-> 计入加权确认,报 Transient
//     (无净变化);
//   5. Changing 窗口中:反向观测(== 当前生效)-> Transient 关闭窗口;
//      同候选 -> 累加权重,达到阈值则候选生效;第三种值 -> Conflict 挂起;
//   6. 新候选(Stable 块):分类 Emergence/Vanishing/Mutation;单源权重即达
//      阈值则立即生效,否则挂起进观察窗口;
//   7. 非 Stable 且非 Changing 的块收到矛盾观测 -> Conflict;
//   8. 各路径末尾把 validateSpatialLocked 的命中信息附加进 message。
ChangeReport StmbService::submitObservationLocked(BlockId id, const Observation& obs) {
    if (sharded()) {
        ensureLoadedForId(id);
    }
    std::optional<MemoryBlock> found = store_.get(id);
    if (!found.has_value()) {
        if (obs.payload.empty()) {
            return ChangeReport{ChangeType::Vanishing, false,
                                "无既有块且观测为空:无操作"};
        }
        return ChangeReport{ChangeType::Emergence, false,
                            "块不存在且无区域信息:需先 put 空地占位块"};
    }
    MemoryBlock block = *found;
    const double weight = sources_.reliabilityOf(obs.source);
    ChangeReport report;

    // 周期模式:命中当前相位预测 -> Seasonal
    if (block.pattern.has_value()) {
        const PeriodicPattern& pattern = *block.pattern;
        const std::int64_t span = pattern.periodMs > 0 ? pattern.periodMs : 1;
        const std::int64_t phase = ((obs.t % span) + span) % span;
        for (const PeriodicPattern::Phase& p : pattern.phases) {
            if (phase >= p.offsetMs && phase < p.offsetMs + p.durationMs &&
                p.payload == obs.payload) {
                block.lastUpdate = obs.t;
                block.confirmations += weight;
                store_.put(block);
                markDirtyOf(id);
                report.type = ChangeType::Seasonal;
                report.accepted = true;
                report.message = "命中周期模式当前相位:按确认处理,不产生新版本";
                for (const std::string& v : validateSpatialLocked(id)) {
                    report.message += ";" + v;
                }
                return report;
            }
        }
    }

    // 观测与当前生效一致(且无挂起候选)-> 加权确认
    if (block.state != BlockState::Changing && obs.payload == block.payload) {
        block.confirmations += weight;
        block.lastUpdate = obs.t;
        if (block.state == BlockState::Pending &&
            block.confirmations >= config_.confirmThreshold) {
            block.state = BlockState::Stable;
        }
        store_.put(block);
        markDirtyOf(id);
        report.type = ChangeType::Transient;
        report.accepted = true;
        report.message = "观测与当前一致:计入加权确认(无净变化)";
        for (const std::string& v : validateSpatialLocked(id)) {
            report.message += ";" + v;
        }
        return report;
    }

    // Changing 窗口中的三种后续观测
    if (block.state == BlockState::Changing && block.pendingPayload.has_value()) {
        if (obs.payload == block.payload) {
            // 反向观测推翻候选 -> Transient 关闭窗口,不产生版本
            block.state = BlockState::Stable;
            block.pendingPayload.reset();
            block.pendingConfidence.reset();
            block.windowStart = -1;
            block.windowWeight = 0.0;
            block.windowSources.clear();
            block.lastUpdate = obs.t;
            store_.put(block);
            markDirtyOf(id);
            report.type = ChangeType::Transient;
            report.accepted = true;
            report.message = "窗口内反向观测推翻候选:按瞬态噪声关闭,不产生版本";
            return report;
        }
        if (*block.pendingPayload == obs.payload) {
            // 同候选加权累计,达阈值则生效
            block.windowWeight += weight;
            block.windowSources.insert(obs.source);
            block.lastUpdate = obs.t;
            if (block.windowWeight >= config_.confirmThreshold) {
                report.type = obs.payload.empty()
                                  ? ChangeType::Vanishing
                                  : (block.payload.empty() ? ChangeType::Emergence
                                                           : ChangeType::Mutation);
                applyCandidate(block, obs.t);
                report.accepted = true;
                report.message = "候选加权达到阈值:变更生效";
            } else {
                store_.put(block);
                markDirtyOf(id);
                report.type = obs.payload.empty() ? ChangeType::Vanishing
                                                  : ChangeType::Mutation;
                report.accepted = false;
                report.message = "候选挂起中:权重累计未达阈值";
            }
            return report;
        }
        // 第三种矛盾值 -> Conflict 挂起
        report.type = ChangeType::Conflict;
        report.accepted = false;
        report.message = "观测与挂起候选矛盾且未达阈值:保持挂起等待仲裁";
        return report;
    }

    // 新候选:仅 Stable 块接受(与 reportChange 语义一致)
    if (block.state != BlockState::Stable) {
        report.type = ChangeType::Conflict;
        report.accepted = false;
        report.message = "块未稳定即收到矛盾观测:挂起";
        return report;
    }
    const ChangeType type = obs.payload.empty()
                                ? ChangeType::Vanishing
                                : (block.payload.empty() ? ChangeType::Emergence
                                                         : ChangeType::Mutation);
    if (weight >= config_.confirmThreshold) {
        // 单源高可靠:直接生效
        block.pendingPayload = obs.payload;
        block.pendingConfidence = obs.confidence;
        block.state = BlockState::Changing;  // 复用候选生效路径
        applyCandidate(block, obs.t);
        report.type = type;
        report.accepted = true;
        report.message = "单源权重达到阈值:变更直接生效";
    } else {
        // 挂起进入观察窗口
        block.state = BlockState::Changing;
        block.pendingPayload = obs.payload;
        block.pendingConfidence = obs.confidence;
        block.windowStart = obs.t;
        block.windowWeight = weight;
        block.windowSources.clear();
        block.windowSources.insert(obs.source);
        block.lastUpdate = obs.t;
        store_.put(block);
        markDirtyOf(id);
        report.type = type;
        report.accepted = false;
        report.message = "候选挂起,进入观察窗口";
    }
    for (const std::string& v : validateSpatialLocked(id)) {
        report.message += ";" + v;
    }
    return report;
}

// 伪代码:
//   1. 加锁,登记来源可靠性到 SourceRegistry;
//   2. 追加 RegisterSource WAL 记录(v6 起重启不丢);
//   3. 触发来源变更钩子(若已设置,供外部系统同步;重放/水合不触发)。
void StmbService::registerSource(SourceId id, double reliability) {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_.registerSource(id, reliability);
    WalRecord rec;
    rec.type = RecordType::RegisterSource;
    rec.source = id;
    rec.weight = reliability;
    logWal(rec);
    if (sourceChangeHook_) {
        sourceChangeHook_(id, reliability);
    }
}

// 伪代码:
//   1. 加锁,查询来源可靠性(未知来源默认 0.5)。
double StmbService::reliabilityOf(SourceId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.reliabilityOf(id);
}

// 伪代码:
//   1. 加锁,安装来源变更钩子(空 function 表示清除)。
void StmbService::setSourceChangeHook(std::function<void(SourceId, double)> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    sourceChangeHook_ = std::move(hook);
}

// 伪代码:
//   1. 只登记不写 WAL、不触发钩子(供 WAL 重放与水合路径共用;
//      调用方须持锁)。
void StmbService::applyRegisterSource(SourceId id, double reliability) {
    sources_.registerSource(id, reliability);
}

// 伪代码:
//   1. 加锁;分片模式确保加载;取块,不存在返回 false;
//   2. 把模式写入块的 pattern 字段并写回 store,标脏,返回 true。
bool StmbService::registerPattern(BlockId id, const PeriodicPattern& pattern) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sharded()) {
        ensureLoadedForId(id);
    }
    std::optional<MemoryBlock> found = store_.get(id);
    if (!found.has_value()) {
        return false;
    }
    MemoryBlock block = *found;
    block.pattern = pattern;
    store_.put(block);
    markDirtyOf(id);
    return true;
}

// 伪代码:
//   1. 加锁;分片模式确保加载;取块返回其 pattern(无则返回空)。
std::optional<PeriodicPattern> StmbService::patternOf(BlockId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sharded() && shardManager_->shardOf(id).has_value()) {
        shardManager_->ensureLoaded(*shardManager_->shardOf(id));
    }
    std::optional<MemoryBlock> found = store_.peek(id);
    if (!found.has_value()) {
        return std::nullopt;
    }
    return found->pattern;
}

// 伪代码:
//   1. 加锁;窗口未启用(observationWindowMs <= 0)返回 0;
//   2. 遍历内存块:Changing 且窗口挂起超时( now - windowStart > 窗口 )的
//      候选被丢弃 —— 清空候选与窗口字段、状态恢复 Stable、写回并标脏;
//   3. 返回丢弃的候选数。
std::size_t StmbService::sweepWindows(TimeStamp now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (config_.observationWindowMs <= 0) {
        return 0;
    }
    std::size_t swept = 0;
    for (const MemoryBlock& snapshot : store_.blocks()) {
        if (snapshot.state != BlockState::Changing || snapshot.windowStart < 0) {
            continue;
        }
        if (now - snapshot.windowStart <= config_.observationWindowMs) {
            continue;
        }
        MemoryBlock block = snapshot;
        block.state = BlockState::Stable;
        block.pendingPayload.reset();
        block.pendingConfidence.reset();
        block.windowStart = -1;
        block.windowWeight = 0.0;
        block.windowSources.clear();
        block.lastUpdate = now;
        store_.put(block);
        markDirtyOf(block.id);
        ++swept;
    }
    return swept;
}

// 伪代码:
//   1. 加锁,委托 validateSpatialLocked。
std::vector<std::string> StmbService::validateSpatial(BlockId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return validateSpatialLocked(id);
}

// 伪代码(空间一致性校验,调用方须持锁):
//   1. 取块,不存在返回空列表;
//   2. 规则①垂直一致:块 payload 命中地面关键词时,检查正上方紧邻区域
//      (沿 z 轴平移一个块高)是否存在空语义/“sky”块,存在则记冲突;
//   3. 规则②悬空检测:非地面块下方紧邻区域(下移一个块高)没有任何
//      支撑块,记悬空;
//   4. 有命中则置 suspect 标志并写回(仅标记不拒绝),返回说明列表。
std::vector<std::string> StmbService::validateSpatialLocked(BlockId id) {
    std::optional<MemoryBlock> found = store_.get(id);
    if (!found.has_value()) {
        return {};
    }
    MemoryBlock block = *found;
    const double height = block.region.max[2] - block.region.min[2];
    AABB above = block.region;
    AABB below = block.region;
    above.min[2] += height;
    above.max[2] += height;
    below.min[2] -= height;
    below.max[2] -= height;

    bool isGround = false;
    for (const std::string& kw : groundKeywords_) {
        if (block.payload == kw) {
            isGround = true;
            break;
        }
    }

    std::vector<std::string> violations;
    if (isGround) {
        for (const MemoryBlock& other : store_.blocks()) {
            if (other.id != id && other.region.intersects(above) &&
                (other.payload.empty() || other.payload == "sky")) {
                violations.push_back("垂直一致冲突:地面语义块上方存在空/天空块");
                break;
            }
        }
    } else {
        bool supported = false;
        for (const MemoryBlock& other : store_.blocks()) {
            if (other.id != id && other.region.intersects(below)) {
                supported = true;
                break;
            }
        }
        if (!supported) {
            violations.push_back("悬空检测:非地面块下方无支撑块");
        }
    }
    if (!violations.empty()) {
        block.suspect = true;
        store_.put(block);
        markDirtyOf(id);
    }
    return violations;
}

// 伪代码:
//   1. 加锁,替换地面语义关键词集合。
void StmbService::setGroundKeywords(std::vector<std::string> keywords) {
    std::lock_guard<std::mutex> lock(mutex_);
    groundKeywords_ = std::move(keywords);
}

// 伪代码(候选生效,调用方须持锁;块应处于 Changing 且候选已填):
//   1. 封存版本链当前版本(validTo = now);
//   2. 候选 payload/confidence 生效,清空候选与观察窗口字段;
//   3. version +1、状态转 Stable、lastUpdate = now,写回 store;
//   4. 新版本归档进历史;分片模式标脏。
void StmbService::applyCandidate(MemoryBlock& block, TimeStamp now) {
    history_.seal(block.id, now);
    block.payload = block.pendingPayload.value_or(block.payload);
    block.confidence = block.pendingConfidence.value_or(block.confidence);
    block.pendingPayload.reset();
    block.pendingConfidence.reset();
    block.windowStart = -1;
    block.windowWeight = 0.0;
    block.windowSources.clear();
    ++block.version;
    block.state = BlockState::Stable;
    block.lastUpdate = now;
    store_.put(block);
    history_.archive(block.id, BlockVersion{block.version, block.payload,
                                            block.confidence, block.state,
                                            now, std::nullopt});
    markDirtyOf(block.id);
}

// 伪代码:
//   1. 分片模式下查路由目录,找到则标记该分片为脏;否则无操作。
void StmbService::markDirtyOf(BlockId id) {
    if (sharded()) {
        const std::optional<ShardKey> key = shardManager_->shardOf(id);
        if (key.has_value()) {
            shardManager_->markDirty(*key);
        }
    }
}

}  // namespace stmb
