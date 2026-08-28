// ============================================================================
// kinematic.hpp —— 运动学 Kinematic：变换作为时间的函数（规范 §3.2，论文 §7）
// ============================================================================
//
// 【本文件的实现思路】
// 公理 A1（拓扑静止）把"运动"严格限定为变换参数的演化：节点的树形位置
// 不变，只有其相对父系的变换随时间变化。Kinematic 即该变换的解析描述，
// 只挂在语义锚点上；匿名节点是 origin 规则生成、rotation = 单位四元数的
// Static 特例（公理 A3 由此保证路径码可计算）。
//
// 三种类型（规范 §3.2；PathConstrained 按 §9.1 检查表第 2 条"可后补"，
// 本实现给出线性时刻表的最小可用版本）：
//   - Static          ：origin / rotation 为常量；
//   - CircularOrbit   ：行星式圆轨道 + 可选自转；
//   - PathConstrained ：一维弧长约束（列车/飞机），s(t) = s0 + speed·t 的
//                        线性时刻表，沿父系 x 轴移动。
//
// 精度纪律（论文 §7.2，规范表 3）在本文件强制落实：
//   P1 圈数取模：相位以"圈数"存储，求值前 fmod(·,1.0)，sin/cos 的参数
//       恒在 [0,2π) —— 运行一万年与运行一秒的精度相同；
//   P2 存周期不存角速度：t/T 一次除法代替 ω·t 后取模，误差小一个数量级；
//   由此派生：OriginVelAt / AngularVelAt 都是解析导数（圆轨道有闭式），
//       Transfer 的速度切换（算法 S4，论文 §7.4）不需要差分近似。
//
// 时间一致性（P3）不在本文件而在调用方：所有 At(t) 的 t 必须来自帧参数，
// 接口刻意不提供无 t 的重载（规范 §9.3 错误预警第 2 条）。
// ============================================================================
#pragma once

#include <cmath>

#include "geocore/math.hpp"

namespace geocore {

// ----------------------------------------------------------------------------
// Kinematic：语义锚点的运动学描述
// ----------------------------------------------------------------------------
struct Kinematic {
    enum class Type : uint8_t { Static, CircularOrbit, PathConstrained };

    Type type = Type::Static;

    // ---- Static（匿名节点恒为此型特例：origin 规则生成，rotation 单位）----
    Vec3d      origin;
    Quaternion rotation;

    // ---- CircularOrbit（行星等）----
    double     orbitRadius = 0.0;   // 轨道半径（父系单位）
    double     period = 1.0;        // 公转周期（秒）——存周期不存角速度（P2）
    double     phase0 = 0.0;        // 初始相位（圈数 [0,1)）
    Quaternion orbitalPlane;        // 轨道面朝向（含自转轴朝向）
    double     spinPeriod = 0.0;    // 自转周期（秒）；0 = 不自转

    // ---- PathConstrained（列车/飞机等一维约束，最小实现）----
    double s0 = 0.0;                // 初始里程（父系单位）
    double speed = 0.0;             // 沿程速度（父系单位/秒），沿 x 轴正方向

    // 便捷构造：静态锚点
    static Kinematic Static(Vec3d o, Quaternion r = Quaternion::Identity()) {
        Kinematic k;
        k.type = Type::Static;
        k.origin = o;
        k.rotation = r;
        return k;
    }

    // 便捷构造：圆轨道锚点（行星）
    static Kinematic Orbit(double radius, double periodSec, double phaseTurns,
                           Quaternion plane = Quaternion::Identity(),
                           double spinPeriodSec = 0.0) {
        Kinematic k;
        k.type = Type::CircularOrbit;
        k.orbitRadius = radius;
        k.period = periodSec;
        k.phase0 = phaseTurns;
        k.orbitalPlane = plane;
        k.spinPeriod = spinPeriodSec;
        return k;
    }

    // 便捷构造：一维路径约束锚点（列车）
    static Kinematic Path(double sInit, double spd) {
        Kinematic k;
        k.type = Type::PathConstrained;
        k.s0 = sInit;
        k.speed = spd;
        return k;
    }

    // 是否为时间无关（烘焙 rootTransform 的合法性条件之一，论文 §7.3）
    bool IsStatic() const {
        return type == Type::Static ||
               (type == Type::CircularOrbit && orbitRadius == 0.0 && spinPeriod == 0.0) ||
               (type == Type::PathConstrained && speed == 0.0);
    }

    // ------------------------------------------------------------------
    // OriginAt：本锚点原点在父系中的位置
    // 伪码（规范 §3.2，CircularOrbit 分支）：
    //   turns = fmod(phase0 + t / period, 1.0)      // P1：圈数取模
    //   if (turns < 0) turns += 1.0
    //   (s, c) = sincos(2π · turns)                  // 参数恒在 [0,2π)
    //   return orbitalPlane · (r·c, r·s, 0)
    // ------------------------------------------------------------------
    Vec3d OriginAt(double t) const {
        switch (type) {
        case Type::Static:
            return origin;
        case Type::CircularOrbit: {
            double turns = std::fmod(phase0 + t / period, 1.0);
            if (turns < 0.0) turns += 1.0;
            double ang = 2.0 * kPi * turns;
            Vec3d p{orbitRadius * std::cos(ang), orbitRadius * std::sin(ang), 0.0};
            return orbitalPlane * p;
        }
        case Type::PathConstrained:
            return {s0 + speed * t, 0.0, 0.0};
        }
        return {};
    }

    // ------------------------------------------------------------------
    // RotationAt：本锚点坐标系相对父系的朝向
    // 自转：绕本系 z 轴（经 orbitalPlane 定向）旋转，相位同样圈数取模（P1）：
    //   R(t) = orbitalPlane ∘ Rz(2π · frac(t / spinPeriod))
    // ------------------------------------------------------------------
    Quaternion RotationAt(double t) const {
        switch (type) {
        case Type::Static:
            return rotation;
        case Type::CircularOrbit: {
            if (spinPeriod <= 0.0) return orbitalPlane;
            double turns = std::fmod(t / spinPeriod, 1.0);
            if (turns < 0.0) turns += 1.0;
            Quaternion spin =
                Quaternion::FromAxisAngle({0.0, 0.0, 1.0}, 2.0 * kPi * turns);
            return (orbitalPlane * spin).Normalized();
        }
        case Type::PathConstrained:
            return rotation;  // 一维约束不改变朝向（最小实现）
        }
        return Quaternion::Identity();
    }

    // ------------------------------------------------------------------
    // OriginVelAt：原点速度的解析导数（算法 S4 速度切换的 ȯ(t) 项）
    // 圆轨道闭式：d/dt [R·(r cosθ, r sinθ, 0)]，θ = 2π·frac(φ0 + t/T)
    //   ȯ = orbitalPlane · (−r·sinθ·2π/T, r·cosθ·2π/T, 0)
    // ------------------------------------------------------------------
    Vec3d OriginVelAt(double t) const {
        switch (type) {
        case Type::Static:
            return {};
        case Type::CircularOrbit: {
            if (orbitRadius == 0.0) return {};
            double turns = std::fmod(phase0 + t / period, 1.0);
            if (turns < 0.0) turns += 1.0;
            double ang = 2.0 * kPi * turns;
            double w = 2.0 * kPi / period;  // 角速度仅在求导点出现，不存储
            Vec3d v{-orbitRadius * std::sin(ang) * w,
                     orbitRadius * std::cos(ang) * w, 0.0};
            return orbitalPlane * v;
        }
        case Type::PathConstrained:
            return {speed, 0.0, 0.0};
        }
        return {};
    }

    // ------------------------------------------------------------------
    // AngularVelAt：角速度向量 ω（父系表达），算法 S4 的牵连速度项 ω×(R·p)
    // 目前仅自转贡献：ω = orbitalPlane · (0, 0, 2π/spinPeriod)
    // ------------------------------------------------------------------
    Vec3d AngularVelAt(double /*t*/) const {
        if (type == Type::CircularOrbit && spinPeriod > 0.0) {
            return orbitalPlane * Vec3d{0.0, 0.0, 2.0 * kPi / spinPeriod};
        }
        return {};
    }
};

} // namespace geocore
