// ============================================================================
// 文件: unistore_source_bridge.h
// 模块: stmb_bridge_unistore(仅 STMB_WITH_UNISTORE=ON 时构建)
// 用途: 声明 UnistoreSourceBridge —— stmb SourceRegistry 与 entitytree
//       et_sources 表(D:\manufacture\storage 第 7 张表)的双向共享桥。
// 设计思路:
//   1. 启动水合:把 et_sources 中的来源载入 stmb 注册表(以表为准——
//      entity 组件反馈闭环 merge/split 写入的值是最新证据);
//   2. 变更同步:利用 StmbService 的来源变更钩子,registerSource 成功后
//      upsert 回 et_sources(保留表中已有 meta,updated_at 取当前时间);
//   3. source_id 映射:stmb SourceId(u64) <-> et_sources 十进制字符串;
//      表中的非数值 id(如 "cam1")无法映射,水合时跳过(该来源在 stmb 侧
//      语义不变:未知来源默认 0.5);
//   4. voxel 默认构建保持零第三方依赖,本桥只在显式开启时编译。
// 架构角色: stmb_service 与 unistore(entitytree)之间的可选适配层。
// ============================================================================
#pragma once

#include "entitytree/entity_store.h"
#include "stmb_service.h"

namespace stmb {

class UnistoreSourceBridge {
public:
    // 构造即完成水合(表 -> stmb)并安装变更钩子(stmb -> 表);
    // service 与 store 须在整个桥生命周期内存活(典型:指向同一个
    // sqlite 库文件的 entitytree::SqlEntityStore)。
    UnistoreSourceBridge(StmbService& service, entitytree::EntityStore& store);
    // 析构时摘除钩子,避免悬挂回调
    ~UnistoreSourceBridge();

    UnistoreSourceBridge(const UnistoreSourceBridge&) = delete;
    UnistoreSourceBridge& operator=(const UnistoreSourceBridge&) = delete;

private:
    StmbService&             service_;
    entitytree::EntityStore& store_;
};

}  // namespace stmb
