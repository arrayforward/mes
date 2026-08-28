// ============================================================================
// 文件: block_store.cpp
// 模块: stmb_store(存储层,依赖 core)
// 用途: 实现 BlockStore 的经典 LRU 逻辑(链表 + 哈希表)与线程安全封装。
// 设计思路:
//   1. 所有公开方法一进函数就用 lock_guard 锁住互斥锁,函数体即临界区;
//   2. 「提到链表前端」的提操作用 splice 实现,O(1) 且不使其他迭代器失效;
//   3. put 的淘汰路径要先取出被淘汰块的完整数据再擦除,因为服务层需要
//      用这份数据同步清理空间索引与时间索引;
//   4. evictCount_ 只在真正发生淘汰时递增,供 stats() 上报。
// 架构角色: BlockStore 的唯一实现文件。
// ============================================================================
#include "block_store.h"

namespace stmb {

// 伪代码:
//   1. 保存 capacity 到成员变量(构造函数体为空,仅做成员初始化)。
BlockStore::BlockStore(std::size_t capacity)
    : capacity_(capacity) {}

// 伪代码:
//   1. 加锁;
//   2. 若 id 已存在:更新块数据,用 splice 把对应节点提到链表前端,返回空;
//   3. 若是新块且当前已满:取链表尾部 id,从 map 中取出该块的完整数据,
//      擦除其 map 项与链表节点,evictCount_ 加一,记录为待返回的被淘汰块;
//   4. 把新 id 压入链表前端,并在 map 中登记 {块, 迭代器};
//   5. 返回被淘汰块(若有)。
std::optional<MemoryBlock> BlockStore::put(const MemoryBlock& block) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(block.id);
    if (it != entries_.end()) {
        it->second.block = block;
        lruList_.splice(lruList_.begin(), lruList_, it->second.lruIt);
        return std::nullopt;
    }

    std::optional<MemoryBlock> evicted;
    if (entries_.size() >= capacity_ && !lruList_.empty()) {
        const BlockId victimId = lruList_.back();
        auto victimIt = entries_.find(victimId);
        evicted = victimIt->second.block;
        entries_.erase(victimIt);
        lruList_.pop_back();
        ++evictCount_;
    }

    lruList_.push_front(block.id);
    entries_.emplace(block.id, Entry{block, lruList_.begin()});
    return evicted;
}

// 伪代码:
//   1. 加锁;
//   2. 在 map 中查找 id,未命中返回空;
//   3. 命中则用 splice 把节点提到链表前端(刷新 LRU),返回块副本。
std::optional<MemoryBlock> BlockStore::get(BlockId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    lruList_.splice(lruList_.begin(), lruList_, it->second.lruIt);
    return it->second.block;
}

// 伪代码:
//   1. 加锁;
//   2. 在 map 中查找 id,未命中返回 false;
//   3. 命中则先擦除链表节点(用保存的迭代器),再擦除 map 项,返回 true。
bool BlockStore::remove(BlockId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return false;
    }
    lruList_.erase(it->second.lruIt);
    entries_.erase(it);
    return true;
}

// 伪代码:
//   1. 加锁;
//   2. 返回 map 查找结果(只查不动,不刷新 LRU)。
bool BlockStore::contains(BlockId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(id) != entries_.end();
}

// 伪代码:
//   1. 加锁;
//   2. 返回 map 的大小。
std::size_t BlockStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

// 伪代码:
//   1. 返回容量上限(不可变成员,无需加锁)。
std::size_t BlockStore::capacity() const {
    return capacity_;
}

// 伪代码:
//   1. 加锁;
//   2. 返回累计淘汰计数。
std::uint64_t BlockStore::evictCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return evictCount_;
}

// 伪代码:
//   1. 加锁;
//   2. 若库为空,返回空;
//   3. 取链表尾部 id(最久未使用),擦除其 map 项与链表节点;
//   4. evictCount_ 加一,返回被淘汰的 BlockId。
std::optional<BlockId> BlockStore::evict() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lruList_.empty()) {
        return std::nullopt;
    }
    const BlockId victimId = lruList_.back();
    entries_.erase(victimId);
    lruList_.pop_back();
    ++evictCount_;
    return victimId;
}

// 伪代码:
//   1. 加锁;
//   2. 遍历 map,把每个 Entry 中的块副本依次放入结果 vector;
//   3. 返回结果(顺序不保证,供持久化快照全量落盘用)。
std::vector<MemoryBlock> BlockStore::blocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MemoryBlock> result;
    result.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) {
        result.push_back(entry.block);
    }
    return result;
}

// 伪代码:
//   1. 加锁;
//   2. 在 map 中查找 id,未命中返回空;命中返回块副本(不动 LRU 链表)。
std::optional<MemoryBlock> BlockStore::peek(BlockId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second.block;
}

}  // namespace stmb
