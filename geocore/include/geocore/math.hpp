// ============================================================================
// math.hpp —— 基础数学类型：Vec3d / Quaternion / Transform / BoundingSphere
// ============================================================================
//
// 【本文件的实现思路】
// 内核中所有跨层换算最终都归结为"旋转 + 平移"的刚体变换复合（论文第 8 节）。
// 本文件提供三个不可变值类型，是整个库的最底层地基（规范 §9.1 检查表第 1 条
// 之前的隐含第 0 条）：
//
//   1. Vec3d      —— 三维双精度向量。内核全程使用 double（精度纪律 P8 要求
//                     合成链以 double 连乘）；float32 降格（P6）属可选优化，
//                     本参考实现不启用。
//   2. Quaternion —— 单位四元数，表示节点坐标系相对父系的朝向。选择四元数而
//                     非矩阵：复合与求逆的舍入更小、存储更省，且 LCA 行走
//                     （算法 S3）每层只需要一次四元数乘向量。
//   3. Transform  —— {origin, rotation} 二元组，语义为"子系坐标 → 父系坐标"：
//                       p_parent = rotation * p_child + origin
//                     提供复合（Compose）与求逆（Inverse），是烘焙
//                     rootTransform（规范 §3.4，纪律 P8）与速度参考系项
//                     （论文 §7.4）的基本构件。
//
// 约定：
//   - 四元数恒为单位四元数（构造与复合后归一化），求逆即共轭；
//   - Transform 不含缩放（内核中跨边界一律刚体化，见公理 A4），
//     因此复合/求逆都有闭式；
//   - 全部函数为 constexpr/noexcept 友好的纯函数，无线程安全问题。
// ============================================================================
#pragma once

#include <cmath>

namespace geocore {

// 圆周率：M_PI 依赖运行库的非标准扩展，在严格 ANSI 模式
// （CMAKE_CXX_EXTENSIONS=OFF → -std=c++17）下 MinGW/MSVC 不定义，
// 统一使用本常量（double 精度足够覆盖规范表 3 的全部纪律）。
inline constexpr double kPi = 3.14159265358979323846264338327950288;

// ----------------------------------------------------------------------------
// Vec3d：三维双精度向量
// 用途：位置、速度、偏移量的统一载体。不同场景的原生坐标（欧氏米制、球面
//       经纬度度数、轨道 (r,θ,h)、列车 (s,lat,h)）都复用此类型，语义由
//       所属场景的 CoordinateModel 解释（规范 §3.3）。
// ----------------------------------------------------------------------------
struct Vec3d {
    double x = 0.0, y = 0.0, z = 0.0;

    Vec3d() = default;
    Vec3d(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    Vec3d operator+(const Vec3d& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3d operator-(const Vec3d& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3d operator-() const { return {-x, -y, -z}; }
    Vec3d operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3d operator/(double s) const { return {x / s, y / s, z / s}; }
    Vec3d& operator+=(const Vec3d& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3d& operator-=(const Vec3d& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }

    // 点积：用于投影与雅可比列向量构造
    double Dot(const Vec3d& o) const { return x * o.x + y * o.y + z * o.z; }

    // 叉积：速度换算的参考系牵连项 ω×(R·p)（论文 §7.4 公式）必需
    Vec3d Cross(const Vec3d& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }

    double NormSq() const { return Dot(*this); }   // 平方模长：平方域比较（P7）
    double Norm() const { return std::sqrt(NormSq()); }
};

// ----------------------------------------------------------------------------
// Quaternion：单位四元数（w 为标量部）
// 用途：节点朝向与旋转复合。旋转向量采用 q·v·q* 的优化形式（两次叉积）。
// ----------------------------------------------------------------------------
struct Quaternion {
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0;  // 默认单位四元数（无旋转）

    Quaternion() = default;
    Quaternion(double w_, double x_, double y_, double z_) : w(w_), x(x_), y(y_), z(z_) {}

    static Quaternion Identity() { return {}; }

    // 轴角构造：绕单位轴 axis 旋转 angle 弧度。
    // 用途：自转（spin）等运动学旋转的解析表达（规范 §3.2）。
    static Quaternion FromAxisAngle(const Vec3d& axis, double angle) {
        double h = 0.5 * angle;
        double s = std::sin(h);
        return {std::cos(h), axis.x * s, axis.y * s, axis.z * s};
    }

    // 哈密顿乘积：this ∘ o（先施加 o，再施加 this）
    Quaternion operator*(const Quaternion& o) const {
        return {
            w * o.w - x * o.x - y * o.y - z * o.z,
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w
        };
    }

    // 共轭 = 单位四元数的逆：下行换算 R⁻¹（算法 S3 下行段）使用
    Quaternion Conjugate() const { return {w, -x, -y, -z}; }

    Quaternion Normalized() const {
        double n = std::sqrt(w * w + x * x + y * y + z * z);
        return {w / n, x / n, y / n, z / n};
    }

    // 旋转向量：q·v·q* 的优化展开 v' = v + 2·qvec × (qvec × v + w·v)
    Vec3d Rotate(const Vec3d& v) const {
        Vec3d qv{x, y, z};
        Vec3d t = qv.Cross(v) + v * w;   // t = qvec × v + w·v
        return v + qv.Cross(t) * 2.0;
    }

    // operator* 直接作用于向量，贴合伪码记号 R·p
    Vec3d operator*(const Vec3d& v) const { return Rotate(v); }
};

// ----------------------------------------------------------------------------
// Transform：刚体变换 {origin, rotation}，语义 p_parent = R·p_child + origin
// 用途：
//   - TransformAt（算法 S2）的返回类型；
//   - 场景 rootTransform 烘焙（P8）与跨场景远亲换算（算法 S5）；
//   - FrameCache 中"物化节点 → 场景根"的累积变换（规范 §3.6）。
// ----------------------------------------------------------------------------
struct Transform {
    Vec3d      origin;     // 本系原点在父系中的位置
    Quaternion rotation;   // 本系坐标轴相对父系的朝向

    static Transform Identity() { return {}; }

    // 作用于点：p' = R·p + o
    Vec3d Apply(const Vec3d& p) const { return rotation * p + origin; }
    Vec3d operator*(const Vec3d& p) const { return Apply(p); }

    // 作用于方向向量：仅旋转，无平移（ConvertDirection 的基本操作）
    Vec3d ApplyDir(const Vec3d& v) const { return rotation * v; }

    // 逆变换：p_child = R⁻¹·(p_parent − o)。单位四元数求逆即共轭。
    Transform Inverse() const {
        Quaternion rc = rotation.Conjugate();
        return {rc * (-origin), rc};
    }

    // 复合：this ∘ inner，即先施加 inner 再施加 this。
    // 伪码： Compose(A, B): origin = A.R·B.o + A.o; rotation = A.R·B.R
    // 用途：烘焙 rootTransform 时沿链 double 连乘（P8）。
    static Transform Compose(const Transform& outer, const Transform& inner) {
        return {outer.rotation * inner.origin + outer.origin,
                (outer.rotation * inner.rotation).Normalized()};
    }
};

// ----------------------------------------------------------------------------
// BoundingSphere：场景粗判包围球（规范 §3.4）
// 运动场景注册时取扫掠包络（如圆轨道 = 轨道环面包围球），只算一次（P3 讨论）。
// ----------------------------------------------------------------------------
struct BoundingSphere {
    Vec3d  center;
    double radius = 0.0;

    // 与查询球求交：SoA 线性扫的单个测试（千级场景不建索引，规范 §3.4）
    bool Intersects(const Vec3d& c, double r) const {
        double rr = radius + r;
        return (center - c).NormSq() <= rr * rr;
    }
};

} // namespace geocore
