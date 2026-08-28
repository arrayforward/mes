// ============================================================================
// 文件: shard_manager.h
// 模块: stmb_shard(分片管理,依赖 core / history / persistence)
// 用途: 声明 ShardManager —— 分片存储的运行时管理器。负责:
//         1. 已知分片清单(manifest 内容)与「块 id -> ShardKey」路由目录;
//         2. 内存分片缓存:maxLoadedShards 上限,超出按 LRU 换出
//            (脏分片先归并写盘再释放,干净分片直接释放);
//         3. 分片文件读写(经 PersistenceManager,自包含二进制格式);
//         4. 脏标记与 flushAll(checkpoint 用)。
// 设计思路:
//   1. ShardManager 不直接持有服务的数据组件(store / 索引 / VersionLog),
//      而是通过三个回调与服务协作:onLoad(把分片数据注入内存)、
//      onMerge(内存现状与盘上旧数据归并出待写内容)、onUnload(把分片内容
//      从内存组件移除);这样分片策略与数据布局解耦,也无循环依赖;
//   2. 块的全局 LRU 淘汰由服务层拦截后调用 markSunk 标记「已下沉」,
//      归并时从盘上旧数据取回这些块,保证不丢数据;
//   3. 分片文件自包含(块 + 版本记录),未来可直接分布到不同节点。
// 架构角色: 分片模式的存储调度核心,由 StmbService 持有(仅 shardCellSize>0 时)。
// ============================================================================
#pragma once

#include "persistence.h"
#include "types.h"

#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stmb {

class ShardManager {
public:
    // 与服务层协作的回调:数据注入 / 归并收集 / 内存移除
    struct Callbacks {
        // 把分片数据注入内存组件(store / 索引 / VersionLog)
        std::function<void(const ShardKey&, const ShardData&)> onLoad;
        // 归并:以内存现状覆盖盘上旧数据,返回待写盘的分片内容
        std::function<ShardData(const ShardKey&, ShardData)> onMerge;
        // 把该分片的块与版本从内存组件移除(数据已落盘)
        std::function<void(const ShardKey&)> onUnload;
    };

    // 构造:数据目录、内存分片上限(0 视为 1)、协作回调
    ShardManager(std::string dir, std::size_t maxLoaded, Callbacks cbs);

    // 确保分片已加载:未加载则先按需换出最冷分片,再从文件加载
    // (文件不存在 = 空分片);文件损坏抛 std::runtime_error
    bool ensureLoaded(const ShardKey& key);

    // 登记新分片(put 触达不存在的分片时)
    void addKnown(const ShardKey& key);
    bool isKnown(const ShardKey& key) const;
    bool isLoaded(const ShardKey& key) const;

    // 脏标记:该分片内存内容领先于盘上文件
    void markDirty(const ShardKey& key);

    // 钉住/解除钉住:被钉住的分片不参与换出(query 加载窗口期防抖动)
    void pin(const ShardKey& key);
    void unpin(const ShardKey& key);

    // 收缩到上限以内:反复换出最冷未钉住分片,直到 loadedCount <= maxLoaded
    // (query 结束解除钉住后调用,恢复驻留上限不变式)
    void trimToLimit();

    // 归并写盘单个分片(仅脏时真正写);flushAll 写全部脏分片
    bool flushShard(const ShardKey& key);
    bool flushAll();

    // 状态查询
    std::size_t loadedCount() const;
    std::size_t knownCount() const;
    std::vector<ShardKey> knownShards() const;

    // 「块 id -> ShardKey」路由目录
    void track(BlockId id, const ShardKey& key);
    void untrack(BlockId id);
    std::optional<ShardKey> shardOf(BlockId id) const;

    // 分片内的块 id 列表(manifest 内容;未知分片返回空列表)
    const std::vector<BlockId>& idsOf(const ShardKey& key) const;

    // 下沉集合:全局 LRU 淘汰出内存、只存在于盘上的块
    void markSunk(const ShardKey& key, BlockId id);
    void clearSunk(const ShardKey& key, BlockId id);
    bool isSunk(const ShardKey& key, BlockId id) const;

    // 版本计数(manifest 展示用,flush / manifest 恢复时更新)
    std::uint32_t versionCountOf(const ShardKey& key) const;

    // 从 manifest 恢复已知分片清单与路由目录(构造恢复用)
    void loadManifest(const ManifestData& manifest);

private:
    struct KnownEntry {
        std::vector<BlockId> ids;            // 该分片全部块 id(含仅历史留存)
        std::uint32_t        versionCount = 0;
    };
    struct LoadedEntry {
        bool dirty = false;                       // 脏标记
        std::list<ShardKey>::iterator lruIt;      // LRU 链表位置
        std::unordered_set<BlockId> sunk;         // 已下沉(仅在盘上)的块
        int pins = 0;                             // 钉住计数(>0 不参与换出)
    };

    // 换出最冷的未钉住分片:脏则先 flush,再从内存移除;
    // 全部被钉住时返回 false(调用方决定是否超上限继续加载)
    bool evictColdest();

    std::string   dir_;        // 数据目录
    std::size_t   maxLoaded_;  // 内存分片上限
    Callbacks     cbs_;        // 服务层协作回调

    std::unordered_map<ShardKey, KnownEntry, ShardKeyHash> known_;    // 已知分片
    std::unordered_map<ShardKey, LoadedEntry, ShardKeyHash> loaded_;  // 已加载分片
    std::list<ShardKey> lru_;                                 // 已加载分片 LRU(前端 = 最热)
    std::unordered_map<BlockId, ShardKey> shardOf_;           // 块 id -> 分片路由目录
};

}  // namespace stmb
