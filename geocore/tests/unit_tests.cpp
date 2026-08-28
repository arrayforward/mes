// ============================================================================
// unit_tests.cpp —— 单元测试：地基组件逐件验证
// ============================================================================
//
// 【本文件的实现思路】
// 对应规范 §9.1 检查表前 3 条的组件级验证，为端到端测试（e2e_tests.cpp）
// 打底：
//   1. AnchorPath：四基本运算 + HashKey 两种编码路径（打包/FNV）；
//   2. 数学库：四元数旋转、Transform 复合/求逆的封闭性；
//   3. Kinematic：P1 圈数取模（10^9 s 推进后相位精度）；
//   4. CoordinateModel：CellOf/ChildOrigin 与 AnchorOf 的重建一致性、
//      各模型 Normalize 主值化（P4）、haversine 基准值、邻域展开计数。
// ============================================================================
#include <cstdio>

#include "../tests/check.hpp"
#include "geocore/anchor_path.hpp"
#include "geocore/coordinate_model.hpp"
#include "geocore/kinematic.hpp"
#include "geocore/math.hpp"

using namespace geocore;

// ----------------------------------------------------------------------------
// AnchorPath：Parent/Prefix/IsPrefixOf/Append/LcaDepth（论文 §5.2 四运算）
// ----------------------------------------------------------------------------
void TestAnchorPath() {
    AnchorPath a;
    a.AppendInPlace(3);
    a.AppendInPlace(5);
    a.AppendInPlace(0);
    a.AppendInPlace(7);
    CHECK(a.depth == 4);

    AnchorPath p = a.Parent();
    CHECK(p.depth == 3 && p.codes[2] == 0);

    AnchorPath pre = a.Prefix(2);
    CHECK(pre.depth == 2 && pre.codes[0] == 3 && pre.codes[1] == 5);
    CHECK(pre.IsPrefixOf(a));
    CHECK(!a.IsPrefixOf(pre));
    CHECK(a.LastSeg() == 7);
    CHECK(a.Append(2).depth == 5);

    // LCA = 最长公共前缀（规范 §2.1）
    AnchorPath b = pre.Append(4).Append(6);
    CHECK(LcaDepth(a, b) == 2);

    // HashKey 打包快路径：深度 ≤ 18 且段码 < 8 ⇒ 整数一一对应
    AnchorPath c;
    c.AppendInPlace(3);
    c.AppendInPlace(5);
    CHECK(c.HashKey() != a.HashKey());
    AnchorPath a2;
    a2.AppendInPlace(3);
    a2.AppendInPlace(5);
    a2.AppendInPlace(0);
    a2.AppendInPlace(7);
    CHECK(a2.HashKey() == a.HashKey());  // 同路径同键
    // 深度不同、段码前缀相同 ⇒ 键不同（深度打包进高位）
    AnchorPath d;
    d.AppendInPlace(3);
    CHECK(d.HashKey() != pre.HashKey() || d == pre);

    // 语义段（≥ kSemanticCodeBase）走 FNV 退化路径，与打包路径隔离
    AnchorPath e;
    e.AppendInPlace(kSemanticCodeBase);
    e.AppendInPlace(1);
    CHECK((e.HashKey() >> 63) == 1);     // 最高位隔离标记
    CHECK(e.HashKey() != a.HashKey());

    // 字符串序列化 / 解析（ToString / ParseAnchorPath）：往返一致
    CHECK(ToString(a) == "3/5/0/7");
    AnchorPath parsed;
    CHECK(ParseAnchorPath("3/5/0/7", parsed) && parsed == a);
    CHECK(ToString(AnchorPath{}).empty());                 // 根 = 空串
    CHECK(ParseAnchorPath("", parsed) && parsed.depth == 0);
    CHECK(ParseAnchorPath("/", parsed) && parsed.depth == 0);
    CHECK(ParseAnchorPath("1", parsed) &&
          parsed.depth == 1 && parsed.codes[0] == 1);
    // 语义段（≥256）同样可往返
    CHECK(ParseAnchorPath(ToString(e), parsed) && parsed == e);
    // 非法输入：空段 / 非数字 / 超深 / 段码超 uint32
    CHECK(!ParseAnchorPath("1//2", parsed));
    CHECK(!ParseAnchorPath("/1", parsed));
    CHECK(!ParseAnchorPath("1/", parsed));
    CHECK(!ParseAnchorPath("gate-1", parsed));
    CHECK(!ParseAnchorPath("1a/2", parsed));
    CHECK(!ParseAnchorPath("4294967296", parsed));         // 2^32
    std::string deep;
    for (int i = 0; i <= kMaxDepth; ++i) deep += (i ? "/1" : "1");
    CHECK(!ParseAnchorPath(deep, parsed));                 // 33 段超深
}

// ----------------------------------------------------------------------------
// 数学库：旋转/复合/求逆的封闭性（Convert 往返 T-P3 的算术基础）
// ----------------------------------------------------------------------------
void TestMath() {
    Quaternion q = Quaternion::FromAxisAngle({0, 0, 1}, 1.234);
    Vec3d v{1.0, -2.0, 3.0};
    Vec3d back = q.Conjugate() * (q * v);
    CHECK_REL(back.x, v.x, 1e-15);
    CHECK_REL(back.y, v.y, 1e-15);
    CHECK_REL(back.z, v.z, 1e-15);

    // Transform：Apply 后 Inverse 还原；Compose(A,B) ≡ 先 B 后 A
    Transform a{Vec3d{10, 0, 0}, Quaternion::FromAxisAngle({0, 1, 0}, 0.7)};
    Transform b{Vec3d{0, -5, 2}, Quaternion::FromAxisAngle({1, 0, 0}, -0.3)};
    Vec3d p{3, 4, 5};
    Vec3d inv = a.Inverse() * (a * p);
    CHECK_REL(inv.x, p.x, 1e-14);
    CHECK_REL(inv.y, p.y, 1e-14);
    CHECK_REL(inv.z, p.z, 1e-14);
    Vec3d c1 = Transform::Compose(a, b) * p;
    Vec3d c2 = a * (b * p);
    CHECK_REL(c1.x, c2.x, 1e-14);
    CHECK_REL(c1.y, c2.y, 1e-14);
    CHECK_REL(c1.z, c2.z, 1e-14);
    // Compose(A,B) 的逆 = Compose(B⁻¹, A⁻¹)
    Vec3d r = Transform::Compose(b.Inverse(), a.Inverse()) * c1;
    CHECK_REL(r.x, p.x, 1e-13);
    CHECK_REL(r.y, p.y, 1e-13);
    CHECK_REL(r.z, p.z, 1e-13);
}

// ----------------------------------------------------------------------------
// Kinematic：P1 圈数取模 —— t = 10^9 s 后与同相位点的偏差（规范 T-P1）
// 取 T = 2×10^6 s（10^9 恰为其整数倍），相位 0.123456789 圈。
// 期望：偏差只含 double 表示误差（~1e-13 相对），而非随运行时长线性增长。
// ----------------------------------------------------------------------------
void TestKinematicP1() {
    const double T = 2.0e6, R = 1.5e11;
    Kinematic k = Kinematic::Orbit(R, T, 0.123456789);
    Vec3d p0 = k.OriginAt(0.0);
    Vec3d p1 = k.OriginAt(1.0e9);
    double rel = (p1 - p0).Norm() / R;
    CHECK(rel < 1e-12);

    // OriginVelAt 闭式：|ȯ| = 2πR/T（切向速度，论文 §7.4）
    double speed = k.OriginVelAt(0.0).Norm();
    CHECK_REL(speed, 2.0 * 3.14159265358979323846 * R / T, 1e-12);

    // 自转：RotationAt 推进一个自转周期后回到原朝向；ω = 2π/spinPeriod
    Kinematic ks = Kinematic::Orbit(0.0, 1.0, 0.0, Quaternion::Identity(), 100.0);
    Vec3d vx{1.0, 0.0, 0.0};
    Vec3d r0 = ks.RotationAt(0.0) * vx;
    Vec3d r1 = ks.RotationAt(100.0) * vx;
    CHECK_REL(r1.x, r0.x, 1e-12);
    CHECK_REL(r1.y, r0.y, 1e-12);
    CHECK_REL(ks.AngularVelAt(0.0).z, 2.0 * 3.14159265358979323846 / 100.0, 1e-12);

    // Static 与 PathConstrained 的最小行为
    Kinematic st = Kinematic::Static({1, 2, 3});
    CHECK(st.OriginAt(999.0).x == 1.0 && st.OriginVelAt(999.0).NormSq() == 0.0);
    Kinematic pc = Kinematic::Path(10.0, 2.5);
    CHECK_REL(pc.OriginAt(4.0).x, 20.0, 1e-15);
    CHECK_REL(pc.OriginVelAt(4.0).x, 2.5, 1e-15);
}

// ----------------------------------------------------------------------------
// 坐标模型：CellOf/ChildOrigin 的重建一致性（AnchorOf 与 TransformAt
// 共用同一套规则生成，是本内核"物化前后变换不变"的根基）
// 伪码：q = p − RootCenter；逐层 q' = q − ChildOrigin(CellOf(q))；
//       反向累加 ChildOrigin 应精确还原 p − RootCenter − residual
// ----------------------------------------------------------------------------
void TestModelReconstruction() {
    Euclidean3D eu;
    eu.extent = 1024.0;
    Vec3d p{123.456, -77.7, 0.03125};
    Vec3d q = p;  // RootCenter = 0
    Vec3d acc{0, 0, 0};
    for (int d = 0; d < 12; ++d) {
        uint32_t c = eu.CellOf(q);
        Vec3d off = eu.ChildOrigin(c, d);
        acc += off;      // 下行累积原点
        q = q - off;     // 上行残余
    }
    // 重建：acc + q ≈ p（加性细分；任意 double 输入允许 1 ulp 级残差）
    Vec3d rec = acc + q;
    CHECK_REL(rec.x, p.x, 1e-15);
    CHECK_REL(rec.y, p.y, 1e-15);
    CHECK_REL(rec.z, p.z, 1e-15);

    // 球面模型（度坐标，加性同样成立）
    SphericalSurface sp;
    Vec3d ps{31.2304, 121.4737, 12.0};  // 上海
    Vec3d qs = ps;
    Vec3d accs{0, 0, 0};
    for (int d = 0; d < 16; ++d) {
        uint32_t c = sp.CellOf(qs);
        Vec3d off = sp.ChildOrigin(c, d);
        accs += off;
        qs = qs - off;
    }
    Vec3d recs = accs + qs;
    CHECK_REL(recs.x, ps.x, 1e-12);
    CHECK_REL(recs.y, ps.y, 1e-12);
}

// ----------------------------------------------------------------------------
// Normalize 主值化（P4，规范 T-INV-3 的单元级依据）
// ----------------------------------------------------------------------------
void TestNormalize() {
    SphericalSurface sp;
    Vec3d a = sp.Normalize({0.0, 181.0, 0.0});
    CHECK_NEAR(a.y, -179.0, 1e-12);   // lon=181 → −179（T-INV-3）
    Vec3d b = sp.Normalize({0.0, -180.0, 0.0});
    CHECK_NEAR(b.y, 180.0, 1e-12);    // 主值区间 (−180, 180]
    Vec3d c = sp.Normalize({95.0, 0.0, 0.0});
    CHECK_NEAR(c.x, 90.0, 1e-12);     // lat 钳制
    Vec3d d = sp.Normalize({0.0, 540.0, 0.0});
    CHECK_NEAR(d.y, 180.0, 1e-9);

    Orbital ob;
    CHECK_NEAR(ob.Normalize({1.0, 1.25, 0.0}).y, 0.25, 1e-15);  // θ 回卷圈数
    CHECK_NEAR(ob.Normalize({1.0, -0.1, 0.0}).y, 0.9, 1e-15);
    CHECK(ob.Normalize({-5.0, 0.0, 0.0}).x == 0.0);             // r 钳制非负

    MovingFrame1D mv;  // 默认 length = 100 km
    CHECK(mv.Normalize({-5.0, 0.0, 0.0}).x == 0.0);        // s<0 → 0
    CHECK(mv.Normalize({150000.0, 0.0, 0.0}).x == mv.length);  // 越界钳制
}

// ----------------------------------------------------------------------------
// 度量基准（规范表 2）：haversine 与图卡转移映射
// ----------------------------------------------------------------------------
void TestMetrics() {
    SphericalSurface sp;  // R = 6.371e6
    // 赤道上经度 1° 的大圆距离 = π/180·R ≈ 111194.9 m
    double d2 = sp.DistanceSq({0, 0, 0}, {0, 1, 0});
    CHECK_REL(std::sqrt(d2), 111194.92664455873, 1e-6);
    // 反经线两侧（±180° 邻域）距离应趋于 0 而非绕远
    double dAnti = sp.DistanceSq({0, 179.999, 0}, {0, -179.999, 0});
    CHECK(std::sqrt(dAnti) < 250.0);  // ≈ 2×0.001°×111km ≈ 222 m
    // 图卡往返：ToParent ∘ FromParent ≈ 恒等
    Vec3d cart = sp.ToParent({31.2304, 121.4737, 12.0});
    Vec3d back = sp.FromParent(cart);
    CHECK_REL(back.x, 31.2304, 1e-12);
    CHECK_REL(back.y, 121.4737, 1e-12);
    CHECK_REL(back.z, 12.0, 1e-9);

    Orbital ob;
    // θ = 0 与 θ = 0.5 的两点距离 = 2r（轨道系笛卡尔）
    CHECK_REL(std::sqrt(ob.DistanceSq({100, 0, 0}, {100, 0.5, 0})), 200.0, 1e-12);
    Vec3d oc = ob.ToParent({100.0, 0.25, 5.0});
    CHECK_NEAR(oc.x, 0.0, 1e-10);
    CHECK_REL(oc.y, 100.0, 1e-12);
    Vec3d oBack = ob.FromParent(oc);
    CHECK_REL(oBack.x, 100.0, 1e-12);
    CHECK_REL(oBack.y, 0.25, 1e-12);
}

// ----------------------------------------------------------------------------
// NeighborCells：邻域展开计数与路径合法性（算法 S6 阶段一）
// extent=8、level=2 ⇒ cell=2；center=(0.1,0.1,0.1)、r=1.5 每轴覆盖 2 格
// ⇒ 共 8 个 cell，路径深度 = root.depth + 2
// ----------------------------------------------------------------------------
void TestNeighborCells() {
    Euclidean3D eu;
    eu.extent = 8.0;
    AnchorPath root;
    root.AppendInPlace(1);  // 场景挂在全局 [1] 下
    std::vector<AnchorPath> out;
    eu.NeighborCells({0.1, 0.1, 0.1}, 2, 1.5, root, out);
    CHECK(out.size() == 8);
    bool depthOk = true, rootOk = true;
    for (const auto& p : out) {
        if (p.depth != root.depth + 2) depthOk = false;
        if (!root.IsPrefixOf(p)) rootOk = false;
    }
    CHECK(depthOk && rootOk);

    // 跨父区边界：center 贴近 cell 边（x≈2⁻）时 x 方向覆盖 idx{2,3}；
    // y/z 方向 0.1±0.5 跨过 0 边界，同样各覆盖 2 格 ⇒ 共 8 个 cell
    out.clear();
    eu.NeighborCells({1.99, 0.1, 0.1}, 2, 0.5, root, out);
    CHECK(out.size() == 8);

    // 球面反经线邻域（T-F-球面 的单元级依据）：
    // level=4（lon span=22.5°），中心 lon=179.9°、r=2000km ⇒ 展开跨 ±180°
    SphericalSurface sp;
    AnchorPath sroot;
    sroot.AppendInPlace(2);
    out.clear();
    sp.NeighborCells({0.0, 179.9, 0.0}, 4, 2.0e6, sroot, out);
    bool wrapped = false;
    for (const auto& p : out) {
        // 末段 lon bit 反映东西半球；回卷后两侧 cell 都应出现
        uint32_t lastLon = p.LastSeg() & 1u;
        if (lastLon == 0u) wrapped = true;
    }
    CHECK(out.size() > 0 && wrapped);
}

// ----------------------------------------------------------------------------
// 测试入口
// ----------------------------------------------------------------------------
void RunAll() {
    TestAnchorPath();
    TestMath();
    TestKinematicP1();
    TestModelReconstruction();
    TestNormalize();
    TestMetrics();
    TestNeighborCells();
}

TEST_MAIN("unit")
