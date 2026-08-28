// ============================================================================
// 文件: tests/arbitration/test_arbitration.cpp
// 模块: stmb_tests(观测处理管线:变化分类 + 周期模式 + 多源仲裁)
// 覆盖范围:
//   1. 变化分类六种:Emergence/Vanishing/Mutation/Seasonal/Transient/Conflict
//      各自的触发路径;
//   2. 周期模式:相位内观测 -> Seasonal 不产生新版本;相位外矛盾观测 ->
//      正常 Mutation 流程;
//   3. 加权仲裁:高可靠来源一次确认即达阈值,低可靠来源需多次,未知来源
//      按默认 0.5;
//   4. 观察窗口:候选挂起 -> sweepWindows 超时丢弃并恢复;窗口内反向观测
//      -> Transient 关闭且无版本产生;
//   5. 空间一致性:悬空块 suspect 标记;地面语义 + 上方空块冲突标记;
//   6. 持久化:模式 / 加权确认数 / 挂起候选 / suspect 重启后一致(v5)。
// 测试思路: 全部走 StmbService 公开 API;临时目录放系统临时路径并清理;
//           #undef NDEBUG 保证 Release 下断言生效。
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
//   1. 组装管线配置:容量 100、阈值 2.0、观察窗口 5000ms,其余默认(不分片、
//      单尺度、不动态层);目录由参数给出(空 = 不持久化)。
ServiceConfig pipeCfg(const std::string& dir) {
    return ServiceConfig{100, 10.0, 1000, 2.0, 0, dir, 0.0, 0, 8, {}, 0, 0, 5000};
}

// 伪代码:
//   1. 以中心坐标构造半边长 2 的块,payload/timestamp 由参数给出;
//   2. 返回构造好的块(id/状态由服务层填充)。
MemoryBlock makeBlock(double cx, double cy, double cz,
                      const std::string& payload, TimeStamp ts) {
    MemoryBlock b;
    b.region = AABB{{cx - 2.0, cy - 2.0, cz - 2.0}, {cx + 2.0, cy + 2.0, cz + 2.0}};
    b.payload = payload;
    b.timestamp = ts;
    return b;
}

// 伪代码:
//   1. 组装一条观测记录。
Observation obs(const std::string& payload, SourceId source, TimeStamp t) {
    Observation o;
    o.payload = payload;
    o.source = source;
    o.t = t;
    return o;
}

// 伪代码(变化分类六种):
//   1. Emergence:空 payload 块(空地)confirm 转 Stable 后,高可靠来源
//      (0.9*3=2.7>=2... 用 registerSource(9, 2.5) 一次到位)观测非空 ->
//      Emergence 立即生效,payload 更新、version+1;
//   2. Vanishing:对非空块提交空观测(高可靠)-> Vanishing 生效,payload 变空;
//   3. Mutation:非空 -> 不同非空(高可靠)-> Mutation 生效;
//   4. Seasonal:注册周期模式后提交相位内观测 -> Seasonal,版本不变;
//   5. Transient:低可靠观测挂起后,反向观测(== 当前值)-> Transient
//      关闭,版本不变;
//   6. Conflict:候选挂起中收到第三种矛盾值 -> Conflict。
void testChangeClassification() {
    StmbService service(pipeCfg(""));
    service.registerSource(9, 2.5);   // 高可靠:单次即达阈值 2.0
    service.registerSource(1, 0.4);   // 低可靠

    // Emergence:空地块 + 非空观测
    service.put(makeBlock(5.0, 5.0, 5.0, "", 0));       // id1 空地
    service.confirm(1, 100);                            // 自确认 1.0
    service.confirm(1, 200);                            // 1.0+1.0 >= 2 -> Stable
    ChangeReport r = service.submitObservation(1, obs("house", 9, 300));
    assert(r.type == ChangeType::Emergence && r.accepted);
    assert(service.get(1)->payload == "house" && service.get(1)->version == 2);

    // Vanishing:非空块 + 空观测
    r = service.submitObservation(1, obs("", 9, 400));
    assert(r.type == ChangeType::Vanishing && r.accepted);
    assert(service.get(1)->payload.empty() && service.get(1)->version == 3);

    // Mutation:非空 -> 不同非空
    r = service.submitObservation(1, obs("shop", 9, 500));
    assert(r.type == ChangeType::Emergence && r.accepted);  // 空 -> 非空
    r = service.submitObservation(1, obs("mall", 9, 600));
    assert(r.type == ChangeType::Mutation && r.accepted);
    assert(service.get(1)->payload == "mall" && service.get(1)->version == 5);

    // Seasonal:注册昼夜模式,相位内观测
    PeriodicPattern pattern;
    pattern.periodMs = 10000;
    pattern.phases = {{0, 5000, "mall"}, {5000, 5000, "mall-night"}};
    assert(service.registerPattern(1, pattern));
    const std::size_t versionsBefore = service.historyOf(1).size();
    r = service.submitObservation(1, obs("mall-night", 1, 6000));  // 相位 6000 -> night
    assert(r.type == ChangeType::Seasonal && r.accepted);
    assert(service.historyOf(1).size() == versionsBefore);  // 不产生新版本

    // Transient:低可靠候选挂起 -> 反向观测关闭
    r = service.submitObservation(1, obs("park", 1, 7000));
    assert(!r.accepted);                                     // 挂起(0.4 < 2)
    assert(service.get(1)->state == BlockState::Changing);
    r = service.submitObservation(1, obs("mall", 1, 7500));  // 反向:回到当前值
    assert(r.type == ChangeType::Transient && r.accepted);
    assert(service.get(1)->state == BlockState::Stable);
    assert(service.get(1)->payload == "mall");
    assert(service.historyOf(1).size() == versionsBefore);  // 无版本产生

    // Conflict:候选挂起中收到第三种矛盾值
    r = service.submitObservation(1, obs("park", 1, 8000));  // 挂起 "park"
    assert(!r.accepted);
    r = service.submitObservation(1, obs("school", 1, 8500));
    assert(r.type == ChangeType::Conflict && !r.accepted);
    assert(service.get(1)->state == BlockState::Changing);   // 候选仍挂起
    std::cout << "[PASS] arbitration: 变化分类六种路径\n";
}

// 伪代码(周期模式):
//   1. 块注册模式 {周期 10000,昼 "lamp-off"/夜 "lamp-on"};
//   2. 相位内观测 "lamp-on"(t=7000 -> 相位 7000):Seasonal,版本数不变,
//      存储 payload 不被改写(模式即真值);
//   3. 相位外观测 "lamp-on"(t=2000 -> 相位 2000,预测 "lamp-off"):
//      走正常流程(低可靠挂起为 Mutation 候选)。
void testPeriodicPattern() {
    StmbService service(pipeCfg(""));
    service.registerSource(1, 0.4);
    service.put(makeBlock(5.0, 5.0, 5.0, "lamp-off", 0));
    service.confirm(1, 100);
    service.confirm(1, 200);  // -> Stable
    PeriodicPattern pattern;
    pattern.periodMs = 10000;
    pattern.phases = {{0, 5000, "lamp-off"}, {5000, 5000, "lamp-on"}};
    assert(service.registerPattern(1, pattern));
    assert(service.patternOf(1).has_value());

    const std::size_t before = service.historyOf(1).size();
    ChangeReport r = service.submitObservation(1, obs("lamp-on", 1, 7000));
    assert(r.type == ChangeType::Seasonal && r.accepted);
    assert(service.historyOf(1).size() == before);
    assert(service.get(1)->payload == "lamp-off");  // 存储值不随相位刷写

    r = service.submitObservation(1, obs("lamp-on", 1, 2000));  // 相位不符
    assert(r.type == ChangeType::Mutation && !r.accepted);      // 正常挂起
    assert(service.get(1)->state == BlockState::Changing);
    std::cout << "[PASS] arbitration: 周期模式 Seasonal 判定与相位外流程\n";
}

// 伪代码(加权仲裁):
//   1. 阈值 2.0:高可靠来源(2.5)一次 confirm 即达阈值转 Stable;
//   2. 低可靠来源(0.4)需 5 次(0.4*5=2.0)才转 Stable;
//   3. 未知来源按默认 0.5:2 次 1.0 未达,4 次 2.0 达成。
void testWeightedArbitration() {
    StmbService service(pipeCfg(""));
    service.registerSource(9, 2.5);
    service.registerSource(1, 0.4);

    service.put(makeBlock(5.0, 5.0, 5.0, "a", 0));      // id1
    assert(service.confirm(1, 9, 100));                 // 2.5 >= 2.0
    assert(service.get(1)->state == BlockState::Stable);

    service.put(makeBlock(15.0, 5.0, 5.0, "b", 0));     // id2
    for (int i = 0; i < 4; ++i) {
        service.confirm(2, 1, 100 + i);                 // 0.4*4 = 1.6 < 2.0
    }
    assert(service.get(2)->state == BlockState::Pending);
    service.confirm(2, 1, 200);                         // 2.0 >= 2.0
    assert(service.get(2)->state == BlockState::Stable);

    service.put(makeBlock(25.0, 5.0, 5.0, "c", 0));     // id3
    service.confirm(3, 777, 100);                       // 未知来源 0.5
    service.confirm(3, 777, 200);                       // 1.0
    assert(service.get(3)->state == BlockState::Pending);
    service.confirm(3, 777, 300);
    service.confirm(3, 777, 400);                       // 2.0
    assert(service.get(3)->state == BlockState::Stable);
    std::cout << "[PASS] arbitration: 加权确认阈值(高/低/未知来源)\n";
}

// 伪代码(观察窗口):
//   1. 低可靠观测使候选挂起(Changing + 窗口字段);
//   2. sweepWindows(窗口内):候选仍在;
//   3. sweepWindows(窗口超时):候选被丢弃,状态恢复 Stable、payload 不变、
//      版本数不变;
//   4. 反向观测路径(Transient)在分类用例已覆盖,此处补窗口边界:
//      恰好未超窗不丢弃。
void testObservationWindow() {
    StmbService service(pipeCfg(""));
    service.registerSource(1, 0.4);
    service.put(makeBlock(5.0, 5.0, 5.0, "meadow", 0));
    service.confirm(1, 100);
    service.confirm(1, 200);  // -> Stable

    ChangeReport r = service.submitObservation(1, obs("shop", 1, 1000));
    assert(r.type == ChangeType::Mutation && !r.accepted);
    assert(service.get(1)->state == BlockState::Changing);

    assert(service.sweepWindows(1000 + 5000) == 0);     // 恰好未超窗
    assert(service.get(1)->state == BlockState::Changing);
    assert(service.sweepWindows(1000 + 5001) == 1);     // 超时丢弃
    const auto block = service.get(1);
    assert(block->state == BlockState::Stable && block->payload == "meadow");
    assert(block->version == 1 && service.historyOf(1).size() == 1);
    std::cout << "[PASS] arbitration: 观察窗口挂起/超时丢弃/状态恢复\n";
}

// 伪代码(空间一致性):
//   1. 地面块 "road" + 其正上方空语义块:validateSpatial 命中垂直一致冲突,
//      road 块 suspect;
//   2. 无支撑的悬空块 "box":validateSpatial 命中悬空检测,suspect;
//   3. 下方有支撑的块:无违规,suspect 保持 false。
void testSpatialConsistency() {
    StmbService service(pipeCfg(""));
    service.put(makeBlock(5.0, 5.0, 1.0, "road", 0));        // id1 z:[-1,3]
    service.put(makeBlock(5.0, 5.0, 5.0, "", 0));            // id2 空块 z:[3,7] 紧邻上方
    service.put(makeBlock(50.0, 50.0, 50.0, "box", 0));      // id3 悬空
    service.put(makeBlock(50.0, 50.0, 46.0, "road", 0));     // id4 垫在 id3 下方? z:[44,48]
    // id3 z:[48,52],id4 z:[44,48] -> id3 下移一个块高(4)为 [44,48] 与 id4 相交 -> 有支撑

    auto v1 = service.validateSpatial(1);
    assert(v1.size() == 1 && v1[0].find("垂直一致") != std::string::npos);
    assert(service.get(1)->suspect);

    // 先校验 id4(地面):其上方 [48,52] 是 "box"(非空) -> 不冲突
    assert(service.validateSpatial(4).empty());
    assert(!service.get(4)->suspect);

    // 把 id3 换成真正悬空的块(下方 [44,48] 的 id4 被删除)
    service.remove(4);
    auto v3 = service.validateSpatial(3);
    assert(v3.size() == 1 && v3[0].find("悬空") != std::string::npos);
    assert(service.get(3)->suspect);
    std::cout << "[PASS] arbitration: 空间一致性(垂直冲突/悬空 suspect)\n";
}

// 伪代码(持久化 v5):
//   1. 服务 A:注册模式、低可靠候选挂起(窗口字段)、suspect 标记、Pending
//      块的加权确认计数,checkpoint;
//   2. 同目录重建服务 B:断言模式、加权计数(double)、挂起候选(Changing +
//      pendingPayload + windowStart/weight/sources)、suspect 全部一致。
void testPipelinePersistence() {
    const std::string dir = freshDir("pipeline_persist");
    {
        StmbService service(pipeCfg(dir));
        service.registerSource(1, 0.4);
        service.put(makeBlock(5.0, 5.0, 5.0, "meadow", 0));      // id1
        service.confirm(1, 1, 100);                              // 0.4
        PeriodicPattern pattern;
        pattern.periodMs = 10000;
        pattern.phases = {{0, 5000, "meadow"}, {5000, 5000, "meadow-night"}};
        assert(service.registerPattern(1, pattern));

        service.put(makeBlock(15.0, 5.0, 5.0, "house", 0));      // id2
        service.confirm(2, 100);
        service.confirm(2, 200);                                 // -> Stable
        service.submitObservation(2, obs("shop", 1, 300));       // 挂起

        service.put(makeBlock(50.0, 50.0, 50.0, "box", 0));      // id3 悬空
        assert(!service.validateSpatial(3).empty());
        assert(service.checkpoint());
    }
    {
        StmbService service(pipeCfg(dir));
        const auto b1 = service.get(1);
        assert(b1->pattern.has_value() && b1->pattern->periodMs == 10000);
        assert(b1->pattern->phases.size() == 2);
        assert(std::abs(b1->confirmations - 0.4) < 1e-9);        // 加权计数保留

        const auto b2 = service.get(2);
        assert(b2->state == BlockState::Changing);               // 挂起候选保留
        assert(b2->pendingPayload.has_value() && *b2->pendingPayload == "shop");
        assert(b2->windowStart == 300);
        assert(std::abs(b2->windowWeight - 0.4) < 1e-9);
        assert(b2->windowSources.count(1) == 1);

        assert(service.get(3)->suspect);                         // suspect 保留
    }
    fs::remove_all(dir);
    std::cout << "[PASS] arbitration: 管线状态持久化(v5)重启一致\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用六个测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testChangeClassification();
    testPeriodicPattern();
    testWeightedArbitration();
    testObservationWindow();
    testSpatialConsistency();
    testPipelinePersistence();
    std::cout << "ALL TESTS PASS (arbitration)\n";
    return 0;
}
