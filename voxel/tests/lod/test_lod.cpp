// ============================================================================
// 文件: tests/lod/test_lod.cpp
// 模块: stmb_tests(LOD 多尺度金字塔单元测试)
// 覆盖范围:
//   1. 层级归属:显式指定与自动判定(AABB 尺寸匹配最近层级)正确,
//      不同 level 的块进入各自层级的索引、互不污染;
//   2. 分层查询:同区域不同 level 各查各的;queryAllLevels 跨层级合并;
//      缺省 query 只查最细层级;
//   3. 聚合上卷:细层 4 块 -> 粗层摘要块(计数/主要语义/均值置信度/多数
//      状态),摘要块参与状态机与版本链;重建时旧摘要被替换;
//   4. 未细化检测:只有粗层数据的区域 isRefined 为 false、细层查询为空;
//      写入细层后指示翻转;粗层命中块的 hasFinerData 标志同步翻转;
//   5. 分片+LOD 组合:分片模式下多层级写入、驻留换出、checkpoint 重启后
//      各层级数据与摘要完整。
// 测试思路: 三级金字塔 levelCellSizes={100,10,1};临时目录放系统临时路径下,
//           用例结束清理;#undef NDEBUG 保证 Release 下断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "stmb_service.h"

#include <cassert>
#include <cmath>
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
//   1. 组装 LOD 配置:容量 5000、最细格子 1.0、时间槽 1000、阈值 1、
//      不衰减、不分片、三级金字塔 {100, 10, 1}(0 最粗)。
ServiceConfig lodCfg() {
    return ServiceConfig{5000, 1.0, 1000, 1, 0, "", 0.0, 0, 8,
                         {100.0, 10.0, 1.0}, 0, 0, 0};
}

// 伪代码:
//   1. 以 (cx, cy, cz) 为中心、half 为半边长构造 AABB;
//   2. 填充 payload / timestamp / level(-1 = 自动判定),返回块。
MemoryBlock makeBlock(double cx, double cy, double cz, double half,
                      TimeStamp ts, const std::string& payload, int level = -1) {
    MemoryBlock b;
    b.region.min = {cx - half, cy - half, cz - half};
    b.region.max = {cx + half, cy + half, cz + half};
    b.payload = payload;
    b.timestamp = ts;
    b.level = level;
    return b;
}

// 伪代码(层级归属):
//   1. 自动判定:半边长 2(extent 4)-> level 2;半边长 5(extent 10)-> level 1;
//      半边长 50(extent 100)-> level 0;逐 put 后断言 get 读回的 level;
//   2. 显式指定:小尺寸块显式 level=0,断言不被自动规则改写;
//   3. 索引隔离:宽区域查询,level 0/1/2 各自只命中本层级的块。
void testLevelAssignment() {
    StmbService service(lodCfg());
    service.put(makeBlock(5.0, 5.0, 5.0, 2.0, 100, "fine"));       // id1 -> L2
    service.put(makeBlock(50.0, 50.0, 50.0, 5.0, 100, "mid"));     // id2 -> L1
    service.put(makeBlock(150.0, 150.0, 150.0, 50.0, 100, "coarse"));  // id3 -> L0
    MemoryBlock explicit0 = makeBlock(5.0, 150.0, 5.0, 1.0, 100, "forced", 0);
    service.put(explicit0);                                        // id4 -> L0

    assert(service.get(1)->level == 2);
    assert(service.get(2)->level == 1);
    assert(service.get(3)->level == 0);
    assert(service.get(4)->level == 0);

    const AABB wide{{-200.0, -200.0, -200.0}, {200.0, 200.0, 200.0}};
    const TimeRange all{0, 10000};
    assert(service.queryLevel(wide, all, 0).size() == 2);  // coarse + forced
    assert(service.queryLevel(wide, all, 1).size() == 1);  // mid
    assert(service.queryLevel(wide, all, 2).size() == 1);  // fine
    std::cout << "[PASS] lod: 层级归属(自动/显式)与索引隔离\n";
}

// 伪代码(分层查询):
//   1. 同一区域写入三个显式层级(0/1/2)的块;
//   2. queryLevel 各层只命中本层块;
//   3. queryAllLevels 合并命中全部 3 块;
//   4. 缺省 query 只查最细层级,命中 1 块。
void testLayeredQuery() {
    StmbService service(lodCfg());
    service.put(makeBlock(5.0, 5.0, 5.0, 4.0, 100, "L0", 0));
    service.put(makeBlock(5.0, 5.0, 5.0, 4.0, 200, "L1", 1));
    service.put(makeBlock(5.0, 5.0, 5.0, 4.0, 300, "L2", 2));

    const AABB region{{0.0, 0.0, 0.0}, {10.0, 10.0, 10.0}};
    const TimeRange all{0, 10000};
    assert(service.queryLevel(region, all, 0)[0].payload == "L0");
    assert(service.queryLevel(region, all, 1)[0].payload == "L1");
    assert(service.queryLevel(region, all, 2)[0].payload == "L2");
    assert(service.queryAllLevels(region, all).size() == 3);
    const auto def = service.query(region, all);
    assert(def.size() == 1 && def[0].payload == "L2");  // 缺省 = 最细层级
    std::cout << "[PASS] lod: 分层查询与跨层级合并\n";
}

// 伪代码(聚合上卷):
//   1. 细层(level 2)写 4 块:同处 level-1 格子 (0,0,0),负载 3x"tree"+
//      1x"rock",置信度 0.4/0.6/0.8/1.0(均值 0.7),前 3 块 confirm 转
//      Stable(多数状态),时间戳 100..400;
//   2. buildSummaries(1):断言返回 1;摘要块 level=1、isSummary、
//      sourceLevel=2、payload 含 "4 个子块" 与 "tree"、confidence≈0.7、
//      state=Stable、timestamp=400、区域为 [0,10]^3;
//   3. 摘要块参与版本链:historyOf 1 个版本;reportChange+confirm 后
//      version=2、historyOf 两个版本;
//   4. 再次 buildSummaries(1):旧摘要被替换(get 旧 id 为空,新摘要仅 1 个)。
void testBuildSummaries() {
    StmbService service(lodCfg());
    const double confs[4] = {0.4, 0.6, 0.8, 1.0};
    for (int i = 0; i < 4; ++i) {
        MemoryBlock b = makeBlock(1.0 + i, 1.0 + i, 1.0 + i, 0.2,
                                  100 * (i + 1), i < 3 ? "tree" : "rock", 2);
        b.confidence = confs[i];
        service.put(b);
    }
    assert(service.confirm(1, 500) && service.confirm(2, 500) &&
           service.confirm(3, 500));  // 3 块 Stable,1 块 Pending

    assert(service.buildSummaries(1) == 1);
    const auto summary = service.get(5);
    assert(summary.has_value());
    assert(summary->level == 1 && summary->isSummary && summary->sourceLevel == 2);
    assert(summary->payload.find("4 个子块") != std::string::npos);
    assert(summary->payload.find("tree") != std::string::npos);
    assert(std::abs(summary->confidence - 0.7) < 1e-9);
    assert(summary->state == BlockState::Stable && summary->timestamp == 400);
    assert((summary->region.min == std::array<double, 3>{0.0, 0.0, 0.0}));
    assert((summary->region.max == std::array<double, 3>{10.0, 10.0, 10.0}));
    assert(service.historyOf(5).size() == 1);  // 摘要块已入版本链

    assert(service.reportChange(5, "summary-v2", 0.9, 600));
    assert(service.confirm(5, 700));
    assert(service.get(5)->version == 2 && service.historyOf(5).size() == 2);

    assert(service.buildSummaries(1) == 1);    // 重建:旧摘要被替换
    assert(!service.get(5).has_value());
    const auto summary2 = service.get(6);
    assert(summary2.has_value() && summary2->isSummary);
    std::cout << "[PASS] lod: 聚合上卷(计数/均值/多数状态)与摘要版本链\n";
}

// 伪代码(未细化检测):
//   1. 只写入粗层(level 0)块:断言 isRefined(region, 0) 为 false,
//      细层查询为空,粗层命中块的 hasFinerData 为 false;
//   2. 在该区域写入细层(level 2)块:断言 isRefined 翻转为 true,
//      细层查询命中,粗层命中块 hasFinerData 翻转为 true;
//   3. 对从未有数据的区域,isRefined 始终为 false。
void testRefinementDetection() {
    StmbService service(lodCfg());
    service.put(makeBlock(50.0, 50.0, 50.0, 50.0, 100, "skeleton", 0));
    const AABB sub{{10.0, 10.0, 10.0}, {20.0, 20.0, 20.0}};
    const TimeRange all{0, 10000};

    assert(!service.isRefined(sub, 0));
    assert(service.queryLevel(sub, all, 2).empty());
    assert(!service.queryLevel(sub, all, 0)[0].hasFinerData);

    service.put(makeBlock(15.0, 15.0, 15.0, 1.0, 200, "detail", 2));
    assert(service.isRefined(sub, 0));
    assert(service.queryLevel(sub, all, 2).size() == 1);
    assert(service.queryLevel(sub, all, 0)[0].hasFinerData);

    assert(!service.isRefined(AABB{{500.0, 500.0, 500.0}, {510.0, 510.0, 510.0}}, 0));
    std::cout << "[PASS] lod: 未细化检测与 hasFinerData 翻转\n";
}

// 伪代码(分片 + LOD 组合):
//   1. 分片配置(驻留上限 2)叠加三级金字塔:写入 L0/L1/L2 各层块 + 一个
//      异时间桶的 L2 块,writing 过程即触发分片换出;
//   2. buildSummaries(1) 生成 2 个摘要(两个 L2 块分属不同 L1 格子),
//      随后 checkpoint;
//   3. 同目录重建服务:逐层 queryLevel 验证 L0/L1/L2 命中数
//      (L1 含 2 个摘要),摘要块数据完整,loadedShards 不超上限。
void testShardLodCombo() {
    const std::string dir = freshDir("shard_lod");
    const ServiceConfig config{1000, 1.0, 1000, 1, 0, dir, 100.0, 10000, 2,
                               {100.0, 10.0, 1.0}, 0, 0, 0};
    {
        StmbService service(config);
        service.put(makeBlock(50.0, 50.0, 50.0, 50.0, 100, "L0"));      // id1
        service.put(makeBlock(150.0, 50.0, 50.0, 5.0, 200, "L1"));      // id2
        service.put(makeBlock(250.5, 5.0, 5.0, 0.5, 300, "L2-a"));      // id3
        service.put(makeBlock(350.5, 5.0, 5.0, 0.5, 15000, "L2-b"));    // id4
        assert(service.stats().loadedShards <= 2);
        assert(service.buildSummaries(1) == 2);
        assert(service.checkpoint());
    }
    {
        StmbService service(config);
        const AABB wide{{0.0, 0.0, 0.0}, {400.0, 100.0, 100.0}};
        const TimeRange all{0, 20000};
        assert(service.queryLevel(wide, all, 0).size() == 1);
        assert(service.queryLevel(wide, all, 1).size() == 3);  // L1 块 + 2 摘要
        assert(service.queryLevel(wide, all, 2).size() == 2);
        assert(service.stats().loadedShards <= 2);

        bool sawSummary = false;
        for (const MemoryBlock& b : service.queryLevel(wide, all, 1)) {
            if (b.isSummary) {
                sawSummary = true;
                assert(b.sourceLevel == 2);
                assert(service.historyOf(b.id).size() == 1);  // 摘要随分片持久化
            }
        }
        assert(sawSummary);
    }
    fs::remove_all(dir);
    std::cout << "[PASS] lod: 分片+LOD 组合(换出/重启后层级数据完整)\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用五个测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testLevelAssignment();
    testLayeredQuery();
    testBuildSummaries();
    testRefinementDetection();
    testShardLodCombo();
    std::cout << "ALL TESTS PASS (lod)\n";
    return 0;
}
