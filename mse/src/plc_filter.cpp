// ============================================================================
// mse/plc_filter.cpp —— 适配层实现:PLC/传感器三层过滤(迟滞 + 驻留 + 聚合网关)
//
// 单信号过滤器(PlcFilter)与聚合网关(AdapterGateway)均为纯逻辑组件:
//   不读物理时钟(tick 由外部注入)、不读随机数——同一条采样流喂两遍,
//   输出逐比特一致(与核心链路同一确定性纪律)。
// ============================================================================

#include "mse/plc_filter.h"

#include <utility>

namespace mse {

// ---- PlcFilter:迟滞状态机 → 驻留 → 聚合去重 ----

PlcFilter::PlcFilter(std::string signal_id, Config cfg)
    : signal_id_(std::move(signal_id)), cfg_(cfg) {}

std::optional<bool> PlcFilter::sample(double value, int64_t tick) {
    // [1] 迟滞:越上阈想要高态、破下阈想要低态,中间带维持原判(不动)。
    //     on > off,两阈不可能同时满足。
    const bool want_high = value >= cfg_.on;
    const bool want_low  = value <= cfg_.off;
    if (!want_high && !want_low) return std::nullopt;  // 中间带:迟滞不动
    const bool target = want_high;

    // [2] 驻留:目标态与已采信态不同才进入/累计驻留计数;值回落(目标回到
    //     已采信态)则清零重计。连续稳定 dwell_ticks 拍才采信翻转。
    if (target == committed_) {
        dwell_ = 0;  // 值回落:驻留计数清零
        return std::nullopt;
    }
    if (target != pending_) {
        pending_ = target;
        dwell_ = 0;
    }
    if (++dwell_ < cfg_.dwell_ticks) return std::nullopt;  // 驻留未满,不采信

    committed_ = target;  // 采信翻转
    dwell_ = 0;

    // [3] 聚合去重:窗口内的重复迁移沿吞掉(状态已采信,只是不再到端点)
    if (cfg_.dedup_window_ticks > 0 && last_emit_tick_ >= 0 &&
        tick - last_emit_tick_ < cfg_.dedup_window_ticks) {
        return std::nullopt;
    }
    last_emit_tick_ = tick;
    return committed_;
}

// ---- AdapterGateway:映射表驱动,迁移沿 → 变化描述候选 ----

void AdapterGateway::map_edge(const std::string& signal_id, EdgeMapping m) {
    mappings_[signal_id] = std::move(m);
}

std::optional<Candidate> AdapterGateway::translate(const std::string& signal_id,
                                                   bool edge, int64_t tick) const {
    const auto it = mappings_.find(signal_id);
    if (it == mappings_.end()) return std::nullopt;
    const EdgeMapping& m = it->second;
    Candidate c;
    c.type = m.type;
    c.writes[m.target_id] = {{m.key, edge ? m.high_value : m.low_value}};
    c.actor = m.actor;
    if (!m.anchor.empty()) c.space = SpaceRef::anchor_ref(m.anchor);
    c.occur_time = "tick:" + std::to_string(tick);  // 逻辑 tick 文本(不读物理时钟)
    return c;
}

} // namespace mse
