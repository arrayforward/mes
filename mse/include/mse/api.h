#pragma once

// ============================================================================
// mse/api.h —— API 门面:两个动词(进程内实现,HTTP 适配器挂在它上面)
//
// 依据《系统API设计》§一:
//   写 = 提交变化描述(POST /events);读 = 求值视图(GET /views/{viewId})。
//   没有 update/delete,没有版本号;没有第三种资源。
//   定义入口是系统内部方法调用(SDK),不走远程 API。
//
// POST /events 的扁平载荷 → Candidate 归一化规则:
//   系统键:type(必), id(必,字符串或字符串数组=多目标), actor(必),
//          space(字符串=锚点路径/模糊文本,或 {"anchor":..}|{"coord":[x,y,z]}|{"raw":..}),
//          time(ISO 发生时间,缺省=""), evidence, corrects(原事件 id),
//          idempotency_key
//   其余键 = 属性写入,应用到每个目标 id("一次发生、多处变化"时如需按目标
//   分别写,用 {"writes": {"id1": {...}, "id2": {...}}} 显式形态)。
//   space 字符串:在锚点表命中 → 锚点参照;否则 → 模糊文本原文照存。
// ============================================================================

#include <map>
#include <string>

#include "mse/model.h"

namespace mse {

class DefinitionLayer;
class ViewEngine;
class WritePipeline;

class ApiGateway {
public:
    ApiGateway(WritePipeline& pipeline, ViewEngine& views, DefinitionLayer& defs);

    /// 写动词:POST /events。入参扁平 k-v(json object),返回回执 JSON:
    /// {"status":"settled","event_id":n} 或
    /// {"status":"rejected","layer":k,"violations":[...]}。
    /// 载荷形状非法(非 object/缺 type/id) → {"status":"rejected","layer":-1,...}。
    json post_events(const json& payload);

    /// 读动词:GET /views/{viewId}?observer=&entity=&t=(t = AS OF 事件序号)。
    json get_view(const std::string& view_id,
                  const std::map<std::string, std::string>& query);

    /// 定义入口(内部方法调用,SDK 性质;HTTP 不暴露):
    /// 提交定义候选 → 校验 → 结算 → 四集合热更新。
    json post_definition(const std::string& def_type, const json& payload);

    /// 扁平载荷 → Candidate 归一化(公开静态,适配器/测试可单测)。
    /// 失败返回 std::nullopt 并填 error。
    static std::optional<Candidate> normalize(const json& payload, std::string& error);

private:
    WritePipeline&   pipeline_;
    ViewEngine&      views_;
    DefinitionLayer& defs_;
};

} // namespace mse
