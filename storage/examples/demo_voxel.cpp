// 演示：voxel 时空记忆块的存储 —— 写入块、版本链回溯、时空联合查询、动态实例。

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

#include "voxelstore/backends/sql_voxel_store.h"

using namespace voxelstore;

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    SqlVoxelStore store("demo_voxel.db");  // 当前目录生成 demo_voxel.db

    // 写入两个静态块
    VoxelBlock house;
    house.id = 1;
    house.region = Aabb{0, 0, 0, 10, 10, 10};
    house.payload = "一座房子";
    house.timestamp = 1000;
    house.version = 1;
    house.state = "Stable";
    house.confidence = 0.95;
    store.put_block(house);
    store.append_version(VoxelVersion{1, 1, "一座房子", 0.95, "Stable", 1000, std::nullopt});

    VoxelBlock tree;
    tree.id = 2;
    tree.region = Aabb{20, 20, 0, 22, 22, 5};
    tree.payload = "一棵大树";
    tree.timestamp = 1200;
    tree.version = 1;
    tree.state = "Stable";
    store.put_block(tree);

    // 房子翻修：seal 旧版本 + 写入新当前态 + 归档新版本
    store.seal_version(1, 2000);
    house.payload = "一座两层房子";
    house.version = 2;
    house.timestamp = 2000;
    store.put_block(house);
    store.append_version(VoxelVersion{1, 2, "一座两层房子", 0.97, "Stable", 2000,
                                      std::nullopt});

    // 动态实例：一辆移动的车
    VoxelInstance car;
    car.id = 1;
    car.class_label = "car";
    car.bounds = Aabb{10, 4, 0, 12, 6, 2};
    car.latest = TrackPoint{3000, 11, 5, 1, 5.0, 0.0, 0.0};
    car.trajectory = {TrackPoint{1000, 1, 5, 1, 5.0, 0, 0},
                      TrackPoint{2000, 6, 5, 1, 5.0, 0, 0},
                      TrackPoint{3000, 11, 5, 1, 5.0, 0, 0}};
    car.sources = {42};
    car.state = "Active";
    car.last_seen = 3000;
    store.put_instance(car);

    // 时空联合查询：区域 [0,0,0,15,15,15] × 时间 [0, 2500]
    std::printf("区域 [0,0,0,15,15,15] × t∈[0,2500] 的块：\n");
    for (const auto& b : store.query_blocks(Aabb{0, 0, 0, 15, 15, 15}, 0, 2500, std::nullopt))
        std::printf("  [id=%llu][t=%lld] %s\n", (unsigned long long)b.id,
                    (long long)b.timestamp, b.payload.c_str());

    // 时间回溯："房子在 t=1500 时是什么？"
    std::printf("\n块 1 在 t=1500 时（版本回溯）：%s\n",
                store.version_at(1, 1500)->payload.c_str());
    std::printf("块 1 在 t=2500 时（版本回溯）：%s\n",
                store.version_at(1, 2500)->payload.c_str());

    // 动态实例轨迹
    auto c = store.get_instance(1);
    std::printf("\n动态实例 %llu（%s，%s）轨迹：\n", (unsigned long long)c->id,
                c->class_label.c_str(), c->state.c_str());
    for (const auto& p : c->trajectory)
        std::printf("  t=%lld 位置(%.1f, %.1f, %.1f)\n", (long long)p.t, p.px, p.py, p.pz);
    return 0;
}
