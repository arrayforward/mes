#pragma once

// ============================================================================
// mse/plc_filter.h —— 适配层:PLC/传感器三层过滤(迟滞 + 驻留 + 聚合网关)
//
// 依据《系统API设计》§七 与实现方案 §3.9 / P4:
//   高频机器信号(PLC 原始流)不是事件类型——先过适配层三层过滤,
//   迁移沿才有资格成为候选:迟滞状态机(去抖)→ 驻留(新态须稳定 N 拍)
//   → 聚合网关(同一迁移沿在窗口内去重)。高频噪声到不了端点:
//   噪声洪峰下事件率有界(验证矩阵红线)。
//   适配器契约:输入外部信号,输出变化描述,别无其他;不得绕过 POST /events。
//
// 时间用外部注入的逻辑 tick(适配器同规则一样不读物理时钟)。
// ============================================================================

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "mse/model.h"

namespace mse {

// ---- 单信号三层过滤器 ----
class PlcFilter {
public:
    struct Config {
        double on = 1.0;               // 迟滞上阈:越过进入高态
        double off = 0.0;              // 迟滞下阈:跌破回到低态(off < on)
        int    dwell_ticks = 3;        // 驻留:新态须连续稳定 N 拍才采信
        int    dedup_window_ticks = 0; // 聚合:同一迁移沿在窗口内去重(0=不去重)
    };

    PlcFilter(std::string signal_id, Config cfg);

    /// 喂一个原始采样。返回 true/false 表示产生了一个可信的迁移沿
    /// (true=进入高态,false=回到低态);std::nullopt = 噪声/驻留中/去重,不到端点。
    std::optional<bool> sample(double value, int64_t tick);

    const std::string& signal_id() const { return signal_id_; }
    bool state() const { return committed_; }  // 当前已采信状态

private:
    std::string signal_id_;
    Config      cfg_;
    bool        committed_ = false;    // 已采信(迟滞+驻留后的)状态
    bool        pending_ = false;      // 驻留中的候选状态
    int         dwell_ = 0;            // 候选状态已连续稳定拍数
    int64_t     last_emit_tick_ = -1;  // 最近一次迁移沿输出的 tick(聚合去重)
};

// ---- 聚合网关:信号迁移沿 → 变化描述候选(映射表驱动) ----
class AdapterGateway {
public:
    /// 映射:某信号的某个迁移沿 → 候选模板。值里 {边} 处替换为迁移沿结果。
    struct EdgeMapping {
        std::string type;                       // 事件类型
        std::string target_id;                  // 目标本体
        std::string key;                        // 写入的属性键
        json        high_value;                 // 进入高态写入的值
        json        low_value;                  // 回到低态写入的值
        std::string actor = "plc-gateway";
        std::string anchor;                     // 时空锚点(空 = 不带)
    };

    void map_edge(const std::string& signal_id, EdgeMapping m);

    /// 迁移沿 → 候选。未映射的信号返回 std::nullopt。
    std::optional<Candidate> translate(const std::string& signal_id, bool edge,
                                       int64_t tick) const;

private:
    std::map<std::string, EdgeMapping> mappings_;
};

} // namespace mse
