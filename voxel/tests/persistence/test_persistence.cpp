// ============================================================================
// 文件: tests/persistence/test_persistence.cpp
// 模块: stmb_tests(PersistenceManager 单元测试 + 服务恢复端到端)
// 覆盖范围:
//   1. 快照 round-trip:块(含状态机/候选字段)与版本记录落盘后读回完全一致;
//   2. WAL 追加与重放:多类型记录 replay 后顺序、内容一致;
//   3. checkpoint:wal.log 被截断(只余文件头),快照含全量;
//   4. 模拟 crash:wal.log 尾部追加垃圾字节/半条记录/CRC 损坏,replay
//      保留有效前缀且不崩溃;magic 错误返回空 optional;
//   5. 服务端到端恢复:服务 A 写入 + 状态机全套操作 + checkpoint + 再写入,
//      同目录新建服务 B,验证 query / 状态 / 版本链 / getAt / nextId 延续。
// 测试思路: 临时目录用 std::filesystem::temp_directory_path 下的专属子目录,
//           每个用例先 remove_all 再打平比较;#undef NDEBUG 保证 Release 下
//           断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "persistence.h"
#include "stmb_service.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using namespace stmb;
namespace fs = std::filesystem;

// 伪代码:
//   1. 在系统临时目录下构造 tests 专属子目录路径;
//   2. 先 remove_all 清掉可能的历史残留,返回路径字符串。
std::string freshDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / "stmb_tests" / name;
    fs::remove_all(dir);
    return dir.string();
}

// 伪代码:
//   1. 以中心坐标构造 AABB(半边长 2),填充负载/时间戳/置信度;
//   2. 返回构造好的块(模拟服务写入后的完整字段由调用方按需补)。
MemoryBlock makeBlock(double cx, double cy, double cz,
                      TimeStamp ts, const std::string& payload) {
    MemoryBlock b;
    b.region.min = {cx - 2.0, cy - 2.0, cz - 2.0};
    b.region.max = {cx + 2.0, cy + 2.0, cz + 2.0};
    b.payload = payload;
    b.timestamp = ts;
    return b;
}

// 伪代码:
//   1. 逐字段比较两个 MemoryBlock:id/key/region/payload/timestamp/version/
//      lastAccess/state/confidence/confirmations/lastUpdate/两个候选字段;
//   2. 任何字段不一致立即断言失败。
void assertBlockEqual(const MemoryBlock& a, const MemoryBlock& b) {
    assert(a.id == b.id);
    assert(a.key.cellX == b.key.cellX && a.key.cellY == b.key.cellY);
    assert(a.key.cellZ == b.key.cellZ && a.key.timeSlot == b.key.timeSlot);
    assert(a.region.min == b.region.min && a.region.max == b.region.max);
    assert(a.payload == b.payload);
    assert(a.timestamp == b.timestamp && a.version == b.version);
    assert(a.lastAccess == b.lastAccess);
    assert(a.state == b.state);
    assert(a.confidence == b.confidence);
    assert(a.confirmations == b.confirmations);
    assert(a.lastUpdate == b.lastUpdate);
    assert(a.pendingPayload == b.pendingPayload);
    assert(a.pendingConfidence == b.pendingConfidence);
}

// 伪代码:
//   1. 逐字段比较两个 BlockVersion:版本号/负载/置信度/状态/生效区间。
void assertVersionEqual(const BlockVersion& a, const BlockVersion& b) {
    assert(a.version == b.version && a.payload == b.payload);
    assert(a.confidence == b.confidence && a.state == b.state);
    assert(a.validFrom == b.validFrom && a.validTo == b.validTo);
}

// 伪代码(快照 round-trip):
//   1. 构造两个块(一个带完整状态机字段与候选变更,一个用默认值);
//   2. 构造两条版本记录(一封存一当前生效);
//   3. saveSnapshot 后 loadSnapshot,断言块数/版本数/nextId 一致;
//   4. 逐字段比较块与版本;
//   5. 对不存在快照的目录断言返回空数据(非错误)。
void testSnapshotRoundTrip() {
    const std::string dir = freshDir("snapshot");

    MemoryBlock b1 = makeBlock(5.0, 5.0, 5.0, 100, "alpha");
    b1.id = 7;
    b1.key = BlockKey{0, 0, 0, 0};
    b1.version = 2;
    b1.lastAccess = 500;
    b1.state = BlockState::Changing;
    b1.confidence = 0.8;
    b1.confirmations = 3;
    b1.lastUpdate = 400;
    b1.pendingPayload = std::string("alpha-v3");
    b1.pendingConfidence = 0.6;

    MemoryBlock b2 = makeBlock(-15.0, 25.0, 5.0, 200, "bravo");
    b2.id = 9;
    b2.key = BlockKey{-2, 2, 0, 0};
    b2.version = 1;
    b2.state = BlockState::Pending;
    b2.confidence = 0.5;

    BlockVersion v1;
    v1.version = 1; v1.payload = "alpha"; v1.confidence = 0.8;
    v1.state = BlockState::Stable; v1.validFrom = 100; v1.validTo = 400;
    BlockVersion v2;
    v2.version = 2; v2.payload = "alpha-v2"; v2.confidence = 0.7;
    v2.state = BlockState::Stable; v2.validFrom = 400; v2.validTo = std::nullopt;

    assert(PersistenceManager::saveSnapshot(dir, {b1, b2},
                                            {{7, v1}, {7, v2}}, 42));
    const auto loaded = PersistenceManager::loadSnapshot(dir);
    assert(loaded.has_value());
    assert(loaded->blocks.size() == 2 && loaded->versions.size() == 2);
    assert(loaded->nextId == 42);
    assertBlockEqual(loaded->blocks[0], b1);
    assertBlockEqual(loaded->blocks[1], b2);
    assert(loaded->versions[0].first == 7);
    assertVersionEqual(loaded->versions[0].second, v1);
    assertVersionEqual(loaded->versions[1].second, v2);

    const auto empty = PersistenceManager::loadSnapshot(freshDir("nonexistent"));
    assert(empty.has_value() && empty->blocks.empty() && empty->nextId == 1);

    fs::remove_all(dir);
    std::cout << "[PASS] persistence: 快照 round-trip 字段一致\n";
}

// 伪代码(WAL 追加与重放):
//   1. 依次追加 PutBlock / Confirm / ReportChange / Remove / ExpireBefore
//      五种记录;
//   2. replayWal:断言返回 5 条、顺序一致、各字段内容一致;
//   3. 对不存在 WAL 的目录断言返回空列表(非错误)。
void testWalAppendAndReplay() {
    const std::string dir = freshDir("wal");

    WalRecord r1;
    r1.type = RecordType::PutBlock;
    r1.block = makeBlock(5.0, 5.0, 5.0, 100, "alpha");
    r1.block.id = 1;
    WalRecord r2;
    r2.type = RecordType::Confirm; r2.id = 1; r2.ts = 200;
    WalRecord r3;
    r3.type = RecordType::ReportChange; r3.id = 1;
    r3.payload = "alpha-v2"; r3.confidence = 0.9; r3.ts = 300;
    WalRecord r4;
    r4.type = RecordType::Remove; r4.id = 2;
    WalRecord r5;
    r5.type = RecordType::ExpireBefore; r5.ts = 1000;

    assert(PersistenceManager::appendWal(dir, r1));
    assert(PersistenceManager::appendWal(dir, r2));
    assert(PersistenceManager::appendWal(dir, r3));
    assert(PersistenceManager::appendWal(dir, r4));
    assert(PersistenceManager::appendWal(dir, r5));

    const auto records = PersistenceManager::replayWal(dir);
    assert(records.has_value() && records->size() == 5);
    assert(records->at(0).type == RecordType::PutBlock);
    assertBlockEqual(records->at(0).block, r1.block);
    assert(records->at(1).type == RecordType::Confirm &&
           records->at(1).id == 1 && records->at(1).ts == 200);
    assert(records->at(2).type == RecordType::ReportChange &&
           records->at(2).payload == "alpha-v2" &&
           records->at(2).confidence == 0.9 && records->at(2).ts == 300);
    assert(records->at(3).type == RecordType::Remove && records->at(3).id == 2);
    assert(records->at(4).type == RecordType::ExpireBefore &&
           records->at(4).ts == 1000);

    const auto empty = PersistenceManager::replayWal(freshDir("no-wal"));
    assert(empty.has_value() && empty->empty());

    fs::remove_all(dir);
    std::cout << "[PASS] persistence: WAL 追加与重放一致\n";
}

// 伪代码(checkpoint 截断):
//   1. 先追加两条 WAL 记录,断言 replay 有 2 条;
//   2. checkpoint(落 1 块 + 1 版本):断言 wal.log 被截断(replay 为空),
//      快照含全量(1 块 + 1 版本 + nextId);
//   3. checkpoint 后再追加一条 WAL:断言 replay 只有新记录(日志从断点续写)。
void testCheckpointTruncatesWal() {
    const std::string dir = freshDir("checkpoint");

    WalRecord r;
    r.type = RecordType::Confirm; r.id = 1; r.ts = 100;
    assert(PersistenceManager::appendWal(dir, r));
    assert(PersistenceManager::appendWal(dir, r));
    assert(PersistenceManager::replayWal(dir)->size() == 2);

    MemoryBlock b = makeBlock(5.0, 5.0, 5.0, 100, "alpha");
    b.id = 1;
    BlockVersion v;
    v.version = 1; v.payload = "alpha"; v.validFrom = 100;
    assert(PersistenceManager::checkpoint(dir, {b}, {{1, v}}, 2));

    assert(PersistenceManager::replayWal(dir)->empty());  // WAL 已截断
    const auto loaded = PersistenceManager::loadSnapshot(dir);
    assert(loaded.has_value() && loaded->blocks.size() == 1);
    assert(loaded->versions.size() == 1 && loaded->nextId == 2);

    assert(PersistenceManager::appendWal(dir, r));          // 断点续写
    assert(PersistenceManager::replayWal(dir)->size() == 1);

    fs::remove_all(dir);
    std::cout << "[PASS] persistence: checkpoint 截断 WAL 且快照含全量\n";
}

// 伪代码(模拟 crash 容错):
//   1. 追加 3 条合法记录后,向 wal.log 尾部追加若干垃圾字节(半条记录):
//      replay 断言只返回前 3 条,不崩溃;
//   2. 重新构造目录:写 2 条合法记录后,把第 2 条的 payload 改一个字节
//      (CRC 不再匹配):replay 断言只保留第 1 条;
//   3. 构造 magic 错误的文件:replay 断言返回空 optional(报错)。
void testCrashTolerance() {
    const std::string dir = freshDir("crash");

    WalRecord r;
    r.type = RecordType::Confirm; r.id = 1; r.ts = 100;
    assert(PersistenceManager::appendWal(dir, r));
    r.ts = 200;
    assert(PersistenceManager::appendWal(dir, r));
    r.ts = 300;
    assert(PersistenceManager::appendWal(dir, r));
    {
        // 模拟 crash 写一半:尾部追加半条记录(类型 + 超长长度 + 少量字节)
        std::ofstream out(dir + "/wal.log", std::ios::binary | std::ios::app);
        const char garbage[] = {static_cast<char>(2), static_cast<char>(0xFF),
                                static_cast<char>(0xFF), static_cast<char>(0x7F),
                                static_cast<char>(0x00), static_cast<char>(0xAA)};
        out.write(garbage, sizeof(garbage));
    }
    const auto prefix = PersistenceManager::replayWal(dir);
    assert(prefix.has_value() && prefix->size() == 3);
    assert(prefix->at(2).ts == 300);

    const std::string dir2 = freshDir("crash-crc");
    r.ts = 100;
    assert(PersistenceManager::appendWal(dir2, r));
    r.ts = 200;
    assert(PersistenceManager::appendWal(dir2, r));
    {
        // 破坏第 2 条记录:翻转文件最后一个字节(即第 2 条帧的 CRC 末字节),
        // 使该帧 CRC 校验失败,replay 应只保留第 1 条
        const auto size = fs::file_size(dir2 + "/wal.log");
        std::fstream io(dir2 + "/wal.log",
                        std::ios::binary | std::ios::in | std::ios::out);
        io.seekg(static_cast<std::streamoff>(size) - 1);
        char byte = 0;
        io.get(byte);
        io.seekp(static_cast<std::streamoff>(size) - 1);
        io.put(static_cast<char>(byte ^ 0xFF));
    }
    const auto afterCorrupt = PersistenceManager::replayWal(dir2);
    assert(afterCorrupt.has_value() && afterCorrupt->size() == 1);
    assert(afterCorrupt->at(0).ts == 100);

    const std::string dir3 = freshDir("crash-magic");
    fs::create_directories(dir3);  // appendWal 会自建目录,裸写文件需要手动建
    {
        std::ofstream out(dir3 + "/wal.log", std::ios::binary | std::ios::trunc);
        const char bad[] = {'B', 'A', 'D', '!', 1, 0, 0, 0};
        out.write(bad, sizeof(bad));
    }
    assert(!PersistenceManager::replayWal(dir3).has_value());

    fs::remove_all(dir);
    fs::remove_all(dir2);
    fs::remove_all(dir3);
    std::cout << "[PASS] persistence: crash 尾部容错保留有效前缀\n";
}

// 伪代码(服务端到端恢复):
//   1. 服务 A(开启 dataDir,确认阈值 2):写入 2 块,confirm 两次转 Stable,
//      reportChange + confirm 完成一次变更(v2),checkpoint;
//   2. checkpoint 后再写入第 3 块(只存在于 WAL);
//   3. 析构 A,同目录新建服务 B:
//      - stats 块数为 3,容量一致;
//      - get(1):Stable / version 2 / 新负载 / 新置信度;
//      - historyOf(1) 两个版本,getAt 旧时刻回溯旧负载;
//      - 第 3 块可查询命中(WAL 重放生效);
//   4. B 写入新块:断言 id 延续为 4(nextId 已恢复);
//   5. 清理临时目录。
void testServiceRecovery() {
    const std::string dir = freshDir("recovery");
    const ServiceConfig config{4, 10.0, 1000, 2, 0, dir, 0.0, 0, 8, {}, 0, 0, 0};

    {
        StmbService serviceA(config);
        MemoryBlock a = makeBlock(5.0, 5.0, 5.0, 100, "A");
        a.confidence = 0.8;
        serviceA.put(a);                                    // id1
        serviceA.put(makeBlock(25.0, 5.0, 5.0, 200, "B"));  // id2
        assert(serviceA.confirm(1, 300));
        assert(serviceA.confirm(1, 400));                   // -> Stable
        assert(serviceA.reportChange(1, "A2", 0.9, 500));   // -> Changing
        assert(serviceA.confirm(1, 600));                   // -> Stable v2
        assert(serviceA.checkpoint());
        serviceA.put(makeBlock(5.0, 25.0, 5.0, 700, "C"));  // id3,仅 WAL
    }

    {
        StmbService serviceB(config);
        assert(serviceB.stats().blockCount == 3);
        assert(serviceB.stats().capacity == 4);

        const auto b1 = serviceB.get(1);
        assert(b1.has_value() && b1->state == BlockState::Stable);
        assert(b1->version == 2 && b1->payload == "A2");
        assert(std::abs(b1->confidence - 0.9) < 1e-9);

        const auto history = serviceB.historyOf(1);
        assert(history.size() == 2);
        assert(history[0].payload == "A" && history[1].payload == "A2");
        assert(serviceB.getAt(1, 100)->payload == "A");
        assert(serviceB.getAt(1, 600)->payload == "A2");

        const auto hits = serviceB.query(AABB{{0.0, 20.0, 0.0}, {10.0, 30.0, 10.0}},
                                         TimeRange{600, 800});
        assert(hits.size() == 1 && hits[0].payload == "C");

        serviceB.put(makeBlock(25.0, 25.0, 5.0, 800, "D"));  // nextId 延续
        assert(serviceB.get(4).has_value());
    }

    fs::remove_all(dir);
    std::cout << "[PASS] persistence: 服务端到端恢复(快照+WAL)状态一致\n";
}

// 伪代码(来源注册表持久化,v6):
//   1. WAL 路径:服务 A 注册两个来源(不做 checkpoint),析构后服务 B
//      reliabilityOf 完整保留;未知来源仍默认 0.5;
//   2. 快照路径:checkpoint 后再注册第三个来源,B 重启后快照(前两个)+
//      WAL(第三个)都恢复;
//   3. 半帧截断容错:注册记录后向 WAL 尾部追加垃圾字节,重启后来源不丢
//      (垃圾记录被丢弃,已验证前缀保留);
//   4. 仲裁联测:加权确认序列(阈值 2,来源权重 0.9/0.4)重启后对第二个
//      Pending 块重放同一序列,仲裁结论一致(来源权重不丢 = 结论可复现)。
void testSourceRegistryPersistence() {
    const std::string dir = freshDir("sources");
    const ServiceConfig config{4, 10.0, 1000, 2, 0, dir, 0.0, 0, 8, {}, 0, 0, 0};

    {
        StmbService serviceA(config);
        serviceA.registerSource(1, 0.9);
        serviceA.registerSource(2, 0.4);              // 仅 WAL,无 checkpoint
    }
    {
        StmbService serviceB(config);
        assert(std::abs(serviceB.reliabilityOf(1) - 0.9) < 1e-9);
        assert(std::abs(serviceB.reliabilityOf(2) - 0.4) < 1e-9);
        assert(std::abs(serviceB.reliabilityOf(99) - 0.5) < 1e-9);  // 未知默认 0.5
        assert(serviceB.checkpoint());                // 来源随快照落盘
        serviceB.registerSource(3, 0.7);              // checkpoint 后仅 WAL

        // 仲裁联测:confirm 权重 = reliabilityOf(source),阈值 2:
        // 0.9+0.4+0.9=2.2 达阈转 Stable
        serviceB.put(makeBlock(25.0, 5.0, 5.0, 200, "Y"));  // id1,Pending
        assert(serviceB.get(1)->state == BlockState::Pending);
        serviceB.confirm(1, 1, 300);                        // +0.9
        assert(serviceB.get(1)->state == BlockState::Pending);
        serviceB.confirm(1, 2, 400);                        // +0.4 = 1.3 < 2
        assert(serviceB.get(1)->state == BlockState::Pending);
        serviceB.confirm(1, 1, 500);                        // +0.9 = 2.2 >= 2 -> Stable
        assert(serviceB.get(1)->state == BlockState::Stable);
    }
    {
        // 模拟 crash:WAL 尾部追加半条垃圾记录(重放应丢弃垃圾、保留前缀)
        std::ofstream out(dir + "/wal.log", std::ios::binary | std::ios::app);
        const char garbage[] = {static_cast<char>(7), static_cast<char>(0xFF),
                                static_cast<char>(0xFF), static_cast<char>(0x7F),
                                static_cast<char>(0x00)};
        out.write(garbage, sizeof(garbage));
    }
    {
        StmbService serviceC(config);
        assert(std::abs(serviceC.reliabilityOf(1) - 0.9) < 1e-9);  // 快照恢复
        assert(std::abs(serviceC.reliabilityOf(2) - 0.4) < 1e-9);
        assert(std::abs(serviceC.reliabilityOf(3) - 0.7) < 1e-9);  // WAL 前缀恢复
        // 重启后来源权重与块状态都在:同一加权序列对第二个块结论一致
        assert(serviceC.get(1)->state == BlockState::Stable);
        serviceC.put(makeBlock(5.0, 25.0, 5.0, 600, "Z"));  // id2,Pending
        serviceC.confirm(2, 1, 700);
        serviceC.confirm(2, 2, 800);
        assert(serviceC.get(2)->state == BlockState::Pending);
        serviceC.confirm(2, 1, 900);
        assert(serviceC.get(2)->state == BlockState::Stable);
    }

    fs::remove_all(dir);
    std::cout << "[PASS] persistence: 来源注册表落盘(快照+WAL+截断容错+仲裁一致)\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用全部测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testSnapshotRoundTrip();
    testWalAppendAndReplay();
    testCheckpointTruncatesWal();
    testCrashTolerance();
    testServiceRecovery();
    testSourceRegistryPersistence();
    std::cout << "ALL TESTS PASS (persistence)\n";
    return 0;
}
