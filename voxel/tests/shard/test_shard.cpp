// ============================================================================
// 文件: tests/shard/test_shard.cpp
// 模块: stmb_tests(分片存储单元测试)
// 覆盖范围:
//   1. 分片归属:不同区域/时间的块落到正确的 ShardKey 文件;
//   2. 延迟加载:重建服务后 query 触达未加载分片自动加载并命中,
//      stats.loadedShards 反映驻留分片数;
//   3. 换出:maxLoadedShards=2 时依次触达 4 个分片,最冷分片被换出,
//      脏数据已 flush,重新加载后数据完整;
//   4. checkpoint/restart:多区域多时间写入 + 状态机流转后 checkpoint,
//      重建服务验证各分片数据、版本链、getAt 回溯与 manifest 一致;
//   5. 海量 smoke:5000 块跨 250 分片、maxLoadedShards=4,随机区域查询
//      100 次全部正确,内存驻留分片数不超上限。
// 测试思路: 临时目录用 std::filesystem::temp_directory_path 下的专属子目录;
//           随机数用确定性 LCG,保证可重复;#undef NDEBUG 保证 Release 下
//           断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "persistence.h"
#include "stmb_service.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

using namespace stmb;
namespace fs = std::filesystem;

// 伪代码:
//   1. 在系统临时目录下构造 tests 专属子目录路径,先 remove_all 清掉残留;
//   2. 返回路径字符串。
std::string freshDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / "stmb_tests" / name;
    fs::remove_all(dir);
    return dir.string();
}

// 伪代码:
//   1. 组装分片模式配置:容量 5000(避免全局淘汰干扰)、格子 10、时间槽 1000、
//      阈值 1、不衰减、分片边长 100、时间桶 10000ms、驻留上限由参数给出。
ServiceConfig shardCfg(const std::string& dir, std::size_t maxLoaded) {
    return ServiceConfig{5000, 10.0, 1000, 1, 0, dir, 100.0, 10000, maxLoaded,
                         {}, 0, 0, 0};
}

// 伪代码:
//   1. 以 (cx, cy, cz) 为中心、半边长 2 构造 AABB;
//   2. 填充 payload 与 timestamp,返回块(id/key 由服务层生成)。
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
//   1. 以 (cx, cy, cz) 为中心构造半边长 3 的窄查询区域(包住目标块,
//      又因块间中心相距足够远而不碰其他块)。
AABB around(double cx, double cy, double cz) {
    return AABB{{cx - 3.0, cy - 3.0, cz - 3.0}, {cx + 3.0, cy + 3.0, cz + 3.0}};
}

// 伪代码(分片归属):
//   1. 写入三块:A(中心 5,5,5,ts=100)、B(中心 150,5,5,ts=200)、
//      C(中心 5,5,5,ts=15000)——分别落在分片 (0,0,0,0)/(1,0,0,0)/(0,0,0,1);
//   2. checkpoint 后用 makeShardKey 计算期望键,逐一 loadShardFile 验证:
//      每个分片文件恰好包含预期的块 id;
//   3. loadManifest 验证分片数、nextId 与条目内容。
void testShardAssignment() {
    const std::string dir = freshDir("shard_assign");
    {
        StmbService service(shardCfg(dir, 8));
        service.put(makeBlock(5.0, 5.0, 5.0, 100, "A"));      // id1
        service.put(makeBlock(150.0, 5.0, 5.0, 200, "B"));    // id2
        service.put(makeBlock(5.0, 5.0, 5.0, 15000, "C"));    // id3
        assert(service.checkpoint());
    }
    const auto manifest = PersistenceManager::loadManifest(dir);
    assert(manifest.has_value() && manifest->present);
    assert(manifest->shards.size() == 3 && manifest->nextId == 4);
    assert(manifest->shardCellSize == 100.0 && manifest->shardTimeSpanMs == 10000);

    const ShardKey kA = makeShardKey(AABB{{3.0, 3.0, 3.0}, {7.0, 7.0, 7.0}}, 100,
                                     100.0, 10000);
    const ShardKey kB = makeShardKey(AABB{{148.0, 3.0, 3.0}, {152.0, 7.0, 7.0}}, 200,
                                     100.0, 10000);
    const ShardKey kC = makeShardKey(AABB{{3.0, 3.0, 3.0}, {7.0, 7.0, 7.0}}, 15000,
                                     100.0, 10000);
    assert((kA == ShardKey{0, 0, 0, 0, 0}));
    assert((kB == ShardKey{1, 0, 0, 0, 0}));
    assert((kC == ShardKey{0, 0, 0, 1, 0}));

    const auto dataA = PersistenceManager::loadShardFile(dir, kA);
    const auto dataB = PersistenceManager::loadShardFile(dir, kB);
    const auto dataC = PersistenceManager::loadShardFile(dir, kC);
    assert(dataA.has_value() && dataA->blocks.size() == 1 &&
           dataA->blocks[0].payload == "A");
    assert(dataB.has_value() && dataB->blocks.size() == 1 &&
           dataB->blocks[0].payload == "B");
    assert(dataC.has_value() && dataC->blocks.size() == 1 &&
           dataC->blocks[0].payload == "C");
    fs::remove_all(dir);
    std::cout << "[PASS] shard: 分片归属正确落到对应 ShardKey 文件\n";
}

// 伪代码(延迟加载):
//   1. 服务 A 写两个分片并 checkpoint 后析构;
//   2. 同目录新建服务 B:断言 loadedShards 为 0(启动不加载任何分片);
//   3. query 触达分片 1:断言命中 A 且 loadedShards 变为 1;
//   4. query 触达分片 2:断言命中 B 且 loadedShards 变为 2。
void testLazyLoad() {
    const std::string dir = freshDir("shard_lazy");
    {
        StmbService service(shardCfg(dir, 8));
        service.put(makeBlock(5.0, 5.0, 5.0, 100, "A"));
        service.put(makeBlock(150.0, 5.0, 5.0, 200, "B"));
        assert(service.checkpoint());
    }
    StmbService service(shardCfg(dir, 8));
    assert(service.stats().loadedShards == 0);

    const auto hitsA = service.query(around(5.0, 5.0, 5.0), TimeRange{0, 1000});
    assert(hitsA.size() == 1 && hitsA[0].payload == "A");
    assert(service.stats().loadedShards == 1);

    const auto hitsB = service.query(around(150.0, 5.0, 5.0), TimeRange{0, 1000});
    assert(hitsB.size() == 1 && hitsB[0].payload == "B");
    assert(service.stats().loadedShards == 2);
    fs::remove_all(dir);
    std::cout << "[PASS] shard: 延迟加载,query 触达自动加载分片\n";
}

// 伪代码(换出):
//   1. maxLoadedShards=2,在 4 个不同空间分片各写 1 块(D1..D4);
//   2. 对 D1 完成一次状态机变更(其分片变脏);
//   3. 依次 query 触达 D2/D3/D4 所在分片:断言 loadedShards 始终 <= 2,
//      最冷分片被换出;
//   4. 重新 query D1 所在分片:断言命中且 payload 为变更后的 "D1-v2"
//      (换出前脏数据已 flush,重新加载数据完整)。
void testShardEviction() {
    const std::string dir = freshDir("shard_evict");
    StmbService service(shardCfg(dir, 2));
    service.put(makeBlock(5.0, 5.0, 5.0, 100, "D1"));
    service.put(makeBlock(150.0, 5.0, 5.0, 200, "D2"));
    service.put(makeBlock(250.0, 5.0, 5.0, 300, "D3"));
    service.put(makeBlock(350.0, 5.0, 5.0, 400, "D4"));
    assert(service.stats().loadedShards <= 2);

    assert(service.confirm(1, 500));
    assert(service.reportChange(1, "D1-v2", 0.9, 600));
    assert(service.confirm(1, 700));

    assert(service.query(around(150.0, 5.0, 5.0), TimeRange{0, 1000}).size() == 1);
    assert(service.stats().loadedShards <= 2);
    assert(service.query(around(250.0, 5.0, 5.0), TimeRange{0, 1000}).size() == 1);
    assert(service.query(around(350.0, 5.0, 5.0), TimeRange{0, 1000}).size() == 1);
    assert(service.stats().loadedShards <= 2);

    const auto hits = service.query(around(5.0, 5.0, 5.0), TimeRange{0, 1000});
    assert(hits.size() == 1 && hits[0].payload == "D1-v2" && hits[0].version == 2);
    fs::remove_all(dir);
    std::cout << "[PASS] shard: 冷分片换出(脏数据 flush 后重新加载完整)\n";
}

// 伪代码(checkpoint / restart):
//   1. 服务 A:3 个分片各写 1 块,对块 1 完成一次变更(v2),checkpoint 后析构;
//   2. loadManifest 断言:分片数 3、nextId 4、块 1 所在分片版本数为 2;
//   3. 同目录新建服务 B:逐分片 query 命中正确 payload;historyOf(1) 两个
//      版本;getAt 旧时刻回溯 "S1",新时刻命中 "S1-v2";新写入 id 延续为 4。
void testCheckpointRestart() {
    const std::string dir = freshDir("shard_restart");
    ShardKey keyOf1;
    {
        StmbService service(shardCfg(dir, 8));
        service.put(makeBlock(5.0, 5.0, 5.0, 100, "S1"));
        service.put(makeBlock(150.0, 5.0, 5.0, 200, "S2"));
        service.put(makeBlock(5.0, 5.0, 5.0, 15000, "S3"));
        assert(service.confirm(1, 300));
        assert(service.reportChange(1, "S1-v2", 0.9, 400));
        assert(service.confirm(1, 500));
        assert(service.checkpoint());
        keyOf1 = makeShardKey(AABB{{3.0, 3.0, 3.0}, {7.0, 7.0, 7.0}}, 100,
                              100.0, 10000);
    }
    const auto manifest = PersistenceManager::loadManifest(dir);
    assert(manifest.has_value() && manifest->shards.size() == 3);
    assert(manifest->nextId == 4);
    for (const ManifestShardEntry& e : manifest->shards) {
        if (e.key == keyOf1) {
            assert(e.versionCount == 2);
        }
    }

    StmbService service(shardCfg(dir, 8));
    assert(service.query(around(5.0, 5.0, 5.0), TimeRange{0, 1000}).size() == 1);
    assert(service.query(around(150.0, 5.0, 5.0), TimeRange{0, 1000}).size() == 1);
    assert(service.query(around(5.0, 5.0, 5.0), TimeRange{14000, 16000}).size() == 1);
    const auto b1 = service.get(1);
    assert(b1.has_value() && b1->payload == "S1-v2" && b1->version == 2);
    assert(service.historyOf(1).size() == 2);
    assert(service.getAt(1, 100)->payload == "S1");
    assert(service.getAt(1, 500)->payload == "S1-v2");
    service.put(makeBlock(250.0, 5.0, 5.0, 600, "S4"));
    assert(service.get(4).has_value());
    fs::remove_all(dir);
    std::cout << "[PASS] shard: checkpoint/restart 各分片数据与版本链一致\n";
}

// 伪代码(海量 smoke):
//   1. 配置容量 5000、驻留 4 的服务;写入 5000 块:cx=(i%50)*10+5、
//      cy=((i/50)%10)*100+50、cz=5、ts=i*4 —— 落在 5*10*5=250 个分片;
//   2. 写完后断言 loadedShards <= 4;
//   3. 用确定性 LCG 随机抽 100 个块,窄区域 + 窄时间查询:
//      断言恰好命中 1 块且 payload 为 "blk<i>";
//   4. 全程断言 loadedShards 不超上限。
void testMassiveSmoke() {
    const std::string dir = freshDir("shard_smoke");
    constexpr int kTotal = 5000;
    StmbService service(shardCfg(dir, 4));
    for (int i = 0; i < kTotal; ++i) {
        const double cx = (i % 50) * 10.0 + 5.0;
        const double cy = ((i / 50) % 10) * 100.0 + 50.0;
        const TimeStamp ts = static_cast<TimeStamp>(i) * 4;
        service.put(makeBlock(cx, cy, 5.0, ts, "blk" + std::to_string(i)));
    }
    assert(service.stats().loadedShards <= 4);

    std::uint32_t state = 12345;
    for (int q = 0; q < 100; ++q) {
        state = state * 1103515245U + 12345U;
        const int i = static_cast<int>((state >> 16) % kTotal);
        const double cx = (i % 50) * 10.0 + 5.0;
        const double cy = ((i / 50) % 10) * 100.0 + 50.0;
        const TimeStamp ts = static_cast<TimeStamp>(i) * 4;
        const auto hits = service.query(around(cx, cy, 5.0), TimeRange{ts - 1, ts + 1});
        assert(hits.size() == 1);
        assert(hits[0].payload == "blk" + std::to_string(i));
        assert(service.stats().loadedShards <= 4);
    }
    fs::remove_all(dir);
    std::cout << "[PASS] shard: 海量 smoke(5000 块 / 250 分片 / 驻留 <=4)\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用五个测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testShardAssignment();
    testLazyLoad();
    testShardEviction();
    testCheckpointRestart();
    testMassiveSmoke();
    std::cout << "ALL TESTS PASS (shard)\n";
    return 0;
}
