// ============================================================================
// 文件: unistore_source_bridge.cpp
// 模块: stmb_bridge_unistore(仅 STMB_WITH_UNISTORE=ON 时构建)
// 用途: 实现 UnistoreSourceBridge 的水合与变更同步。
// ============================================================================
#include "unistore_source_bridge.h"

#include <ctime>
#include <string>

namespace stmb {

// 伪代码:
//   1. 水合:list_sources 全表,十进制可解析的 id 经 registerSource 载入
//      (此时钩子尚未安装,不会回写;registerSource 会追加 WAL,幂等无害);
//      非数值 id 跳过(stmb SourceId 为 u64,无法映射);
//   2. 安装变更钩子:registerSource(id, w) 成功后 upsert 回 et_sources——
//      保留表中已有 meta,updated_at 取当前时间;
//   3. 冲突策略:以表为准——水合在挂钩之前完成,表中已有的 id 以表值
//      覆盖 stmb 内存值。
UnistoreSourceBridge::UnistoreSourceBridge(StmbService& service,
                                           entitytree::EntityStore& store)
    : service_(service), store_(store) {
    // 1. 启动水合:et_sources -> stmb 注册表(以表为准)
    for (const auto& rec : store_.list_sources()) {
        try {
            const SourceId id = std::stoull(rec.source_id);
            service_.registerSource(id, rec.reliability);
        } catch (const std::exception&) {
            // 非数值 source_id(如 "cam1")无法映射到 stmb SourceId,跳过
        }
    }
    // 2. 变更同步:stmb -> et_sources
    service_.setSourceChangeHook([this](SourceId id, double reliability) {
        entitytree::SourceRecord rec;
        const std::string key = std::to_string(id);
        if (auto existing = store_.get_source(key)) {
            rec = *existing;  // 保留 meta 等既有字段
        }
        rec.source_id = key;
        rec.reliability = reliability;
        rec.updated_at = static_cast<std::int64_t>(std::time(nullptr));
        store_.upsert_source(rec);
    });
}

UnistoreSourceBridge::~UnistoreSourceBridge() {
    service_.setSourceChangeHook({});
}

}  // namespace stmb
