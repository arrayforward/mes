// ============================================================================
// 文件: tests/store/test_block_store.cpp
// 模块: stmb_tests(BlockStore 单元测试)
// 覆盖范围:
//   1. 基本操作:put / get / contains / remove / size,含未命中与无效删除;
//   2. 容量满时的 LRU 淘汰顺序(最久未使用者优先被淘汰);
//   3. get 命中会刷新 LRU 位置,改变后续淘汰对象;
//   4. put 触发淘汰时返回被淘汰块的完整数据(供服务层清理索引);
//   5. 重复 put 同一 id:更新语义(覆盖数据、不增块数、不触发淘汰、刷新 LRU);
//   6. 主动 evict 与累计淘汰计数 evictCount。
// 测试思路: 每个主题一个测试函数,用显式 id 构造块以便断言淘汰对象;
//           #undef NDEBUG 保证 Release 构建下断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "block_store.h"

#include <cassert>
#include <iostream>
#include <string>

namespace {

using namespace stmb;

// 伪代码:
//   1. 以给定 id 与负载构造一个最小 MemoryBlock(区域/时间戳不参与本层逻辑);
//   2. 返回构造好的块。
MemoryBlock makeBlock(BlockId id, const std::string& payload) {
    MemoryBlock b;
    b.id = id;
    b.payload = payload;
    return b;
}

// 伪代码:
//   1. 构造容量 3 的 store,断言初始 size/evictCount 为 0;
//   2. put 两个块,断言 size、contains 符合预期,get 未命中返回空;
//   3. get 命中断言负载正确;
//   4. remove 命中返回 true 且 size 减一,remove 不存在的 id 返回 false。
void testBasicOperations() {
    BlockStore store(3);
    assert(store.size() == 0 && store.capacity() == 3 && store.evictCount() == 0);

    assert(!store.put(makeBlock(1, "one")).has_value());
    assert(!store.put(makeBlock(2, "two")).has_value());
    assert(store.size() == 2);
    assert(store.contains(1) && store.contains(2) && !store.contains(99));
    assert(!store.get(99).has_value());

    const auto got = store.get(1);
    assert(got.has_value() && got->payload == "one");

    assert(store.remove(2) && !store.contains(2) && store.size() == 1);
    assert(!store.remove(2));
    std::cout << "[PASS] block_store: put/get/contains/remove/size 基本操作\n";
}

// 伪代码:
//   1. 构造容量 2 的 store,依次 put 块 1、块 2;
//   2. put 块 3:断言触发淘汰且被淘汰的是最久未使用的块 1;
//   3. 断言被淘汰块的完整数据(id 与 payload)随 put 返回;
//   4. 再 put 块 4:断言淘汰块 2(此时最久未使用);
//   5. 断言 evictCount 累计为 2。
void testLruEvictionOrder() {
    BlockStore store(2);
    store.put(makeBlock(1, "one"));
    store.put(makeBlock(2, "two"));

    const auto evicted1 = store.put(makeBlock(3, "three"));
    assert(evicted1.has_value() && evicted1->id == 1 && evicted1->payload == "one");
    assert(!store.contains(1) && store.contains(2) && store.contains(3));

    const auto evicted2 = store.put(makeBlock(4, "four"));
    assert(evicted2.has_value() && evicted2->id == 2);
    assert(store.evictCount() == 2);
    std::cout << "[PASS] block_store: 容量满时 LRU 淘汰顺序与返回数据\n";
}

// 伪代码:
//   1. 容量 2 的 store 写入块 1、块 2;
//   2. get(1) 刷新块 1 为最近使用;
//   3. put 块 3:断言被淘汰的是块 2 而非块 1(淘汰顺序因访问而改变);
//   4. 断言块 1、3 仍在库中。
void testGetRefreshesLru() {
    BlockStore store(2);
    store.put(makeBlock(1, "one"));
    store.put(makeBlock(2, "two"));

    assert(store.get(1).has_value());  // 刷新块 1
    const auto evicted = store.put(makeBlock(3, "three"));
    assert(evicted.has_value() && evicted->id == 2);
    assert(store.contains(1) && store.contains(3) && !store.contains(2));
    std::cout << "[PASS] block_store: get 命中刷新 LRU 位置\n";
}

// 伪代码:
//   1. 容量 2 的 store 写入块 1、块 2;
//   2. 以同 id=1、新负载重复 put:断言返回空(无淘汰)、size 不变、
//      evictCount 为 0,且 get(1) 读到更新后的负载;
//   3. 重复 put 同时刷新了块 1 的 LRU 位置,故 put 块 3 时淘汰块 2。
void testPutSameIdUpdates() {
    BlockStore store(2);
    store.put(makeBlock(1, "one"));
    store.put(makeBlock(2, "two"));

    assert(!store.put(makeBlock(1, "one-v2")).has_value());
    assert(store.size() == 2 && store.evictCount() == 0);
    const auto got = store.get(1);
    assert(got.has_value() && got->payload == "one-v2");

    const auto evicted = store.put(makeBlock(3, "three"));
    assert(evicted.has_value() && evicted->id == 2);
    std::cout << "[PASS] block_store: 重复 put 同 id 的更新语义\n";
}

// 伪代码:
//   1. 容量 2 的 store 写入块 1、块 2;
//   2. 主动 evict:断言返回最久未使用的块 1,size 减一;
//   3. 再 evict 两次:第二次(空库)断言返回空;
//   4. 断言 evictCount 累计为 2。
void testExplicitEvict() {
    BlockStore store(2);
    store.put(makeBlock(1, "one"));
    store.put(makeBlock(2, "two"));

    const auto victim = store.evict();
    assert(victim.has_value() && *victim == 1 && store.size() == 1);
    assert(store.evict().has_value());
    assert(!store.evict().has_value());
    assert(store.evictCount() == 2);
    std::cout << "[PASS] block_store: 主动 evict 与淘汰计数\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用五个测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testBasicOperations();
    testLruEvictionOrder();
    testGetRefreshesLru();
    testPutSameIdUpdates();
    testExplicitEvict();
    std::cout << "ALL TESTS PASS (block_store)\n";
    return 0;
}
