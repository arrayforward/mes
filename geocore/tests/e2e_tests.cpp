// ============================================================================
// e2e_tests.cpp —— 端到端测试：规范 §8 测试与验证清单的完整落地
// ============================================================================
//
// 【本文件的实现思路】
// 逐条实现规范 §8 的 T-INV / T-P / T-F 系列场景（规模按参考实现等比
// 缩小，语义不变），外加跨场景换算（算法 S5）与跨场景 Find 的验证：
//
//   T-INV-1  随机插入/删除后物化最小性（无空洞、无冗余空节点）
//   T-INV-2  归属最深性 + Find 结果无重复 id
//   T-INV-3  非法坐标写入即规范化（P4）
//   T-INV-5  多线程并发 Transfer + Find：不丢对象、不重复、不死锁
//   T-P1     圆轨道 t 推进 10^9 s 后相位精度（圈数取模）
//   T-P3/P5  往返换算一致性 < 1e-12；LCA 路径误差 ≤ 绕根路径
//   T-P-尺度  10^11 m 与 10^-2 m 锚点互换算，误差不随尺度差恶化
//   T-F-邻域  跨 cell 边界半径查询与暴力扫描对拍
//   T-F-球面  反经线（lon=±180°）邻域展开与大圆距离对拍
//   T-F-列车  MovingFrame1D 越界钳制行为
//   T-F-休眠  Dormant 场景 Convert 可用、Find 报错不崩溃
//   T-F-速度  自转行星表面静止物体 Transfer 含表面线速度项（论文 §7.4）
//   跨场景    远亲烘焙路径与行走路径对拍 + FindCrossScene
//
// 测试场景（除特别说明外每个用例独立 Kernel 实例）：
//   [1] solar    Euclidean3D extent=2^39 m（行星系，内含运动行星锚点）
//   [2] base     Euclidean3D extent=2^21 m（基地，INV/Find 主战场）
//   [3] surface  SphericalSurface R=6.371e6 m（行星地表，球面用例）
//   [4] train    MovingFrame1D length=100 km（一维线路用例）
// ============================================================================
#include <algorithm>
#include <atomic>
#include <cmath>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "../tests/check.hpp"
#include "geocore/kernel.hpp"

using namespace geocore;

namespace {

// 便捷构造路径：P({1,3,5}) = [1,3,5]
AnchorPath P(std::initializer_list<uint32_t> segs) {
    AnchorPath p;
    for (uint32_t s : segs) p.AppendInPlace(s);
    return p;
}

constexpr double kSolarExtent = 549755813888.0;  // 2^39
constexpr double kBaseExtent = 2097152.0;        // 2^21

// 暴力参考：对象绝对位置（场景根原生坐标）——内核换算重建 + 根中心还原
// （ConvertPoint 停在场景根 q-空间，非零根中心模型需补回 RootCenter）
Vec3d AbsPos(Kernel& k, SceneId sid, const ObjectRecord& rec, double t) {
    const Scene* sc = k.GetScene(sid);
    return k.ConvertPoint(rec.anchor, sc->rootPath, rec.pos, t) +
           ModelRootCenter(sc->model);
}

} // namespace

// ============================================================================
// T-INV-1：随机插入/删除后，物化表恰好覆盖非空区域（语义节点除外）
// ============================================================================
void TestInv1() {
    Kernel k;
    SceneId b = k.CreateScene("base", 0, P({2}), Euclidean3D{kBaseExtent});
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> pos(-1.0e6, 1.0e6);
    std::uniform_real_distribution<double> logSize(-3.0, 2.0);
    std::vector<ObjectId> live;
    for (int i = 0; i < 100000; ++i) {
        if (live.empty() || (rng() % 100) < 60) {
            ObjectId id = k.Save(b, {pos(rng), pos(rng), pos(rng)}, {},
                                 std::pow(10.0, logSize(rng)));
            live.push_back(id);
        } else {
            size_t idx = rng() % live.size();
            k.Remove(live[idx]);
            live[idx] = live.back();
            live.pop_back();
        }
    }
    // INV-1：每个物化节点必须 含对象 ∨ 含物化子节点 ∨ 语义节点
    std::atomic<bool> ok{true};
    k.Tree().ForEachNode([&](const NodeData& nd) {
        if (!nd.semantic && nd.bucket.empty() && nd.children.empty())
            ok.store(false);
    });
    CHECK(ok.load());
    CHECK(k.ObjectCount() == live.size());
}

// ============================================================================
// T-INV-2：归属最深性（AnchorOf 可复算）+ Find 结果无重复 id
// ============================================================================
void TestInv2() {
    Kernel k;
    SceneId b = k.CreateScene("base", 0, P({2}), Euclidean3D{kBaseExtent});
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> pos(-5.0e5, 5.0e5);
    std::uniform_real_distribution<double> logSize(-2.0, 2.0);
    struct E { ObjectId id; Vec3d p; double size; };
    std::vector<E> objs;
    for (int i = 0; i < 500; ++i) {
        Vec3d p{pos(rng), pos(rng), pos(rng)};
        double s = std::pow(10.0, logSize(rng));
        objs.push_back({k.Save(b, p, {}, s), p, s});
    }
    AnchorPath root = P({2});
    for (const E& e : objs) {
        ObjectRecord rec;
        CHECK(k.GetObject(e.id, rec));
        // 归属最深性：归属路径与 AnchorOf 重新定位一致（INV-2）
        CHECK(rec.anchor == k.AnchorOf(b, e.p, e.size));
        // Find 结果无重复 id（INV-2 的查询面推论）
        FindResult r = k.Find(b, root, e.p, e.size * 4.0, 0.0);
        CHECK(r.status == FindStatus::Ok);
        std::set<ObjectId> uniq(r.hits.begin(), r.hits.end());
        CHECK(uniq.size() == r.hits.size());
        CHECK(uniq.count(e.id) == 1);
    }
}

// ============================================================================
// T-INV-3：注入非法坐标（lon=181°、s<0、θ 超界），存储值必为规范化主值
// ============================================================================
void TestInv3() {
    Kernel k;
    SceneId sph = k.CreateScene("surface", 0, P({3}), SphericalSurface{});
    SceneId trn = k.CreateScene("train", 0, P({4}), MovingFrame1D{});
    ObjectRecord rec;

    ObjectId o1 = k.Save(sph, {10.0, 181.0, 0.0}, {}, 100.0);
    CHECK(k.GetObject(o1, rec));
    CHECK_NEAR(AbsPos(k, sph, rec, 0.0).y, -179.0, 1e-9);  // lon=181 → −179

    ObjectId o2 = k.Save(trn, {-5.0, 1.0, 0.0}, {}, 10.0);
    CHECK(k.GetObject(o2, rec));
    CHECK_NEAR(AbsPos(k, trn, rec, 0.0).x, 0.0, 1e-9);     // s<0 → 0（钳制）

    ObjectId o3 = k.Save(trn, {150000.0, 0.0, 0.0}, {}, 10.0);
    CHECK(k.GetObject(o3, rec));
    CHECK_NEAR(AbsPos(k, trn, rec, 0.0).x, 100000.0, 1e-6);
}

// ============================================================================
// T-INV-5：多线程并发 Transfer + Find 压测——无丢失、无重复、无死锁
// ============================================================================
void TestInv5() {
    Kernel k;
    SceneId b = k.CreateScene("base", 0, P({2}), Euclidean3D{kBaseExtent});
    AnchorPath root = P({2});
    AnchorPath t1 = k.AnchorOf(b, {-8.0e5, -8.0e5, -8.0e5}, 100.0);
    AnchorPath t2 = k.AnchorOf(b, {8.0e5, 8.0e5, 8.0e5}, 100.0);
    std::vector<ObjectId> ids;
    for (int i = 0; i < 200; ++i)
        ids.push_back(k.Save(b, {-8.0e5 + i, -8.0e5, -8.0e5}, {}, 100.0));

    std::atomic<bool> stop{false};
    std::atomic<int> findErrors{0};
    // 查询线程：结果集不允许出现重复 id（INV-2 的查询面推论）
    std::vector<std::thread> readers;
    for (int i = 0; i < 2; ++i)
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                FindResult r = k.Find(b, root, {0, 0, 0}, 3.0e6, 0.0);
                if (r.status != FindStatus::Ok) {
                    findErrors.fetch_add(1);
                    continue;
                }
                std::set<ObjectId> uniq(r.hits.begin(), r.hits.end());
                if (uniq.size() != r.hits.size()) findErrors.fetch_add(1);
            }
        });
    // 迁移线程：全部对象在两锚点间往返（先写后删由内核保证，INV-5）
    std::vector<std::thread> movers;
    for (int i = 0; i < 4; ++i)
        movers.emplace_back([&] {
            for (int round = 0; round < 10; ++round) {
                for (ObjectId id : ids) k.Transfer(id, t2, 0.0);
                for (ObjectId id : ids) k.Transfer(id, t1, 0.0);
            }
        });
    for (auto& t : movers) t.join();
    stop.store(true);
    for (auto& t : readers) t.join();

    CHECK(findErrors.load() == 0);
    CHECK(k.ObjectCount() == ids.size());  // 无丢失
    // 收尾一致性：每条记录的归属桶确实持有该对象
    bool membership = true;
    for (ObjectId id : ids) {
        ObjectRecord rec;
        if (!k.GetObject(id, rec)) { membership = false; break; }
        std::vector<uint64_t> bucket = k.Tree().BucketCopy(rec.anchor);
        if (std::find(bucket.begin(), bucket.end(), id) == bucket.end())
            membership = false;
    }
    CHECK(membership);
}

// ============================================================================
// T-P1：圆轨道锚点 t 推进 10^9 s 后与同相位处偏差（圈数取模纪律）
// ============================================================================
void TestP1() {
    Kernel k;
    k.CreateScene("solar", 0, P({1}), Euclidean3D{kSolarExtent});
    AnchorPath planet = k.CreateAnchor(
        P({1}), Kinematic::Orbit(1.5e11, 2.0e6, 0.123456789));
    Transform a = k.TransformAt(planet, 0.0);
    Transform b = k.TransformAt(planet, 1.0e9);  // 10^9 恰为周期整数倍
    double rel = (b.origin - a.origin).Norm() / 1.5e11;
    CHECK(rel < 1e-12);  // 机器精度 × 常数，而非随运行时长漂移
}

// ============================================================================
// T-P3/P5：往返换算一致性 < 1e-12（同尺度局部），且 LCA 路径误差
// 严格不大于绕根路径（P5 的实验验证）
// ============================================================================
void TestP3P5() {
    Kernel k;
    k.CreateScene("solar", 0, P({1}), Euclidean3D{kSolarExtent});
    AnchorPath planet = k.CreateAnchor(
        P({1}), Kinematic::Orbit(1.5e11, 31557600.0, 0.123,
                                 Quaternion::Identity(), 86164.0));
    AnchorPath base = k.CreateAnchor(planet, Kinematic::Static({6.371e6, 0, 0}));
    AnchorPath rover = k.CreateAnchor(base, Kinematic::Static({10.0, 0, 0}));
    AnchorPath antenna = k.CreateAnchor(base, Kinematic::Static({-50.0, 20.0, 5.0}));
    const double t = 123456.789;  // 任意帧时刻（P3：全链共享）

    // 随机锚点对 + 随机局部点的往返一致性
    std::mt19937_64 rng(11);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    AnchorPath pairs[2][2] = {{rover, antenna}, {rover, base}};
    for (auto& pr : pairs) {
        for (int i = 0; i < 20; ++i) {
            Vec3d p{u(rng), u(rng), u(rng)};
            Vec3d q = k.ConvertPoint(pr[0], pr[1], p, t);
            Vec3d r = k.ConvertPoint(pr[1], pr[0], q, t);
            double rel = (r - p).Norm() / (p.Norm() + 1e-300);
            CHECK(rel < 1e-12);
        }
    }

    // LCA 行走 vs 绕全局根：前者误差严格不大于后者
    Vec3d p{0.5, -0.3, 0.8};
    Vec3d qL = k.ConvertPoint(rover, antenna, p, t);
    Vec3d rL = k.ConvertPoint(antenna, rover, qL, t);
    double errLca = (rL - p).Norm();
    Vec3d g1 = k.ConvertPoint(rover, AnchorPath{}, p, t);      // 绕根上行
    Vec3d g2 = k.ConvertPoint(AnchorPath{}, antenna, g1, t);   // 绕根下行
    Vec3d g3 = k.ConvertPoint(antenna, AnchorPath{}, g2, t);
    Vec3d g4 = k.ConvertPoint(AnchorPath{}, rover, g3, t);
    double errRoot = (g4 - p).Norm();
    CHECK(errLca < 1e-12);
    CHECK(errLca <= errRoot * (1.0 + 1e-9));
}

// ============================================================================
// T-P-尺度：10^11 m 锚点与 10^-2 m 锚点互换算往返，相对误差不随
// 路径两端尺度差恶化（误差只随链长增长）
// ============================================================================
void TestPScale() {
    Kernel k;
    SceneId a = k.CreateScene("solar", 0, P({1}), Euclidean3D{kSolarExtent});
    SceneId b = k.CreateScene("base", 0, P({2}), Euclidean3D{kBaseExtent});
    // 大尺度锚点：行星轨道尺度 1.5e11 m 处的 10 m 对象
    ObjectId big = k.Save(a, {1.5e11, 5.0e10, -2.0e10}, {}, 10.0);
    // 小尺度锚点：基地内厘米级螺丝
    ObjectId small = k.Save(b, {0.005, 0.005, 0.005}, {}, 1.0e-3);
    // 同尺度参照：基地内另一个厘米级锚点
    ObjectId small2 = k.Save(b, {0.008, 0.005, 0.005}, {}, 1.0e-3);
    ObjectRecord rb, rs, rs2;
    CHECK(k.GetObject(big, rb) && k.GetObject(small, rs) && k.GetObject(small2, rs2));

    Vec3d p{1e-3, 2e-3, -1e-3};
    // 跨尺度往返：误差下限由链上最大中间帧（全局 ~10^12 m）的 ulp 决定
    Vec3d q = k.ConvertPoint(rb.anchor, rs.anchor, p, 0.0);
    Vec3d r = k.ConvertPoint(rs.anchor, rb.anchor, q, 0.0);
    double errCross = (r - p).Norm();
    // 同尺度往返：误差应在 1e-12 相对量级
    Vec3d q2 = k.ConvertPoint(rs.anchor, rs2.anchor, p, 0.0);
    Vec3d r2 = k.ConvertPoint(rs2.anchor, rs.anchor, q2, 0.0);
    double errSame = (r2 - p).Norm() / p.Norm();
    CHECK(errSame < 1e-12);
    // 跨尺度误差被链长 × 中间帧 ulp（~1e-4 m）界定，而非失控放大
    CHECK(errCross < 1e-2);
}

// ============================================================================
// T-F-邻域：跨 cell 边界的半径查询不漏对象（与暴力结果对拍）
// ============================================================================
void TestFindNeighborhood() {
    Kernel k;
    SceneId b = k.CreateScene("base", 0, P({2}), Euclidean3D{kBaseExtent});
    AnchorPath root = P({2});
    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> pos(-1.0e5, 1.0e5);
    std::vector<std::pair<ObjectId, Vec3d>> objs;
    for (int i = 0; i < 2000; ++i) {
        Vec3d p{pos(rng), pos(rng), pos(rng)};
        objs.emplace_back(k.Save(b, p, {}, 1.0), p);
    }
    // 查询中心刻意包含 cell 边界点（0 在每一层都是边界）
    std::vector<Vec3d> centers = {{0, 0, 0}, {1024.0, -1024.0, 0.0}};
    std::uniform_real_distribution<double> cd(-1.0e5, 1.0e5);
    for (int i = 0; i < 100; ++i) centers.push_back({cd(rng), cd(rng), cd(rng)});
    std::uniform_real_distribution<double> rd(10.0, 2000.0);

    for (const Vec3d& c : centers) {
        double radius = rd(rng);
        FindResult r = k.Find(b, root, c, radius, 0.0);
        CHECK(r.status == FindStatus::Ok);
        std::set<ObjectId> got(r.hits.begin(), r.hits.end());
        // 暴力对拍：边界带（|d²−r²|/r² < 1e-9）内的对象豁免，
        // 消除"残余重建的最后 1 ulp"造成的边界抖动
        double r2v = radius * radius;
        bool mismatch = false;
        for (const auto& [id, p] : objs) {
            double d2 = (p - c).NormSq();
            if (std::fabs(d2 - r2v) < 1e-9 * r2v) continue;
            bool inBrute = d2 <= r2v;
            if (inBrute != (got.count(id) != 0)) mismatch = true;
        }
        CHECK(!mismatch);
        // 结果按距离升序（规范 §7 Find 契约）
        CHECK(std::is_sorted(r.hits.begin(), r.hits.end(),
            [&](ObjectId x, ObjectId y) {
                ObjectRecord rx, ry;
                k.GetObject(x, rx);
                k.GetObject(y, ry);
                return (AbsPos(k, b, rx, 0.0) - c).NormSq() <
                       (AbsPos(k, b, ry, 0.0) - c).NormSq();
            }));
    }
    // Top-N：大半径覆盖全场景，恰好返回最近的 N 个
    FindResult top = k.Find(b, root, centers[0], 1.0e6, 0.0, 7);
    CHECK(top.hits.size() == 7);
}

// ============================================================================
// T-F-球面：跨反经线（lon=±180°）的邻域展开正确；大圆距离与对拍一致
// ============================================================================
void TestSphericalAntimeridian() {
    Kernel k;
    SceneId sph = k.CreateScene("surface", 0, P({3}), SphericalSurface{});
    AnchorPath root = P({3});
    SphericalSurface metric{};
    std::vector<std::pair<ObjectId, Vec3d>> objs;
    // 反经线两侧网格
    for (double lat : {-1.0, -0.5, 0.0, 0.5, 1.0})
        for (double lon : {179.9, 179.95, 179.99, 180.0,
                           -180.0, -179.99, -179.95, -179.9})
            objs.emplace_back(k.Save(sph, {lat, lon, 0.0}, {}, 100.0),
                              metric.Normalize({lat, lon, 0.0}));
    // 全球随机背景
    std::mt19937_64 rng(5);
    std::uniform_real_distribution<double> ulat(-80.0, 80.0), ulon(-180.0, 180.0);
    for (int i = 0; i < 200; ++i) {
        Vec3d p{ulat(rng), ulon(rng), 0.0};
        objs.emplace_back(k.Save(sph, p, {}, 100.0), metric.Normalize(p));
    }

    auto crossCheck = [&](Vec3d center, double radius) {
        FindResult r = k.Find(sph, root, center, radius, 0.0);
        CHECK(r.status == FindStatus::Ok);
        std::set<ObjectId> got(r.hits.begin(), r.hits.end());
        double r2v = radius * radius;
        bool mismatch = false;
        for (const auto& [id, p] : objs) {
            double d2 = metric.DistanceSq(p, center);
            if (std::fabs(d2 - r2v) < 1e-9 * r2v) continue;  // 边界带豁免
            if ((d2 <= r2v) != (got.count(id) != 0)) mismatch = true;
        }
        return !mismatch;
    };
    CHECK(crossCheck({0.0, 180.0, 0.0}, 50000.0));   // 反经线正中心
    CHECK(crossCheck({0.5, -179.95, 0.0}, 30000.0)); // 反经线西缘
    CHECK(crossCheck({31.2304, 121.4737, 0.0}, 100000.0));  // 常规区域
    CHECK(crossCheck({80.0, 0.0, 0.0}, 200000.0));   // 高纬度（经度放宽）
}

// ============================================================================
// T-F-列车：MovingFrame1D 对象驶出线路末端时钳制（规格允许的两种行为
// 之一，本实现选择钳制并声明）；沿程查询按 |Δs| 语义工作
// ============================================================================
void TestTrain() {
    Kernel k;
    SceneId trn = k.CreateScene("train", 0, P({4}), MovingFrame1D{});
    AnchorPath root = P({4});
    ObjectRecord rec;
    ObjectId tail = k.Save(trn, {150000.0, 0.0, 0.0}, {}, 10.0);
    CHECK(k.GetObject(tail, rec));
    CHECK_NEAR(AbsPos(k, trn, rec, 0.0).x, 100000.0, 1e-6);  // 末端钳制

    std::vector<ObjectId> cars;
    for (double s : {1000.0, 2000.0, 3000.0, 50000.0})
        cars.push_back(k.Save(trn, {s, 0.0, 0.0}, {}, 10.0));
    FindResult r = k.Find(trn, root, {1500.0, 0.0, 0.0}, 600.0, 0.0);
    std::set<ObjectId> got(r.hits.begin(), r.hits.end());
    CHECK(got.count(cars[0]) == 1 && got.count(cars[1]) == 1);
    CHECK(got.count(cars[2]) == 0 && got.count(cars[3]) == 0);
    // 按距离升序：1000 m（Δs=500）先于 2000 m（Δs=500）——同距并列无序要求
    CHECK(r.hits.size() == 2);
}

// ============================================================================
// T-F-休眠：目标场景 Dormant 时 Convert 仍正确（元数据可用），
// Find 返回"场景未加载"错误而非崩溃
// ============================================================================
void TestDormant() {
    Kernel k;
    SceneId a = k.CreateScene("solar", 0, P({1}), Euclidean3D{kSolarExtent});
    SceneId b = k.CreateScene("base", 0, P({2}), Euclidean3D{kBaseExtent});
    ObjectId o = k.Save(b, {100.0, 200.0, 300.0}, {}, 10.0);
    ObjectRecord rec;
    CHECK(k.GetObject(o, rec));
    AnchorPath anchorInB = rec.anchor;

    k.SetSceneState(b, SceneState::Dormant);
    // Find 报错而非崩溃（T-F-休眠契约）
    FindResult r = k.Find(b, P({2}), {100.0, 200.0, 300.0}, 1000.0, 0.0);
    CHECK(r.status == FindStatus::SceneNotLoaded);
    CHECK(k.Find(999, P({2}), {}, 1.0, 0.0).status == FindStatus::SceneNotFound);
    // Convert 仍可用：rootPath、根变换、运动学元数据保留（规范 §8.2）
    Vec3d g = k.Convert(anchorInB, k.GetScene(a)->rootPath, {0, 0, 0}, 0.0);
    CHECK(std::isfinite(g.x) && std::isfinite(g.y) && std::isfinite(g.z));
    // 物化数据已释放（对象记录随物化数据一并清除）
    CHECK(k.ObjectCount() == 0);

    k.SetSceneState(b, SceneState::Resident);  // 恢复后查询通道恢复
    CHECK(k.Find(b, P({2}), {0, 0, 0}, 1.0e6, 0.0).status == FindStatus::Ok);
}

// ============================================================================
// T-F-速度：自转行星表面静止物体 Transfer 到轨道锚点后，速度包含
// 表面线速度项（论文 §7.4：地球赤道约 465 m/s），与解析解对拍
// 场景：行星表面球面场景（父子场景嵌套）挂载在运动行星锚点上
// ============================================================================
void TestVelocityFrameTerms() {
    constexpr double R_ORBIT = 1.5e11, T_YEAR = 31557600.0, T_SPIN = 86164.0;
    constexpr double R_PLANET = 6371000.0;
    Kernel k;
    k.CreateScene("solar", 0, P({1}), Euclidean3D{kSolarExtent});
    AnchorPath planet = k.CreateAnchor(
        P({1}), Kinematic::Orbit(R_ORBIT, T_YEAR, 0.0,
                                 Quaternion::Identity(), T_SPIN));
    // 父子场景：表面球面场景挂载在行星锚点（规范 §7 显式父子场景）
    SceneId surf = k.CreateScene("surface", 0, planet, SphericalSurface{R_PLANET},
                                 std::nullopt, /*allowNested=*/true);
    CHECK(surf != kInvalidScene);
    // 烘焙合法性：行星链含运动节点，surface 场景不可烘焙（论文 §7.3）
    CHECK(!k.GetScene(surf)->bakedValid);
    CHECK(k.GetScene(0)->bakedValid);

    // 静止在赤道表面的物体（本系速度为 0）
    ObjectId o = k.Save(surf, {0.0, 0.0, 0.0}, {}, 0.1);
    CHECK(o != kInvalidObject);

    // 迁移目标：行星系场景内、地表外侧 8e6 m 处的匿名锚点
    AnchorPath tgt = k.AnchorOf(0, {R_ORBIT + 8.0e6, 0.0, 0.0}, 100.0);
    k.Transfer(o, tgt, 0.0);

    // 解析解：v = ȯ_orbit + ω×(R·p_surface)
    double orbitSpeed = 2.0 * kPi * R_ORBIT / T_YEAR;      // ≈ 29.87 km/s
    double surfSpeed = (2.0 * kPi / T_SPIN) * R_PLANET;    // ≈ 464.6 m/s
    ObjectRecord rec;
    CHECK(k.GetObject(o, rec));
    CHECK_REL(rec.vel.y, orbitSpeed + surfSpeed, 1e-3);
    CHECK_NEAR(rec.vel.x, 0.0, 1.0);
    CHECK_NEAR(rec.vel.z, 0.0, 1.0);
    // 位置同步切换：落在行星系场景的预期位置
    Vec3d abs = k.ConvertPoint(rec.anchor, P({1}), rec.pos, 0.0);
    CHECK_NEAR(abs.x, R_ORBIT + R_PLANET, 1000.0);
    CHECK_NEAR(abs.y, 0.0, 1000.0);
}

// ============================================================================
// 跨场景换算（算法 S5）：远亲烘焙路径与行走路径对拍 + 热对缓存 +
// FindCrossScene（半径边界点重估）
// ============================================================================
void TestCrossScene() {
    Kernel k;
    SceneId a = k.CreateScene("solar", 0, P({1}), Euclidean3D{kSolarExtent});
    SceneId b = k.CreateScene("base", 0, P({2}), Euclidean3D{kBaseExtent});
    CHECK(k.GetScene(a)->bakedValid && k.GetScene(b)->bakedValid);

    ObjectId oa = k.Save(a, {1.0e11, 5.0e10, -2.0e10}, {}, 10.0);
    ObjectId ob = k.Save(b, {1000.0, -500.0, 200.0}, {}, 1.0);
    ObjectRecord ra, rb;
    CHECK(k.GetObject(oa, ra) && k.GetObject(ob, rb));

    // 烘焙路径（S5 远亲）与行走路径（S3）对拍
    Vec3d p{0.5, -0.25, 1.0};
    Vec3d walk = k.ConvertPoint(ra.anchor, rb.anchor, p, 0.0);
    Vec3d baked = k.Convert(ra.anchor, rb.anchor, p, 0.0);
    CHECK((walk - baked).Norm() < 1e-2);  // 全局 10^12 m 帧的 ulp 量级
    // 热场景对缓存命中（第二次调用）结果一致
    Vec3d baked2 = k.Convert(ra.anchor, rb.anchor, p, 0.0);
    CHECK((baked2 - baked).NormSq() == 0.0);
    // 往返
    Vec3d back = k.Convert(rb.anchor, ra.anchor, baked, 0.0);
    CHECK((back - p).Norm() < 1e-2);

    // FindCrossScene：从 A 场景上下文查询 B 场景原点 500 m 内的对象
    ObjectId o1 = k.Save(b, {10.0, 0.0, 0.0}, {}, 1.0);
    ObjectId o2 = k.Save(b, {800.0, 0.0, 0.0}, {}, 1.0);
    Vec3d bOriginInA = k.Convert(P({2}), P({1}), {0, 0, 0}, 0.0);
    FindResult r = k.FindCrossScene(b, P({1}), bOriginInA, 500.0, 0.0);
    CHECK(r.status == FindStatus::Ok);
    std::set<ObjectId> got(r.hits.begin(), r.hits.end());
    CHECK(got.count(o1) == 1);
    CHECK(got.count(o2) == 0);
}

// ============================================================================
// 测试入口
// ============================================================================
void RunAll() {
    TestInv1();
    TestInv2();
    TestInv3();
    TestInv5();
    TestP1();
    TestP3P5();
    TestPScale();
    TestFindNeighborhood();
    TestSphericalAntimeridian();
    TestTrain();
    TestDormant();
    TestVelocityFrameTerms();
    TestCrossScene();
}

TEST_MAIN("e2e")
