// ============================================================================
// 文件: functions.h
// 模块: stmb_vql(大模型 Function Calling 接口,依赖 service)
// 用途: 声明面向大模型的工具调用接口:
//         - toolSchemas():输出 OpenAI 风格 tools 数组(8 个工具,含
//           name/description/parameters JSON Schema),可直接粘贴给大模型 API;
//         - dispatch(name, args):参数校验(缺参/类型错/数组长度错给出
//           结构化错误),执行后返回 {ok:true, data:...};
//         - blockToJson / instanceToJson / versionToJson:块、动态实例、
//           历史版本的统一 JSON 序列化助手(VQL 引擎与 dispatch 共用)。
// 设计思路:
//   1. 一切返回都是 JsonValue,错误统一 {ok:false, error:{code, message}},
//      成功统一 {ok:true, data:...},大模型侧可直接按 ok 字段分支;
//   2. 序列化助手集中在此,保证 VQL 与工具调用输出结构一致。
// 架构角色: vql 模块面向大模型的出口,与 VQL 引擎并列。
// ============================================================================
#pragma once

#include "json.h"
#include "stmb_service.h"

#include <string>

namespace stmb {

// 统一序列化:记忆块 / 动态实例 / 历史版本 -> JsonValue
//   块字段:id、region[6]、payload、timestamp、state、confidence、level、
//          version、isSummary、suspect;
//   实例字段:id、class、position[3]、velocity[3]、state、confidence、
//            sources、轨迹点数;
//   版本字段:version、payload、confidence、state、validFrom、validTo
JsonValue blockToJson(const MemoryBlock& block);
JsonValue instanceToJson(const DynamicInstance& inst);
JsonValue versionToJson(const BlockVersion& version);

class FunctionRegistry {
public:
    // 构造:持有服务引用(工具调用直接作用于该服务)
    explicit FunctionRegistry(StmbService& service) : service_(service) {}

    // 输出 8 个工具的 OpenAI 风格 schema 数组
    JsonValue toolSchemas() const;

    // 派发一次工具调用:未知工具名 / 参数缺失 / 类型错误 / 数组长度错误
    // 均返回结构化错误;成功返回 {ok:true, data:...}
    JsonValue dispatch(const std::string& name, const JsonValue& args);

private:
    // 结构化错误便捷构造
    static JsonValue fail(const std::string& code, const std::string& message);
    // 参数提取与校验(失败时填 error 并返回 false)
    static bool readRegion(const JsonValue& args, AABB& out, JsonValue& error);
    static bool readTimeRange(const JsonValue& args, TimeRange& out, JsonValue& error);
    static bool readUint(const JsonValue& args, const std::string& key,
                         std::uint64_t& out, JsonValue& error);
    static bool readString(const JsonValue& args, const std::string& key,
                           std::string& out, JsonValue& error);
    static bool readNumber(const JsonValue& args, const std::string& key,
                           double& out, JsonValue& error);

    StmbService& service_;
};

}  // namespace stmb
