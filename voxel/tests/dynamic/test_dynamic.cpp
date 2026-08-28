// ============================================================================
// 文件: tests/dynamic/test_dynamic.cpp
// 模块: stmb_tests(动态实例轨迹层单元测试)
// 覆盖范围:
//   1. 基本上报与轨迹:多次 report 轨迹累积、点数上限裁剪丢最老点、
//      零速度上报时的自动差分估计;
//   2. 跨源关联:两个来源对同一移动物体的渐进上报关联为同一实例,
//      sources 含两者;类别不符 / 空间不重叠则新建;
//   3. 状态推进:静止超时 -> Stationary;观测中断超时 -> Archived;
//      Archived 不参与 queryActive 但 trajectoryOf 可查;运动中复活;
//   4. 沉淀:promoteStationary 把 Stationary 实例变成静态层 Pending 块,
//      confirm 后转 Stable,实例归档;
//   5. 临时占用:静态块与活跃实例相交时查询附带占用标记,归档后消失;
//   6. 持久化:单文件与分片两种模式下 checkpoint -> 重启 -> 实例、轨迹、
//      状态恢复一致。
// 测试思路: 层级单测直接驱动 DynamicLayer;服务级用例走 StmbService;
//           临时目录放系统临时路径下并清理;#undef NDEBUG 保证 Release 下
//           断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "dynamic_layer.h"
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
//   1. 以 (cx, cy, cz) 为中心、half 为半边长构造 AABB 并返回。
AABB boxAt(double cx, double cy, double cz, double half) {
    return AABB{{cx - half, cy - half, cz - half}, {cx + half, cy + half, cz + half}};
}

// 伪代码(基本上报与轨迹):
//   1. 构造 DynamicLayer(静止 1000ms / 归档 5000ms / 上限 64 点);
//   2. 同实例 3 次上报:位置 (0,0,0)->(10,0,0)->(30,0,0),后两次零速度
//      触发差分:断言速度分别约 {10,0,0} 与 {20,0,0}(单位/秒);
//   3. 断言轨迹累积 3 点、位置递增;
//   4. 再补 70 次上报:断言轨迹裁剪到 64 点,最老点被丢(队首 t=9000)。
void testReportAndTrajectory() {
    DynamicLayer layer(1000, 5000, 64, 2000);
    const std::array<double, 3> zero{0.0, 0.0, 0.0};
    assert(layer.report(1, "car", boxAt(0.0, 0.0, 0.0, 1.0), {1.0, 0.0, 0.0},
                        0, 1) == 1);
    layer.report(1, "car", boxAt(10.0, 0.0, 0.0, 1.0), zero, 1000, 1);
    layer.report(1, "car", boxAt(30.0, 0.0, 0.0, 1.0), zero, 2000, 1);

    const auto traj = layer.trajectoryOf(1);
    assert(traj.size() == 3);
    assert(std::abs(traj[1].velocity[0] - 10.0) < 1e-9);   // (10-0)/1s
    assert(std::abs(traj[2].velocity[0] - 20.0) < 1e-9);   // (30-10)/1s
    assert(traj[2].position[0] == 30.0);

    for (int i = 0; i < 70; ++i) {
        layer.report(1, "car", boxAt(30.0 + i + 1, 0.0, 0.0, 1.0), {1.0, 0.0, 0.0},
                     3000 + i * 1000, 1);
    }
    const auto trimmed = layer.trajectoryOf(1);
    assert(trimmed.size() == 64);
    assert(trimmed.front().t == 9000);  // 73 点丢最老 9 点
    std::cout << "[PASS] dynamic: 上报/轨迹累积/上限裁剪/速度差分\n";
}

// 伪代码(跨源关联):
//   1. 来源 A 上报 car @(0,0,0);
//   2. 来源 B 在 500ms 后上报 car @(1.5,0,0)(包围盒相交、类别一致、时间
//      在窗内):断言关联为同一实例,sources 含 A、B,轨迹 2 点;
//   3. 远处上报:断言新建实例;类别不符:断言新建实例。
void testCrossSourceAssociation() {
    DynamicLayer layer(1000, 5000, 64, 2000);
    const std::array<double, 3> vel{1.0, 0.0, 0.0};
    const InstanceId first = layer.reportAuto("car", boxAt(0.0, 0.0, 0.0, 1.0),
                                              vel, 0, 100);
    const InstanceId second = layer.reportAuto("car", boxAt(1.5, 0.0, 0.0, 1.0),
                                               vel, 500, 200);
    assert(first == second);
    const DynamicInstance* inst = layer.find(first);
    assert(inst != nullptr && inst->sources.size() == 2);
    assert(inst->sources.count(100) == 1 && inst->sources.count(200) == 1);
    assert(inst->trajectory.size() == 2);

    const InstanceId far = layer.reportAuto("car", boxAt(100.0, 0.0, 0.0, 1.0),
                                            vel, 600, 200);
    assert(far != first);
    const InstanceId otherClass = layer.reportAuto("truck", boxAt(1.5, 0.0, 0.0, 1.0),
                                                   vel, 700, 200);
    assert(otherClass != first && otherClass != far);
    std::cout << "[PASS] dynamic: 跨源关联(同物合并/异类异处新建)\n";
}

// 伪代码(状态推进):
//   1. 零速度上报:Active 且 stationarySince 记录;
//   2. update(500) 未超静止阈值:仍 Active;update(1500) 超时:Stationary;
//   3. 运动中上报(非零速度):复活为 Active;
//   4. update(最后观测 + 归档超时):Archived;断言 queryActive 查不到、
//      trajectoryOf 仍可回溯。
void testStateProgression() {
    DynamicLayer layer(1000, 5000, 64, 2000);
    const std::array<double, 3> zero{0.0, 0.0, 0.0};
    layer.report(1, "car", boxAt(5.0, 5.0, 5.0, 1.0), zero, 0, 1);
    layer.update(500);
    assert(layer.find(1)->state == InstanceState::Active);
    layer.update(1500);
    assert(layer.find(1)->state == InstanceState::Stationary);

    layer.report(1, "car", boxAt(15.0, 5.0, 5.0, 1.0), {10.0, 0.0, 0.0}, 2000, 1);
    assert(layer.find(1)->state == InstanceState::Active);  // 重新运动复活

    layer.update(7000);  // 2000 + 5000 观测中断超时
    assert(layer.find(1)->state == InstanceState::Archived);
    assert(layer.queryActive(boxAt(15.0, 5.0, 5.0, 2.0), TimeRange{0, 10000}).empty());
    assert(layer.trajectoryOf(1).size() == 2);  // 轨迹保留可回溯
    std::cout << "[PASS] dynamic: 状态推进 Active->Stationary->Archived\n";
}

// 伪代码(沉淀):
//   1. 启用动态层的服务(静止 1000ms / 归档 100000ms):零速度上报实例 1;
//   2. updateDynamic(1500) 推进为 Stationary;
//   3. promoteStationary(2000):断言返回 1 个静态块 id,块 payload=类别、
//      状态 Pending;confirm 后转 Stable;
//   4. 断言实例已归档(活跃/静止计数归零,归档计数 1)。
void testPromoteStationary() {
    StmbService service(ServiceConfig{100, 10.0, 1000, 1, 0, "", 0.0, 0, 8, {},
                                      1000, 100000, 0});
    const std::array<double, 3> zero{0.0, 0.0, 0.0};
    service.reportMoving(1, "car", boxAt(5.0, 5.0, 5.0, 2.0), zero, 0, 1);
    service.updateDynamic(1500);
    assert(service.dynamicStats().stationaryCount == 1);

    const auto created = service.promoteStationary(2000);
    assert(created.size() == 1 && created[0] == 1);
    const auto block = service.get(1);
    assert(block.has_value() && block->payload == "car");
    assert(block->state == BlockState::Pending);  // 走待确认流程
    assert(service.confirm(1, 3000));
    assert(service.get(1)->state == BlockState::Stable);

    const DynamicStats ds = service.dynamicStats();
    assert(ds.activeCount == 0 && ds.stationaryCount == 0 && ds.archivedCount == 1);
    std::cout << "[PASS] dynamic: 沉淀 Stationary->静态块(Pending->Stable)\n";
}

// 伪代码(临时占用):
//   1. 静态层写入建筑块;上报一个与建筑相交的活跃车辆实例;
//   2. 查询建筑:断言命中块的 temporarilyOccupiedBy 含车辆实例 id;
//   3. 观测中断超时使车辆归档:再次查询断言占用标记消失。
void testTemporaryOccupancy() {
    StmbService service(ServiceConfig{100, 10.0, 1000, 1, 0, "", 0.0, 0, 8, {},
                                      1000, 5000, 0});
    MemoryBlock building;
    building.region = boxAt(5.0, 5.0, 5.0, 2.0);
    building.payload = "building";
    building.timestamp = 100;
    service.put(building);

    service.reportMoving(1, "car", boxAt(6.0, 5.0, 5.0, 1.0), {1.0, 0.0, 0.0},
                         1000, 1);
    auto hits = service.query(boxAt(5.0, 5.0, 5.0, 3.0), TimeRange{0, 2000});
    assert(hits.size() == 1 && hits[0].temporarilyOccupiedBy.size() == 1);
    assert(hits[0].temporarilyOccupiedBy[0] == 1);

    service.updateDynamic(6000 + 1000);  // 车辆归档
    hits = service.query(boxAt(5.0, 5.0, 5.0, 3.0), TimeRange{0, 2000});
    assert(hits.size() == 1 && hits[0].temporarilyOccupiedBy.empty());
    std::cout << "[PASS] dynamic: 临时占用标记随活跃状态出现/消失\n";
}

// 伪代码(持久化):
//   1. 单文件模式:服务 A 上报实例(2 轨迹点)并 checkpoint;同目录重建
//      服务 B:断言实例数、轨迹、状态一致;
//   2. 分片模式:同样上报 + 写入静态块 + checkpoint;重建后断言动态层与
//      静态层都恢复(dynamic.stmb 路径)。
void testDynamicPersistence() {
    const std::array<double, 3> zero{0.0, 0.0, 0.0};
    {
        const std::string dir = freshDir("dynamic_persist");
        {
            StmbService service(ServiceConfig{100, 10.0, 1000, 1, 0, dir,
                                              0.0, 0, 8, {}, 1000, 60000, 0});
            service.reportMoving(1, "car", boxAt(0.0, 0.0, 0.0, 1.0), {1.0, 0.0, 0.0},
                                 0, 1);
            service.reportMoving(1, "car", boxAt(5.0, 0.0, 0.0, 1.0), zero, 1000, 1);
            assert(service.checkpoint());
        }
        StmbService service(ServiceConfig{100, 10.0, 1000, 1, 0, dir,
                                          0.0, 0, 8, {}, 1000, 60000});
        assert(service.dynamicStats().activeCount == 1);
        const auto traj = service.trajectoryOf(1);
        assert(traj.size() == 2 && traj[1].position[0] == 5.0);
        assert(service.queryDynamic(boxAt(5.0, 0.0, 0.0, 2.0),
                                    TimeRange{0, 10000}).size() == 1);
        fs::remove_all(dir);
    }
    {
        const std::string dir = freshDir("dynamic_persist_shard");
        {
            StmbService service(ServiceConfig{100, 10.0, 1000, 1, 0, dir,
                                              100.0, 10000, 2, {}, 1000, 60000, 0});
            MemoryBlock b;
            b.region = boxAt(5.0, 5.0, 5.0, 2.0);
            b.payload = "building";
            b.timestamp = 100;
            service.put(b);
            service.reportMoving(1, "car", boxAt(5.0, 5.0, 5.0, 1.0), zero, 200, 1);
            assert(service.checkpoint());
        }
        StmbService service(ServiceConfig{100, 10.0, 1000, 1, 0, dir,
                                          100.0, 10000, 2, {}, 1000, 60000});
        assert(service.dynamicStats().activeCount == 1);
        assert(service.trajectoryOf(1).size() == 1);
        assert(service.query(boxAt(5.0, 5.0, 5.0, 3.0), TimeRange{0, 1000}).size() == 1);
        fs::remove_all(dir);
    }
    std::cout << "[PASS] dynamic: 持久化(单文件/分片)重启后实例轨迹一致\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用六个测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testReportAndTrajectory();
    testCrossSourceAssociation();
    testStateProgression();
    testPromoteStationary();
    testTemporaryOccupancy();
    testDynamicPersistence();
    std::cout << "ALL TESTS PASS (dynamic)\n";
    return 0;
}
