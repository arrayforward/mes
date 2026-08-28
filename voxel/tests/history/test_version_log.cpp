// ============================================================================
// 文件: tests/history/test_version_log.cpp
// 模块: stmb_tests(VersionLog 单元测试)
// 覆盖范围:
//   1. archive / historyOf:多版本按版本号升序返回,字段完整;
//   2. seal:封存当前生效版本(填 validTo),重复封存不覆盖首次结束时刻;
//   3. getAt 时间回溯边界:validFrom 含、validTo 不含、当前生效版本
//      (validTo 空)对任意晚近时刻命中、落在任何版本区间之外返回空;
//   4. 未知 id:historyOf 返回空、getAt 返回空;
//   5. drop:清除历史后不可再回溯;trackedCount 随之变化。
// 测试思路: 每个主题一个测试函数,逐条 assert;#undef NDEBUG 保证
//           Release 构建下断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "version_log.h"

#include <cassert>
#include <iostream>

namespace {

using namespace stmb;

// 伪代码:
//   1. 按给定版本号/负载/生效区间组装一个 BlockVersion;
//   2. validTo 用 optional 表达,调用方传空表示「当前生效」;
//   3. 返回组装好的版本快照。
BlockVersion makeVersion(std::uint32_t version, const std::string& payload,
                         TimeStamp validFrom, std::optional<TimeStamp> validTo) {
    BlockVersion v;
    v.version = version;
    v.payload = payload;
    v.confidence = 0.9;
    v.state = BlockState::Stable;
    v.validFrom = validFrom;
    v.validTo = validTo;
    return v;
}

// 伪代码:
//   1. 空日志对未知 id:断言 historyOf 为空、getAt 返回空、trackedCount 为 0;
//   2. 对块 1 依次归档 v1/v2/v3(模拟两次 seal + archive 的完整变更链);
//   3. 断言 historyOf 返回 3 个版本且按版本号升序、字段正确;
//   4. 对块 2 归档独立版本,断言两块历史互不干扰、trackedCount 为 2。
void testArchiveAndHistoryOf() {
    VersionLog log;
    assert(log.historyOf(42).empty());
    assert(!log.getAt(42, 0).has_value());
    assert(log.trackedCount() == 0);

    log.archive(1, makeVersion(1, "v1", 0, std::nullopt));
    log.seal(1, 1000);
    log.archive(1, makeVersion(2, "v2", 1000, std::nullopt));
    log.seal(1, 2000);
    log.archive(1, makeVersion(3, "v3", 2000, std::nullopt));

    const auto history = log.historyOf(1);
    assert(history.size() == 3);
    assert(history[0].version == 1 && history[0].payload == "v1");
    assert(history[1].version == 2 && history[1].payload == "v2");
    assert(history[2].version == 3 && history[2].payload == "v3");
    assert(history[0].validTo.has_value() && *history[0].validTo == 1000);
    assert(!history[2].validTo.has_value());  // 当前生效版本

    log.archive(2, makeVersion(1, "other", 0, std::nullopt));
    assert(log.historyOf(2).size() == 1 && log.historyOf(1).size() == 3);
    assert(log.trackedCount() == 2);
    std::cout << "[PASS] version_log: archive / historyOf 升序与字段\n";
}

// 伪代码:
//   1. 构造 v1[0,1000)、v2[1000,2000)、v3[2000, 至今) 的版本链;
//   2. 边界命中:t=0 命中 v1(validFrom 含),t=1000 命中 v2(validTo 不含),
//      t=1999 命中 v2,t=2000 命中 v3;
//   3. 当前生效版本:t=9999 命中 v3;
//   4. 区间之外:t=-1 返回空;
//   5. 断言 getAt 返回的字段(版本号与负载)正确。
void testGetAtBoundaries() {
    VersionLog log;
    log.archive(1, makeVersion(1, "v1", 0, std::nullopt));
    log.seal(1, 1000);
    log.archive(1, makeVersion(2, "v2", 1000, std::nullopt));
    log.seal(1, 2000);
    log.archive(1, makeVersion(3, "v3", 2000, std::nullopt));

    assert(log.getAt(1, 0).value().payload == "v1");        // validFrom 含
    assert(log.getAt(1, 999).value().payload == "v1");
    assert(log.getAt(1, 1000).value().payload == "v2");     // validTo 不含
    assert(log.getAt(1, 1999).value().payload == "v2");
    assert(log.getAt(1, 2000).value().payload == "v3");
    assert(log.getAt(1, 9999).value().payload == "v3");     // 当前生效
    assert(!log.getAt(1, -1).has_value());                  // 区间之外
    std::cout << "[PASS] version_log: getAt 时间回溯边界\n";
}

// 伪代码:
//   1. 归档后 seal 两次:断言第一次的 validTo 不被第二次覆盖;
//   2. 对未知 id seal:断言无副作用。
void testSeal() {
    VersionLog log;
    log.archive(1, makeVersion(1, "v1", 0, std::nullopt));
    log.seal(1, 1000);
    log.seal(1, 5000);  // 重复封存不应覆盖
    const auto history = log.historyOf(1);
    assert(history[0].validTo.has_value() && *history[0].validTo == 1000);

    log.seal(99, 1000);  // 未知 id,无崩溃无副作用
    assert(log.trackedCount() == 1);
    std::cout << "[PASS] version_log: seal 封存语义\n";
}

// 伪代码:
//   1. 对块 1、2 各归档一个版本,断言 trackedCount 为 2;
//   2. drop(1):断言 historyOf(1) 为空、getAt 不再命中、trackedCount 减一;
//   3. 块 2 的历史不受影响;
//   4. 再次 drop(1):断言无副作用。
void testDrop() {
    VersionLog log;
    log.archive(1, makeVersion(1, "v1", 0, std::nullopt));
    log.archive(2, makeVersion(1, "other", 0, std::nullopt));
    assert(log.trackedCount() == 2);

    log.drop(1);
    assert(log.historyOf(1).empty());
    assert(!log.getAt(1, 0).has_value());
    assert(log.trackedCount() == 1);
    assert(log.historyOf(2).size() == 1);

    log.drop(1);
    assert(log.trackedCount() == 1);
    std::cout << "[PASS] version_log: drop 清理历史\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用四个测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testArchiveAndHistoryOf();
    testGetAtBoundaries();
    testSeal();
    testDrop();
    std::cout << "ALL TESTS PASS (version_log)\n";
    return 0;
}
