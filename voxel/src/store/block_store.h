// ============================================================================
// 文件: block_store.h
// 模块: stmb_store(存储层,依赖 core)
// 用途: 声明 BlockStore —— 记忆块的唯一数据存放点,带容量上限与 LRU 淘汰。
// 设计思路:
//   1. 经典 LRU 结构:std::list<BlockId> 维护使用顺序(前端 = 最近使用),
//      unordered_map<BlockId, Entry{块数据, 链表迭代器}> 提供 O(1) 定位;
//   2. put 新块时若已满,先淘汰链表尾部(最久未使用)的块,并把被淘汰块的
//      完整数据返回给调用方 —— 服务层需要块数据(区域/时间戳)来同步清理索引,
//      因此 put 返回 optional<MemoryBlock> 而非仅 BlockId;
//   3. get 会刷新 LRU(把命中的 id 提到链表前端);
//   4. 所有公开方法用一把互斥锁保护,线程安全。
// 架构角色: 数据层,索引中只存 BlockId,真正的 MemoryBlock 只在这里存一份。
// 与其他模块关系: 依赖 core 的 MemoryBlock / BlockId;被 StmbService 组合。
// ============================================================================
#pragma once

#include "types.h"

#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace stmb {

class BlockStore {
public:
    // 构造:指定容量上限(最多存放的块数)
    explicit BlockStore(std::size_t capacity);

    // 写入块;若触发淘汰,返回被淘汰块的完整数据,否则返回空
    std::optional<MemoryBlock> put(const MemoryBlock& block);

    // 按 id 读取块;命中则刷新 LRU 并返回块副本,未命中返回空
    std::optional<MemoryBlock> get(BlockId id);

    // 按 id 读取块副本但不刷新 LRU(只读访问)
    std::optional<MemoryBlock> peek(BlockId id) const;

    // 按 id 删除块;返回是否删除成功
    bool remove(BlockId id);

    // 判断 id 是否存在(不刷新 LRU)
    bool contains(BlockId id) const;

    // 当前块数 / 容量上限 / 累计淘汰次数
    std::size_t size() const;
    std::size_t capacity() const;
    std::uint64_t evictCount() const;

    // 返回库内全部块的副本(持久化快照用)
    std::vector<MemoryBlock> blocks() const;

    // 主动淘汰最久未使用的块,返回其 BlockId;空库返回空
    std::optional<BlockId> evict();

private:
    // Entry:块数据 + 指向其在 LRU 链表中位置的迭代器
    struct Entry {
        MemoryBlock block;
        std::list<BlockId>::iterator lruIt;
    };

    std::size_t capacity_;                          // 容量上限
    std::list<BlockId> lruList_;                    // LRU 链表:前端 = 最近使用
    std::unordered_map<BlockId, Entry> entries_;    // id -> {块, 链表迭代器}
    std::uint64_t evictCount_ = 0;                  // 累计淘汰计数
    mutable std::mutex mutex_;                      // 保护以上全部成员
};

}  // namespace stmb
