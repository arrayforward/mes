// ============================================================================
// 文件: shard_manager.cpp
// 模块: stmb_shard(分片管理,依赖 core / history / persistence)
// 用途: 实现 ShardManager 的加载、换出、脏标记、归并写盘与路由目录。
// 设计思路:
//   1. 已加载分片用「链表 + 哈希表」的经典 LRU 管理,前端为最热;
//   2. ensureLoaded 是唯一的加载入口:未命中 -> 按需 evictColdest 腾出位置
//      -> 读分片文件 -> 回调 onLoad 注入内存组件;
//   3. 换出与 flush 都走 onMerge 回调:服务层把内存现状(含全局 LRU 下沉块
//      从盘上旧数据取回)归并成完整分片内容,ShardManager 只负责落盘;
//   4. 路由目录 shardOf_ 对「已淘汰但历史保留」的块同样有效,保证
//      get/confirm/remove 等按 id 操作总能定位分片。
// 架构角色: ShardManager 的唯一实现文件。
// ============================================================================
#include "shard_manager.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace stmb {

// 伪代码:
//   1. 保存目录与回调;maxLoaded 为 0 时按 1 处理(至少允许一个分片驻留)。
ShardManager::ShardManager(std::string dir, std::size_t maxLoaded, Callbacks cbs)
    : dir_(std::move(dir)), maxLoaded_(maxLoaded == 0 ? 1 : maxLoaded),
      cbs_(std::move(cbs)) {}

// 伪代码:
//   1. 已加载:把该分片提到 LRU 前端,返回 true;
//   2. 未加载:当已加载数达到上限时,反复 evictColdest 腾位(失败返回 false);
//   3. 登记一个干净的空 LoadedEntry 并置于 LRU 前端;
//   4. 读分片文件:损坏抛 runtime_error;不存在得到空分片;
//   5. 回调 onLoad 把数据注入内存组件,返回 true。
bool ShardManager::ensureLoaded(const ShardKey& key) {
    auto it = loaded_.find(key);
    if (it != loaded_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.lruIt);
        return true;
    }
    while (loaded_.size() >= maxLoaded_) {
        if (!evictColdest()) {
            break;  // 全部已加载分片都被钉住:允许暂时超过上限
        }
    }
    lru_.push_front(key);
    loaded_.emplace(key, LoadedEntry{false, lru_.begin(), {}, 0});
    std::optional<ShardData> data = PersistenceManager::loadShardFile(dir_, key);
    if (!data.has_value()) {
        throw std::runtime_error("stmb: 分片文件损坏: " + dir_ + "/shards/" +
                                 PersistenceManager::shardFileName(key));
    }
    cbs_.onLoad(key, *data);
    return true;
}

// 伪代码:
//   1. 若分片不在已知清单,登记一条空的 KnownEntry。
void ShardManager::addKnown(const ShardKey& key) {
    known_.emplace(key, KnownEntry{});
}

// 伪代码:
//   1. 返回 known_ 中是否存在该分片。
bool ShardManager::isKnown(const ShardKey& key) const {
    return known_.find(key) != known_.end();
}

// 伪代码:
//   1. 返回 loaded_ 中是否存在该分片。
bool ShardManager::isLoaded(const ShardKey& key) const {
    return loaded_.find(key) != loaded_.end();
}

// 伪代码:
//   1. 仅当分片已加载时置脏标记(未加载分片的盘上文件即最新,无需标记)。
void ShardManager::markDirty(const ShardKey& key) {
    auto it = loaded_.find(key);
    if (it != loaded_.end()) {
        it->second.dirty = true;
    }
}

// 伪代码:
//   1. 仅当分片已加载时,钉住计数 +1。
void ShardManager::pin(const ShardKey& key) {
    auto it = loaded_.find(key);
    if (it != loaded_.end()) {
        ++it->second.pins;
    }
}

// 伪代码:
//   1. 仅当分片已加载且计数大于 0 时,钉住计数 -1。
void ShardManager::unpin(const ShardKey& key) {
    auto it = loaded_.find(key);
    if (it != loaded_.end() && it->second.pins > 0) {
        --it->second.pins;
    }
}

// 伪代码:
//   1. 分片未加载或不脏:直接返回 true(盘上文件即最新);
//   2. 读盘上旧数据(不存在 = 空;损坏抛异常);
//   3. 回调 onMerge 让服务层用内存现状覆盖归并出完整内容;
//   4. 写分片文件,成功后更新版本计数并清除脏标记。
bool ShardManager::flushShard(const ShardKey& key) {
    auto it = loaded_.find(key);
    if (it == loaded_.end() || !it->second.dirty) {
        return true;
    }
    std::optional<ShardData> old = PersistenceManager::loadShardFile(dir_, key);
    if (!old.has_value()) {
        throw std::runtime_error("stmb: 分片文件损坏: " + dir_ + "/shards/" +
                                 PersistenceManager::shardFileName(key));
    }
    ShardData merged = cbs_.onMerge(key, std::move(*old));
    if (!PersistenceManager::saveShardFile(dir_, key, merged)) {
        return false;
    }
    known_[key].versionCount = static_cast<std::uint32_t>(merged.versions.size());
    it->second.dirty = false;
    return true;
}

// 伪代码:
//   1. 收集全部已加载分片键(拷贝,避免遍历中修改);
//   2. 逐 flushShard,任一失败返回 false;
//   3. 全部成功返回 true。
bool ShardManager::flushAll() {
    std::vector<ShardKey> keys;
    keys.reserve(loaded_.size());
    for (const auto& [key, entry] : loaded_) {
        keys.push_back(key);
    }
    for (const ShardKey& key : keys) {
        if (!flushShard(key)) {
            return false;
        }
    }
    return true;
}

// 伪代码:
//   1. 各状态查询:直接返回对应容器大小 / 键列表副本。
std::size_t ShardManager::loadedCount() const {
    return loaded_.size();
}

std::size_t ShardManager::knownCount() const {
    return known_.size();
}

std::vector<ShardKey> ShardManager::knownShards() const {
    std::vector<ShardKey> keys;
    keys.reserve(known_.size());
    for (const auto& [key, entry] : known_) {
        keys.push_back(key);
    }
    return keys;
}

// 伪代码:
//   1. 记录 id -> 分片的映射;
//   2. 若 id 尚未在该分片的 id 列表中,追加(known_ 自动登记新分片)。
void ShardManager::track(BlockId id, const ShardKey& key) {
    shardOf_[id] = key;
    std::vector<BlockId>& ids = known_[key].ids;
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
        ids.push_back(id);
    }
}

// 伪代码:
//   1. 查路由目录,不存在则直接返回;
//   2. 从路由目录、该分片的 id 列表与(若已加载)下沉集合中移除该 id。
void ShardManager::untrack(BlockId id) {
    auto it = shardOf_.find(id);
    if (it == shardOf_.end()) {
        return;
    }
    KnownEntry& known = known_[it->second];
    std::vector<BlockId>& ids = known.ids;
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    auto loadedIt = loaded_.find(it->second);
    if (loadedIt != loaded_.end()) {
        loadedIt->second.sunk.erase(id);
    }
    shardOf_.erase(it);
}

// 伪代码:
//   1. 查路由目录返回 id 所属分片;不存在返回空。
std::optional<ShardKey> ShardManager::shardOf(BlockId id) const {
    auto it = shardOf_.find(id);
    if (it == shardOf_.end()) {
        return std::nullopt;
    }
    return it->second;
}

// 伪代码:
//   1. 返回已知分片的 id 列表;未知分片返回静态空列表。
const std::vector<BlockId>& ShardManager::idsOf(const ShardKey& key) const {
    static const std::vector<BlockId> kEmpty;
    auto it = known_.find(key);
    return it == known_.end() ? kEmpty : it->second.ids;
}

// 伪代码:
//   1. 仅当分片已加载时,把 id 加入其下沉集合。
void ShardManager::markSunk(const ShardKey& key, BlockId id) {
    auto it = loaded_.find(key);
    if (it != loaded_.end()) {
        it->second.sunk.insert(id);
    }
}

// 伪代码:
//   1. 仅当分片已加载时,把 id 从其下沉集合移除。
void ShardManager::clearSunk(const ShardKey& key, BlockId id) {
    auto it = loaded_.find(key);
    if (it != loaded_.end()) {
        it->second.sunk.erase(id);
    }
}

// 伪代码:
//   1. 分片已加载且 id 在其下沉集合中返回 true。
bool ShardManager::isSunk(const ShardKey& key, BlockId id) const {
    auto it = loaded_.find(key);
    return it != loaded_.end() && it->second.sunk.count(id) != 0;
}

// 伪代码:
//   1. 返回已知分片的版本计数;未知分片返回 0。
std::uint32_t ShardManager::versionCountOf(const ShardKey& key) const {
    auto it = known_.find(key);
    return it == known_.end() ? 0 : it->second.versionCount;
}

// 伪代码:
//   1. 遍历 manifest 分片清单:登记 KnownEntry(id 列表 + 版本计数);
//   2. 对每个 id 建立 id -> 分片的路由映射。
void ShardManager::loadManifest(const ManifestData& manifest) {
    for (const ManifestShardEntry& e : manifest.shards) {
        KnownEntry entry;
        entry.ids = e.blockIds;
        entry.versionCount = e.versionCount;
        known_[e.key] = std::move(entry);
        for (BlockId id : e.blockIds) {
            shardOf_[id] = e.key;
        }
    }
}

// 伪代码:
//   1. 当已加载数超过上限时,反复 evictColdest;
//   2. evictColdest 失败(理论上仅剩被钉住分片)则停止,避免死循环。
void ShardManager::trimToLimit() {
    while (loaded_.size() > maxLoaded_) {
        if (!evictColdest()) {
            break;
        }
    }
}

// 伪代码:
//   1. 从 LRU 链表尾部向前找第一个「未钉住」的已加载分片;全部被钉住返回
//      false(调用方决定是否超上限继续加载);
//   2. 该分片脏则 flushShard(归并写盘);
//   3. 回调 onUnload 让服务层把该分片内容从内存组件移除;
//   4. 从 loaded_ 与 LRU 链表 erase(known_ 与路由目录保留,数据已在盘上)。
bool ShardManager::evictColdest() {
    for (auto rit = lru_.rbegin(); rit != lru_.rend(); ++rit) {
        const ShardKey key = *rit;
        auto it = loaded_.find(key);
        if (it == loaded_.end() || it->second.pins > 0) {
            continue;
        }
        if (it->second.dirty && !flushShard(key)) {
            return false;
        }
        cbs_.onUnload(key);
        loaded_.erase(key);
        lru_.erase(std::next(rit).base());
        return true;
    }
    return false;
}

}  // namespace stmb
