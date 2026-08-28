// ============================================================================
// coordinate_model.hpp —— 坐标模型 CoordinateModel：场景的坐标策略注入点
// ============================================================================
//
// 【本文件的实现思路】
// 公理 A4（边界欧氏化）：场景内部可以是任意几何（球面、弧长、周期角），
// 但跨场景边界一律换算为父场景坐标。实现上对应微分几何的"图册—图卡"
// 构造（论文 §6.1）：每个场景是一张图卡，CoordinateModel 是图卡的坐标
// 策略，场景树是图册，Convert 即转移映射。
//
// 契约（规范 §3.3 + 论文表 3），每个模型提供六组运算：
//   1. DistanceSq     —— 场景度量（平方域，纪律 P7：Find 过滤与排序不开方）；
//   2. CellOf         —— 场景内划分：本层中心坐标 → 子区段号；
//   3. ChildOrigin    —— 段号 → 子区中心相对父中心的偏移（TransformAt 虚拟
//                        节点分支与 AnchorOf 下沉共用，保证二者严格一致）；
//   4. NeighborCells  —— 邻域展开：查询球覆盖的同层 cell 路径集合；
//   5. Normalize      —— 坐标规范化（P4：写入即强制，读取不隐式修正）；
//   6. ToParent/FromParent —— 图卡边界的转移映射：场景根原生坐标 ↔
//                        场景根笛卡尔坐标（欧氏模型为恒等）。
//
// 静态分派（论文 §6.3 工程纪律）：std::variant + std::visit，
// Euclidean 为快路径，每对象高频调用的 DistanceSq/CellOf 零虚函数开销。
//
// 内置模型目录（规范表 2）：
//   Euclidean3D      (x,y,z)     米制      Morton 量化（位运算）
//   SphericalSurface (lat,lon,alt) 度+米   经纬分带（每层 4 子区，2 bit 段码）
//   Orbital          (r,θ,h)     米+圈数   径向/角向分层（每层 4 子区）
//   MovingFrame1D    (s,lat,h)   米        里程分段（每层 2 子区，1 bit 段码）
//
// 统一的不变量：所有模型的细分都是"每层二分"，因此匿名节点的局部坐标
// 恒为加性偏移——这是 AnchorOf（算法 S1）与 ConvertPoint（算法 S3）可以
// 用同一套行走代码覆盖全部模型的原因。
//
// 已知简化（README 中声明）：
//   - MovingFrame1D 的 ToParent/FromParent 取恒等（真实线路需样条投影，
//     属外围模块）；
//   - 图卡雅可比（速度跨界换算用）用中心差分数值计算，极点邻域精度下降。
// ============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <variant>
#include <vector>

#include "geocore/anchor_path.hpp"
#include "geocore/math.hpp"

namespace geocore {

namespace detail {
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDeg2Rad = kPi / 180.0;
inline constexpr double kRad2Deg = 180.0 / kPi;
} // namespace detail

// ============================================================================
// Euclidean3D：三维欧氏场景（约九成场景，快路径）
// 划分：根节点覆盖以原点为中心、边长 extent 的立方体，每层每轴二分，
//       段码 = Morton 位组 (x<<2)|(y<<1)|z。
// ============================================================================
struct Euclidean3D {
    double extent = 1024.0;  // 根立方体边长（米）；2 的幂可保证每层尺寸精确

    // 图卡为恒等映射：跨界无畸变，烘焙与速度换算可直接复合
    static constexpr bool kIdentityChart = true;

    // 场景根中心（根节点覆盖 [-extent/2, extent/2]^3，中心即原点）
    Vec3d RootCenter() const { return {}; }

    // 第 d 层 cell 的边长（米）：extent·2^-d，2 的幂在 double 中精确
    double CellSizeMeters(int d) const { return extent * std::ldexp(1.0, -d); }

    // CellOf：本层中心坐标 → Morton 段号
    // 伪码：seg = (q.x≥0 ? 4:0) | (q.y≥0 ? 2:0) | (q.z≥0 ? 1:0)
    uint32_t CellOf(Vec3d q) const {
        return (q.x >= 0.0 ? 4u : 0u) | (q.y >= 0.0 ? 2u : 0u) | (q.z >= 0.0 ? 1u : 0u);
    }

    // ChildOrigin：段号 → 子区中心相对父区中心的偏移
    // 伪码：off = 父cell边长/4；各轴符号由段码对应 bit 决定
    Vec3d ChildOrigin(uint32_t seg, int parentDepth) const {
        double off = CellSizeMeters(parentDepth) * 0.25;
        return {(seg & 4u) ? off : -off,
                (seg & 2u) ? off : -off,
                (seg & 1u) ? off : -off};
    }

    // 场景度量（平方域）：Δx²+Δy²+Δz²
    double DistanceSq(Vec3d a, Vec3d b) const { return (a - b).NormSq(); }

    // 规范化：欧氏坐标无回卷，恒等
    Vec3d Normalize(Vec3d p) const { return p; }

    // 图卡转移映射：恒等（公理 A4 下欧氏场景跨界无换算）
    Vec3d ToParent(Vec3d p) const { return p; }
    Vec3d FromParent(Vec3d p) const { return p; }

    // ----------------------------------------------------------------------
    // NeighborCells：查询球覆盖的同层 cell 路径集合（Find 阶段一，算法 S6）
    // 伪码：
    //   cell = extent·2^-L；idx(p) = floor((p + extent/2) / cell)
    //   lo[i] = clamp(idx(c[i] − r))，hi[i] = clamp(idx(c[i] + r))
    //   for (ix,iy,iz) ∈ [lo,hi]³:
    //       path = rootPath；for j=1..L: path ⊕ 第 j 层的 Morton 位组
    // 关键：索引在场景根绝对坐标系计算，天然处理跨父区边界的邻居
    //       （邻居可以落在兄弟、堂兄弟乃至更远祖先之下），无需逐级进位。
    // ----------------------------------------------------------------------
    void NeighborCells(Vec3d centerRoot, int level, double radius,
                       const AnchorPath& rootPath,
                       std::vector<AnchorPath>& out) const {
        if (level > 30) level = 30;  // 位移保护：2^30 层远超实用深度
        double cell = CellSizeMeters(level);
        double half = extent * 0.5;
        long maxIdx = (1L << level) - 1;
        double c[3] = {centerRoot.x, centerRoot.y, centerRoot.z};
        long lo[3], hi[3];
        for (int i = 0; i < 3; ++i) {
            lo[i] = static_cast<long>(std::floor((c[i] - radius + half) / cell));
            hi[i] = static_cast<long>(std::floor((c[i] + radius + half) / cell));
            lo[i] = lo[i] < 0 ? 0 : (lo[i] > maxIdx ? maxIdx : lo[i]);
            hi[i] = hi[i] < 0 ? 0 : (hi[i] > maxIdx ? maxIdx : hi[i]);
        }
        for (long ix = lo[0]; ix <= hi[0]; ++ix)
            for (long iy = lo[1]; iy <= hi[1]; ++iy)
                for (long iz = lo[2]; iz <= hi[2]; ++iz) {
                    // 由绝对 cell 索引反推逐层段码：第 j 层取索引的第 (L-j) 位
                    AnchorPath p = rootPath;
                    for (int j = 1; j <= level; ++j) {
                        int sh = level - j;
                        uint32_t seg = static_cast<uint32_t>(
                            (((ix >> sh) & 1) << 2) | (((iy >> sh) & 1) << 1) |
                            ((iz >> sh) & 1));
                        p.AppendInPlace(seg);
                    }
                    out.push_back(p);
                }
    }
};

// ============================================================================
// SphericalSurface：球面场景（行星地表）
// 原生坐标：(lat, lon, alt) = (纬度°[-90,90], 经度°(-180,180], 高度 m)
// 度量：haversine 大圆距离平方 + 高度差平方（近似，规范表 2）
// 划分：经纬分带，每层纬度、经度各二分 → 4 子区，段码 (lat<<1)|lon
// ============================================================================
struct SphericalSurface {
    double radius = 6.371e6;  // 球半径 R（米），默认地球

    static constexpr bool kIdentityChart = false;  // 图卡非线性（度→米）

    // 根中心：lat=0, lon=0（根覆盖全球）
    Vec3d RootCenter() const { return {}; }

    // 第 d 层 cell 的角尺寸换算为米（取经度方向，赤道处最大，上界语义）
    double CellSizeMeters(int d) const {
        return (360.0 * std::ldexp(1.0, -d)) * detail::kDeg2Rad * radius;
    }

    // CellOf：q 为相对本层中心的 (Δlat°, Δlon°, Δalt)
    uint32_t CellOf(Vec3d q) const {
        return (q.x >= 0.0 ? 2u : 0u) | (q.y >= 0.0 ? 1u : 0u);
    }

    // ChildOrigin：第 d 层纬度跨度 180·2^-d 度、经度跨度 360·2^-d 度
    Vec3d ChildOrigin(uint32_t seg, int parentDepth) const {
        double latOff = 180.0 * std::ldexp(1.0, -parentDepth) * 0.25;
        double lonOff = 360.0 * std::ldexp(1.0, -parentDepth) * 0.25;
        return {(seg & 2u) ? latOff : -latOff,
                (seg & 1u) ? lonOff : -lonOff, 0.0};
    }

    // haversine 大圆距离平方（平方域延迟开方，P7）+ 高度差平方
    // 输入允许未回卷的瞬时值（换算中间态）：三角函数对任意角度正确
    double DistanceSq(Vec3d a, Vec3d b) const {
        double lat1 = a.x * detail::kDeg2Rad, lat2 = b.x * detail::kDeg2Rad;
        double dLat = (b.x - a.x) * detail::kDeg2Rad;
        double dLon = (b.y - a.y) * detail::kDeg2Rad;
        double s1 = std::sin(dLat * 0.5), s2 = std::sin(dLon * 0.5);
        double h = s1 * s1 + std::cos(lat1) * std::cos(lat2) * s2 * s2;
        h = h > 1.0 ? 1.0 : h;
        double d = 2.0 * radius * std::asin(std::sqrt(h));
        double da = b.z - a.z;
        return d * d + da * da;
    }

    // ----------------------------------------------------------------------
    // Normalize（P4：写入即规范化）
    // 伪码：lat ← clamp(lat, −90, 90)；lon ← 回卷至 (−180, 180]
    //   lon=181 → −179；lon=−180 → +180（主值区间右闭）
    // ----------------------------------------------------------------------
    Vec3d Normalize(Vec3d p) const {
        double lat = p.x < -90.0 ? -90.0 : (p.x > 90.0 ? 90.0 : p.x);
        double l = std::fmod(p.y - 180.0, 360.0);
        if (l <= 0.0) l += 360.0;
        return {lat, l - 180.0, p.z};
    }

    // ----------------------------------------------------------------------
    // 图卡转移映射（公理 A4：跨边界一律欧氏化）
    // ToParent：(lat°, lon°, alt) → 球心笛卡尔（米）：
    //   ρ = R + alt；p = ρ·(cosφ·cosλ, cosφ·sinλ, sinφ)，φ/λ 为弧度
    // FromParent：逆映射，lon 由 atan2 给出主值
    // ----------------------------------------------------------------------
    Vec3d ToParent(Vec3d p) const {
        double phi = p.x * detail::kDeg2Rad, lam = p.y * detail::kDeg2Rad;
        double rho = radius + p.z;
        double cp = std::cos(phi);
        return {rho * cp * std::cos(lam), rho * cp * std::sin(lam),
                rho * std::sin(phi)};
    }
    Vec3d FromParent(Vec3d p) const {
        double rho = p.Norm();
        double lat = std::asin(p.z / rho) * detail::kRad2Deg;
        double lon = std::atan2(p.y, p.x) * detail::kRad2Deg;
        return {lat, lon, rho - radius};
    }

    // 邻域展开：查询球（半径米）覆盖的经纬 cell 路径集合
    // 伪码：
    //   degLat = r/R·(180/π)；degLon = degLat / max(cos(lat), ε)（高纬放宽）
    //   ilat ∈ clamp(floor((lat+90)/spanLat) ± ⌈degLat/spanLat⌉)
    //   ilon ∈ floor((lon+180)/spanLon) ± ⌈degLon/spanLon⌉（对 2^L 取模回卷）
    // 反经线（lon=±180°）邻居由取模天然覆盖（T-F-球面 测试的依据）。
    void NeighborCells(Vec3d centerRoot, int level, double radiusMeters,
                       const AnchorPath& rootPath,
                       std::vector<AnchorPath>& out) const {
        if (level > 20) level = 20;  // 经纬格规模保护
        double spanLat = 180.0 * std::ldexp(1.0, -level);
        double spanLon = 360.0 * std::ldexp(1.0, -level);
        long n2 = 1L << level, maxIdx = n2 - 1;
        double degLat = radiusMeters / radius * detail::kRad2Deg;
        double cosLat = std::cos(centerRoot.x * detail::kDeg2Rad);
        double degLon = degLat / (cosLat > 0.02 ? cosLat : 0.02);
        if (degLon > 180.0) degLon = 180.0;
        long ilatC = static_cast<long>(std::floor((centerRoot.x + 90.0) / spanLat));
        long ilonC = static_cast<long>(std::floor((centerRoot.y + 180.0) / spanLon));
        long dLat = static_cast<long>(std::ceil(degLat / spanLat));
        long dLon = static_cast<long>(std::ceil(degLon / spanLon));
        if (dLat > 32) dLat = 32;   // 候选数上界：极端半径由调用方自查
        if (dLon > 32) dLon = 32;
        for (long ilat = ilatC - dLat; ilat <= ilatC + dLat; ++ilat) {
            if (ilat < 0 || ilat > maxIdx) continue;  // 纬度钳制（极点无回卷）
            for (long ilon = ilonC - dLon; ilon <= ilonC + dLon; ++ilon) {
                long w = ((ilon % n2) + n2) % n2;     // 经度回卷（反经线）
                AnchorPath p = rootPath;
                for (int j = 1; j <= level; ++j) {
                    int sh = level - j;
                    uint32_t seg = static_cast<uint32_t>(
                        (((ilat >> sh) & 1) << 1) | ((w >> sh) & 1));
                    p.AppendInPlace(seg);
                }
                out.push_back(p);
            }
        }
    }
};

// ============================================================================
// Orbital：轨道场景（行星系、卫星轨道）
// 原生坐标：(r, θ, h) = (径向 m, 角向圈数 [0,1), 离面高度 m)
// 度量：转轨道系笛卡尔后欧氏（规范表 2"轨道系内欧氏"）
// 划分：r ∈ [0, rMax] 与 θ ∈ [0,1) 每层各二分 → 4 子区，段码 (r<<1)|θ
// ============================================================================
struct Orbital {
    double rMax = 3.0e11;  // 径向覆盖上限（米），默认约 2 AU

    static constexpr bool kIdentityChart = false;

    // 根中心：r = rMax/2, θ = 0.5（根覆盖 [0,rMax]×[0,1)）
    Vec3d RootCenter() const { return {rMax * 0.5, 0.5, 0.0}; }

    double CellSizeMeters(int d) const { return rMax * std::ldexp(1.0, -d); }

    uint32_t CellOf(Vec3d q) const {
        return (q.x >= 0.0 ? 2u : 0u) | (q.y >= 0.0 ? 1u : 0u);
    }

    Vec3d ChildOrigin(uint32_t seg, int parentDepth) const {
        double rOff = rMax * std::ldexp(1.0, -parentDepth) * 0.25;
        double tOff = std::ldexp(1.0, -parentDepth) * 0.25;  // θ 跨度单位：圈
        return {(seg & 2u) ? rOff : -rOff, (seg & 1u) ? tOff : -tOff, 0.0};
    }

    // 轨道系笛卡尔转换（度量与图卡共用）
    // p = (r·cos 2πθ, r·sin 2πθ, h)
    Vec3d ToCartesian(Vec3d p) const {
        double ang = 2.0 * detail::kPi * p.y;
        return {p.x * std::cos(ang), p.x * std::sin(ang), p.z};
    }

    double DistanceSq(Vec3d a, Vec3d b) const {
        return (ToCartesian(a) - ToCartesian(b)).NormSq();
    }

    // Normalize（P4）：θ 回卷至圈数 [0,1)，r 钳制非负且不超 rMax
    Vec3d Normalize(Vec3d p) const {
        double t = std::fmod(p.y, 1.0);
        if (t < 0.0) t += 1.0;
        double r = p.x < 0.0 ? 0.0 : (p.x > rMax ? rMax : p.x);
        return {r, t, p.z};
    }

    // 图卡转移映射 = 轨道系笛卡尔化（及逆）
    Vec3d ToParent(Vec3d p) const { return ToCartesian(p); }
    Vec3d FromParent(Vec3d p) const {
        double r = std::hypot(p.x, p.y);
        double t = std::atan2(p.y, p.x) / (2.0 * detail::kPi);
        if (t < 0.0) t += 1.0;
        return {r, t, p.z};
    }

    // 邻域展开：径向直接换算，角向按 r·2π 弧长近似放宽并回卷
    void NeighborCells(Vec3d centerRoot, int level, double radiusMeters,
                       const AnchorPath& rootPath,
                       std::vector<AnchorPath>& out) const {
        if (level > 20) level = 20;
        double spanR = rMax * std::ldexp(1.0, -level);
        double spanT = std::ldexp(1.0, -level);
        long n2 = 1L << level, maxIdx = n2 - 1;
        double dTheta = radiusMeters / (centerRoot.x > 1.0 ? centerRoot.x : 1.0) /
                        (2.0 * detail::kPi);
        long irC = static_cast<long>(std::floor(centerRoot.x / spanR));
        long itC = static_cast<long>(std::floor(centerRoot.y / spanT));
        long dR = static_cast<long>(std::ceil(radiusMeters / spanR));
        long dT = static_cast<long>(std::ceil(dTheta / spanT));
        if (dR > 32) dR = 32;
        if (dT > 32) dT = 32;
        for (long ir = irC - dR; ir <= irC + dR; ++ir) {
            if (ir < 0 || ir > maxIdx) continue;
            for (long it = itC - dT; it <= itC + dT; ++it) {
                long w = ((it % n2) + n2) % n2;  // 角向回卷
                AnchorPath p = rootPath;
                for (int j = 1; j <= level; ++j) {
                    int sh = level - j;
                    uint32_t seg = static_cast<uint32_t>(
                        (((ir >> sh) & 1) << 1) | ((w >> sh) & 1));
                    p.AppendInPlace(seg);
                }
                out.push_back(p);
            }
        }
    }
};

// ============================================================================
// MovingFrame1D：一维运动坐标系（列车/飞机/电梯）
// 原生坐标：(s, lat, h) = (沿程 m ∈ [0,length], 横向 m, 高度 m)
// 度量：|Δs| 为主，横向/高度为辅（规范表 2）
// 划分：里程每层二分 → 2 子区，段码 1 bit
// ============================================================================
struct MovingFrame1D {
    double length = 100000.0;  // 线路长度（米）

    // 最小实现的简化声明：图卡取恒等——(s,lat,h) 直接作为父系局部坐标；
    // 真实线路的弧长→曲面投影属外围样条模块（README 已知边界）。
    static constexpr bool kIdentityChart = true;

    // 根中心：s = length/2（根覆盖 [0, length]）
    Vec3d RootCenter() const { return {length * 0.5, 0.0, 0.0}; }

    double CellSizeMeters(int d) const { return length * std::ldexp(1.0, -d); }

    uint32_t CellOf(Vec3d q) const { return q.x >= 0.0 ? 1u : 0u; }

    Vec3d ChildOrigin(uint32_t seg, int parentDepth) const {
        double off = CellSizeMeters(parentDepth) * 0.25;
        return {(seg & 1u) ? off : -off, 0.0, 0.0};
    }

    double DistanceSq(Vec3d a, Vec3d b) const { return (a - b).NormSq(); }

    // ----------------------------------------------------------------------
    // Normalize（P4）：s 越界钳制到 [0, length]
    // 规范允许"钳制或触发段间 Transfer"两种行为，本实现选钳制并声明；
    // 段间 Transfer 由上层按业务触发（T-F-列车 测试验证钳制语义）。
    // ----------------------------------------------------------------------
    Vec3d Normalize(Vec3d p) const {
        double s = p.x < 0.0 ? 0.0 : (p.x > length ? length : p.x);
        return {s, p.y, p.z};
    }

    Vec3d ToParent(Vec3d p) const { return p; }
    Vec3d FromParent(Vec3d p) const { return p; }

    // 邻域展开：一维里程区间覆盖
    void NeighborCells(Vec3d centerRoot, int level, double radiusMeters,
                       const AnchorPath& rootPath,
                       std::vector<AnchorPath>& out) const {
        if (level > 30) level = 30;
        double span = length * std::ldexp(1.0, -level);
        long maxIdx = (1L << level) - 1;
        long lo = static_cast<long>(std::floor((centerRoot.x - radiusMeters) / span));
        long hi = static_cast<long>(std::floor((centerRoot.x + radiusMeters) / span));
        lo = lo < 0 ? 0 : (lo > maxIdx ? maxIdx : lo);
        hi = hi < 0 ? 0 : (hi > maxIdx ? maxIdx : hi);
        for (long i = lo; i <= hi; ++i) {
            AnchorPath p = rootPath;
            for (int j = 1; j <= level; ++j) {
                int sh = level - j;
                p.AppendInPlace(static_cast<uint32_t>((i >> sh) & 1));
            }
            out.push_back(p);
        }
    }
};

// ============================================================================
// 静态分派：std::variant + 自由函数（规范 §3.3：Euclidean 快路径禁虚函数）
// ============================================================================
using CoordinateModel =
    std::variant<Euclidean3D, SphericalSurface, Orbital, MovingFrame1D>;

// 图卡是否为恒等映射：烘焙 rootTransform 的合法性条件（非恒等图卡非线性，
// 无法折叠进单个刚体 Transform，烘焙路径自动回退为行走，算法 S5 兜底）
inline bool ModelIdentityChart(const CoordinateModel& m) {
    return std::visit([](const auto& v) { return v.kIdentityChart; }, m);
}

inline Vec3d ModelRootCenter(const CoordinateModel& m) {
    return std::visit([](const auto& v) { return v.RootCenter(); }, m);
}

inline double ModelCellSizeMeters(const CoordinateModel& m, int d) {
    return std::visit([&](const auto& v) { return v.CellSizeMeters(d); }, m);
}

inline uint32_t ModelCellOf(const CoordinateModel& m, Vec3d q) {
    return std::visit([&](const auto& v) { return v.CellOf(q); }, m);
}

inline Vec3d ModelChildOrigin(const CoordinateModel& m, uint32_t seg, int pd) {
    return std::visit([&](const auto& v) { return v.ChildOrigin(seg, pd); }, m);
}

inline double ModelDistanceSq(const CoordinateModel& m, Vec3d a, Vec3d b) {
    return std::visit([&](const auto& v) { return v.DistanceSq(a, b); }, m);
}

inline Vec3d ModelNormalize(const CoordinateModel& m, Vec3d p) {
    return std::visit([&](const auto& v) { return v.Normalize(p); }, m);
}

inline Vec3d ModelToParent(const CoordinateModel& m, Vec3d p) {
    return std::visit([&](const auto& v) { return v.ToParent(p); }, m);
}

inline Vec3d ModelFromParent(const CoordinateModel& m, Vec3d p) {
    return std::visit([&](const auto& v) { return v.FromParent(p); }, m);
}

inline void ModelNeighborCells(const CoordinateModel& m, Vec3d centerRoot,
                               int level, double radius,
                               const AnchorPath& rootPath,
                               std::vector<AnchorPath>& out) {
    std::visit([&](const auto& v) {
        v.NeighborCells(centerRoot, level, radius, rootPath, out);
    }, m);
}

// ----------------------------------------------------------------------------
// 图卡雅可比（数值中心差分）：速度跨越场景边界时的线速度映射
// 用途：ConvertVelocity（算法 S4）经过图卡边界时 v_cart = J·v_native。
// 已知边界：中心差分在极点/奇异邻域精度下降，属有界近似（README 声明）。
// ----------------------------------------------------------------------------
template <typename F>
inline Vec3d NumericJacobianApply(F&& f, Vec3d p, Vec3d v, double eps) {
    Vec3d out;
    Vec3d f0 = f(p);
    for (int i = 0; i < 3; ++i) {
        Vec3d dp = p;
        if (i == 0) dp.x += eps;
        if (i == 1) dp.y += eps;
        if (i == 2) dp.z += eps;
        Vec3d col = (f(dp) - f0) / eps;  // 雅可比第 i 列
        double vi = (i == 0 ? v.x : (i == 1 ? v.y : v.z));
        out += col * vi;
    }
    return out;
}

// v_native（场景根原生坐标速度）→ v_cart（场景根笛卡尔速度）
inline Vec3d ModelChartVelToParent(const CoordinateModel& m, Vec3d p, Vec3d v) {
    if (ModelIdentityChart(m)) return v;
    return NumericJacobianApply([&](Vec3d q) { return ModelToParent(m, q); },
                                p, v, 1e-6);
}

// v_cart → v_native（逆方向）
inline Vec3d ModelChartVelFromParent(const CoordinateModel& m, Vec3d p, Vec3d v) {
    if (ModelIdentityChart(m)) return v;
    return NumericJacobianApply([&](Vec3d q) { return ModelFromParent(m, q); },
                                p, v, 1e-4);
}

} // namespace geocore
