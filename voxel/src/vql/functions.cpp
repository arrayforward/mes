// ============================================================================
// 文件: functions.cpp
// 模块: stmb_vql(大模型 Function Calling 接口,依赖 service)
// 用途: 实现 8 个工具的 schema 输出与 dispatch 派发。
// 设计思路:
//   1. toolSchemas 用 JsonValue 直接拼装,结构即 OpenAI tools 数组,
//      parameters 为 JSON Schema(type/properties/required);
//   2. dispatch 先做通用校验(args 必须是 object),再按工具名分派,
//      每个分支用统一的 read* 助手做参数提取与校验,错误即返回结构化
//      {ok:false, error:{code, message}};
//   3. 返回数据复用 blockToJson / instanceToJson / versionToJson,
//      与 VQL 引擎输出保持一致。
// 架构角色: FunctionRegistry 的唯一实现文件。
// ============================================================================
#include "functions.h"

#include <algorithm>
#include <initializer_list>

namespace stmb {

// 伪代码(块 -> JSON):
//   1. 组装 object:id/region(6 元数组)/payload/timestamp/state(字符串)/
//      confidence/level/version/isSummary/suspect;
//   2. 返回该 object。
JsonValue blockToJson(const MemoryBlock& block) {
    JsonValue obj = JsonValue::makeObject();
    obj.set("id", JsonValue::makeNumber(static_cast<double>(block.id)));
    JsonValue region = JsonValue::makeArray();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        region.push(JsonValue::makeNumber(block.region.min[axis]));
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        region.push(JsonValue::makeNumber(block.region.max[axis]));
    }
    obj.set("region", region);
    obj.set("payload", JsonValue::makeString(block.payload));
    obj.set("timestamp", JsonValue::makeNumber(static_cast<double>(block.timestamp)));
    obj.set("state", JsonValue::makeString(toString(block.state)));
    obj.set("confidence", JsonValue::makeNumber(block.confidence));
    obj.set("level", JsonValue::makeNumber(block.level));
    obj.set("version", JsonValue::makeNumber(block.version));
    obj.set("is_summary", JsonValue::makeBool(block.isSummary));
    obj.set("suspect", JsonValue::makeBool(block.suspect));
    return obj;
}

// 伪代码(实例 -> JSON):
//   1. 组装 object:id/class/position(3 元数组)/velocity(3 元数组)/state/
//      confidence/sources(数组)/trajectory_points;
//   2. 返回该 object。
JsonValue instanceToJson(const DynamicInstance& inst) {
    JsonValue obj = JsonValue::makeObject();
    obj.set("id", JsonValue::makeNumber(static_cast<double>(inst.id)));
    obj.set("class", JsonValue::makeString(inst.classLabel));
    JsonValue pos = JsonValue::makeArray();
    JsonValue vel = JsonValue::makeArray();
    for (std::size_t axis = 0; axis < 3; ++axis) {
        pos.push(JsonValue::makeNumber(inst.latest.position[axis]));
        vel.push(JsonValue::makeNumber(inst.latest.velocity[axis]));
    }
    obj.set("position", pos);
    obj.set("velocity", vel);
    obj.set("state", JsonValue::makeString(toString(inst.state)));
    obj.set("confidence", JsonValue::makeNumber(inst.confidence));
    JsonValue sources = JsonValue::makeArray();
    for (SourceId src : inst.sources) {
        sources.push(JsonValue::makeNumber(static_cast<double>(src)));
    }
    obj.set("sources", sources);
    obj.set("trajectory_points",
            JsonValue::makeNumber(static_cast<double>(inst.trajectory.size())));
    return obj;
}

// 伪代码(版本 -> JSON):
//   1. 组装 object:version/payload/confidence/state/validFrom/validTo
//     (无 validTo 用 null);
//   2. 返回该 object。
JsonValue versionToJson(const BlockVersion& version) {
    JsonValue obj = JsonValue::makeObject();
    obj.set("version", JsonValue::makeNumber(version.version));
    obj.set("payload", JsonValue::makeString(version.payload));
    obj.set("confidence", JsonValue::makeNumber(version.confidence));
    obj.set("state", JsonValue::makeString(toString(version.state)));
    obj.set("valid_from",
            JsonValue::makeNumber(static_cast<double>(version.validFrom)));
    if (version.validTo.has_value()) {
        obj.set("valid_to",
                JsonValue::makeNumber(static_cast<double>(*version.validTo)));
    } else {
        obj.set("valid_to", JsonValue::makeNull());
    }
    return obj;
}

// 伪代码:
//   1. 组装 {ok:false, error:{code, message}} 并返回。
JsonValue FunctionRegistry::fail(const std::string& code, const std::string& message) {
    JsonValue root = JsonValue::makeObject();
    root.set("ok", JsonValue::makeBool(false));
    JsonValue err = JsonValue::makeObject();
    err.set("code", JsonValue::makeString(code));
    err.set("message", JsonValue::makeString(message));
    root.set("error", err);
    return root;
}

// 伪代码:
//   1. 要求 args.region 为 6 元数字数组;否则写结构化错误返回 false;
//   2. 读出 min/max 三元组填入 out。
bool FunctionRegistry::readRegion(const JsonValue& args, AABB& out, JsonValue& error) {
    if (!args.has("region") || !args.at("region").isArray() ||
        args.at("region").asArray().size() != 6) {
        error = fail("invalid_args", "region 必须是 6 元数字数组 [x1,y1,z1,x2,y2,z2]");
        return false;
    }
    for (const JsonValue& v : args.at("region").asArray()) {
        if (!v.isNumber()) {
            error = fail("invalid_args", "region 元素必须是数字");
            return false;
        }
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        out.min[axis] = args.at("region").asArray()[axis].asNumber();
        out.max[axis] = args.at("region").asArray()[axis + 3].asNumber();
    }
    return true;
}

// 伪代码:
//   1. 要求 args.time_range 为 2 元数字数组;否则写错误返回 false;
//   2. 读出 start/end 填入 out。
bool FunctionRegistry::readTimeRange(const JsonValue& args, TimeRange& out,
                                     JsonValue& error) {
    if (!args.has("time_range") || !args.at("time_range").isArray() ||
        args.at("time_range").asArray().size() != 2) {
        error = fail("invalid_args", "time_range 必须是 2 元数字数组 [t1,t2]");
        return false;
    }
    if (!args.at("time_range").asArray()[0].isNumber() ||
        !args.at("time_range").asArray()[1].isNumber()) {
        error = fail("invalid_args", "time_range 元素必须是数字");
        return false;
    }
    out.start = static_cast<TimeStamp>(args.at("time_range").asArray()[0].asNumber());
    out.end = static_cast<TimeStamp>(args.at("time_range").asArray()[1].asNumber());
    return true;
}

// 伪代码:
//   1. 要求指定键存在且为数字;否则写「缺参/类型错」错误返回 false。
bool FunctionRegistry::readUint(const JsonValue& args, const std::string& key,
                                std::uint64_t& out, JsonValue& error) {
    if (!args.has(key)) {
        error = fail("missing_param", "缺少必需参数: " + key);
        return false;
    }
    if (!args.at(key).isNumber()) {
        error = fail("invalid_args", "参数 " + key + " 必须是数字");
        return false;
    }
    out = static_cast<std::uint64_t>(args.at(key).asNumber());
    return true;
}

// 伪代码:
//   1. 要求指定键存在且为字符串;否则写错误返回 false。
bool FunctionRegistry::readString(const JsonValue& args, const std::string& key,
                                  std::string& out, JsonValue& error) {
    if (!args.has(key)) {
        error = fail("missing_param", "缺少必需参数: " + key);
        return false;
    }
    if (!args.at(key).isString()) {
        error = fail("invalid_args", "参数 " + key + " 必须是字符串");
        return false;
    }
    out = args.at(key).asString();
    return true;
}

// 伪代码:
//   1. 要求指定键存在且为数字;否则写错误返回 false。
bool FunctionRegistry::readNumber(const JsonValue& args, const std::string& key,
                                  double& out, JsonValue& error) {
    if (!args.has(key)) {
        error = fail("missing_param", "缺少必需参数: " + key);
        return false;
    }
    if (!args.at(key).isNumber()) {
        error = fail("invalid_args", "参数 " + key + " 必须是数字");
        return false;
    }
    out = args.at(key).asNumber();
    return true;
}

// 伪代码:
//   1. 定义一个局部 lambda 把 {name, description, properties, required}
//      包成 {type:"function", function:{...}} 并 push 到 tools;
//   2. 依次登记 8 个工具:stmb_query / stmb_query_at / stmb_history /
//      stmb_put / stmb_observe / stmb_dynamic_query / stmb_stats /
//      stmb_checkpoint,parameters 用 JSON Schema 描述;
//   3. 返回 tools 数组。
JsonValue FunctionRegistry::toolSchemas() const {
    JsonValue tools = JsonValue::makeArray();
    auto addTool = [&tools](const std::string& name, const std::string& desc,
                            JsonValue properties, JsonValue required) {
        JsonValue params = JsonValue::makeObject();
        params.set("type", JsonValue::makeString("object"));
        params.set("properties", std::move(properties));
        params.set("required", std::move(required));
        JsonValue fn = JsonValue::makeObject();
        fn.set("name", JsonValue::makeString(name));
        fn.set("description", JsonValue::makeString(desc));
        fn.set("parameters", std::move(params));
        JsonValue tool = JsonValue::makeObject();
        tool.set("type", JsonValue::makeString("function"));
        tool.set("function", std::move(fn));
        tools.push(std::move(tool));
    };
    auto strProp = [](const std::string& type, const std::string& desc) {
        JsonValue p = JsonValue::makeObject();
        p.set("type", JsonValue::makeString(type));
        p.set("description", JsonValue::makeString(desc));
        return p;
    };
    auto arrayProp = [](const std::string& itemType, std::size_t count,
                        const std::string& desc) {
        JsonValue p = JsonValue::makeObject();
        p.set("type", JsonValue::makeString("array"));
        JsonValue items = JsonValue::makeObject();
        items.set("type", JsonValue::makeString(itemType));
        p.set("items", items);
        p.set("minItems", JsonValue::makeNumber(static_cast<double>(count)));
        p.set("maxItems", JsonValue::makeNumber(static_cast<double>(count)));
        p.set("description", JsonValue::makeString(desc));
        return p;
    };
    auto requiredOf = [](std::initializer_list<const char*> keys) {
        JsonValue r = JsonValue::makeArray();
        for (const char* k : keys) {
            r.push(JsonValue::makeString(k));
        }
        return r;
    };

    {
        JsonValue props = JsonValue::makeObject();
        props.set("region", arrayProp("number", 6, "查询区域 [x1,y1,z1,x2,y2,z2]"));
        props.set("time_range", arrayProp("number", 2, "时间范围 [t1,t2](闭区间毫秒)"));
        props.set("level", strProp("integer", "可选:LOD 层级,缺省最细"));
        props.set("state", strProp("string", "可选:状态过滤 Pending|Stable|Changing"));
        props.set("payload_contains", strProp("string", "可选:负载子串过滤"));
        addTool("stmb_query", "按空间区域 + 时间范围查询时空记忆块",
                props, requiredOf({"region", "time_range"}));
    }
    {
        JsonValue props = JsonValue::makeObject();
        props.set("block_id", strProp("integer", "块 id"));
        props.set("timestamp", strProp("integer", "回溯时刻(毫秒)"));
        addTool("stmb_query_at", "时间回溯:查询块在某时刻生效的版本内容",
                props, requiredOf({"block_id", "timestamp"}));
    }
    {
        JsonValue props = JsonValue::makeObject();
        props.set("block_id", strProp("integer", "块 id"));
        addTool("stmb_history", "查询块的版本历史(按版本升序)",
                props, requiredOf({"block_id"}));
    }
    {
        JsonValue props = JsonValue::makeObject();
        props.set("region", arrayProp("number", 6, "块绑定区域 [x1,y1,z1,x2,y2,z2]"));
        props.set("payload", strProp("string", "负载语义"));
        props.set("timestamp", strProp("integer", "绑定时间点(毫秒)"));
        props.set("confidence", strProp("number", "可选:初始置信度 0~1"));
        props.set("level", strProp("integer", "可选:LOD 层级,缺省自动判定"));
        addTool("stmb_put", "写入新的时空记忆块",
                props, requiredOf({"region", "payload", "timestamp"}));
    }
    {
        JsonValue props = JsonValue::makeObject();
        props.set("block_id", strProp("integer", "目标块 id"));
        props.set("payload", strProp("string", "观测语义;空串 = 空地/消失"));
        props.set("confidence", strProp("number", "观测置信度 0~1"));
        props.set("source_id", strProp("integer", "观测来源 id"));
        props.set("timestamp", strProp("integer", "观测时刻(毫秒)"));
        addTool("stmb_observe", "提交观测,走变化分类与多源仲裁管线",
                props, requiredOf({"block_id", "payload", "confidence",
                                   "source_id", "timestamp"}));
    }
    {
        JsonValue props = JsonValue::makeObject();
        props.set("region", arrayProp("number", 6, "查询区域 [x1,y1,z1,x2,y2,z2]"));
        props.set("time_range", arrayProp("number", 2, "时间范围 [t1,t2]"));
        addTool("stmb_dynamic_query", "查询活跃动态实例(运动物体)",
                props, requiredOf({"region", "time_range"}));
    }
    addTool("stmb_stats", "查询服务统计信息",
            JsonValue::makeObject(), requiredOf({}));
    addTool("stmb_checkpoint", "落盘当前状态(快照/分片)并截断 WAL",
            JsonValue::makeObject(), requiredOf({}));
    return tools;
}

// 伪代码:
//   1. 通用校验:args 必须是 object,否则结构化错误;
//   2. 按工具名分派:
//      - stmb_query:读 region/time_range(+可选 level/state/payload_contains),
//        执行 queryLevel/query,过滤后序列化块数组;
//      - stmb_query_at:读 block_id/timestamp,走 getAt 时间回溯;
//      - stmb_history:读 block_id,返回版本数组;
//      - stmb_put:读 region/payload/timestamp(+可选 confidence/level),
//        组装块写入,返回新建块 id;
//      - stmb_observe:读全部观测字段,走 submitObservation,返回 ChangeReport;
//      - stmb_dynamic_query:读 region/time_range,返回实例数组;
//      - stmb_stats:返回 stats 对象(含动态层与分片计数);
//      - stmb_checkpoint:调用 checkpoint 返回成败;
//   3. 未知工具名返回 unknown_tool 错误。
JsonValue FunctionRegistry::dispatch(const std::string& name, const JsonValue& args) {
    if (!args.isObject()) {
        return fail("invalid_args", "工具参数必须是 JSON object");
    }
    JsonValue ok = JsonValue::makeObject();
    ok.set("ok", JsonValue::makeBool(true));
    JsonValue error;

    if (name == "stmb_query") {
        AABB region;
        TimeRange range;
        if (!readRegion(args, region, error)) return error;
        if (!readTimeRange(args, range, error)) return error;
        int level = service_.levelCount() - 1;
        if (args.has("level")) {
            if (!args.at("level").isNumber()) {
                return fail("invalid_args", "level 必须是整数");
            }
            level = static_cast<int>(args.at("level").asNumber());
        }
        std::vector<MemoryBlock> hits = service_.queryLevel(region, range, level);
        JsonValue blocks = JsonValue::makeArray();
        for (const MemoryBlock& b : hits) {
            if (args.has("state")) {
                if (!args.at("state").isString()) {
                    return fail("invalid_args", "state 必须是字符串");
                }
                if (toString(b.state) != args.at("state").asString()) {
                    continue;
                }
            }
            if (args.has("payload_contains")) {
                if (!args.at("payload_contains").isString()) {
                    return fail("invalid_args", "payload_contains 必须是字符串");
                }
                if (b.payload.find(args.at("payload_contains").asString()) ==
                    std::string::npos) {
                    continue;
                }
            }
            blocks.push(blockToJson(b));
        }
        ok.set("data", blocks);
        return ok;
    }
    if (name == "stmb_query_at") {
        std::uint64_t id = 0;
        std::uint64_t t = 0;
        if (!readUint(args, "block_id", id, error)) return error;
        if (!readUint(args, "timestamp", t, error)) return error;
        const std::optional<BlockVersion> v =
            service_.getAt(id, static_cast<TimeStamp>(t));
        if (!v.has_value()) {
            return fail("not_found", "该时刻没有生效版本(或块不存在)");
        }
        ok.set("data", versionToJson(*v));
        return ok;
    }
    if (name == "stmb_history") {
        std::uint64_t id = 0;
        if (!readUint(args, "block_id", id, error)) return error;
        JsonValue versions = JsonValue::makeArray();
        for (const BlockVersion& v : service_.historyOf(id)) {
            versions.push(versionToJson(v));
        }
        ok.set("data", versions);
        return ok;
    }
    if (name == "stmb_put") {
        AABB region;
        std::string payload;
        std::uint64_t ts = 0;
        if (!readRegion(args, region, error)) return error;
        if (!readString(args, "payload", payload, error)) return error;
        if (!readUint(args, "timestamp", ts, error)) return error;
        MemoryBlock block;
        block.region = region;
        block.payload = payload;
        block.timestamp = static_cast<TimeStamp>(ts);
        if (args.has("confidence")) {
            if (!args.at("confidence").isNumber()) {
                return fail("invalid_args", "confidence 必须是数字");
            }
            block.confidence = args.at("confidence").asNumber();
        }
        if (args.has("level")) {
            if (!args.at("level").isNumber()) {
                return fail("invalid_args", "level 必须是整数");
            }
            block.level = static_cast<int>(args.at("level").asNumber());
        }
        service_.put(block);
        // put 返回的是被淘汰块而非新 id,这里按「同区域 + 同时间戳 + 同负载」
        // 反查新建块的 id(服务未暴露 nextId,保持接口最小)
        std::uint64_t newId = 0;
        for (int level = 0; level < service_.levelCount(); ++level) {
            const auto hits = service_.queryLevel(
                region, TimeRange{block.timestamp, block.timestamp}, level);
            for (const MemoryBlock& b : hits) {
                if (b.payload == payload) {
                    newId = std::max(newId, b.id);
                }
            }
        }
        JsonValue data = JsonValue::makeObject();
        data.set("block_id", JsonValue::makeNumber(static_cast<double>(newId)));
        ok.set("data", data);
        return ok;
    }
    if (name == "stmb_observe") {
        std::uint64_t id = 0;
        std::string payload;
        double confidence = 1.0;
        std::uint64_t source = 0;
        std::uint64_t ts = 0;
        if (!readUint(args, "block_id", id, error)) return error;
        if (!readString(args, "payload", payload, error)) return error;
        if (!readNumber(args, "confidence", confidence, error)) return error;
        if (!readUint(args, "source_id", source, error)) return error;
        if (!readUint(args, "timestamp", ts, error)) return error;
        Observation ob;
        ob.payload = payload;
        ob.confidence = confidence;
        ob.source = source;
        ob.t = static_cast<TimeStamp>(ts);
        const ChangeReport report = service_.submitObservation(id, ob);
        JsonValue data = JsonValue::makeObject();
        data.set("type", JsonValue::makeString(toString(report.type)));
        data.set("accepted", JsonValue::makeBool(report.accepted));
        data.set("message", JsonValue::makeString(report.message));
        ok.set("data", data);
        return ok;
    }
    if (name == "stmb_dynamic_query") {
        AABB region;
        TimeRange range;
        if (!readRegion(args, region, error)) return error;
        if (!readTimeRange(args, range, error)) return error;
        JsonValue instances = JsonValue::makeArray();
        for (const DynamicInstance& inst : service_.queryDynamic(region, range)) {
            instances.push(instanceToJson(inst));
        }
        ok.set("data", instances);
        return ok;
    }
    if (name == "stmb_stats") {
        const ServiceStats s = service_.stats();
        const DynamicStats ds = service_.dynamicStats();
        JsonValue data = JsonValue::makeObject();
        data.set("blocks", JsonValue::makeNumber(static_cast<double>(s.blockCount)));
        data.set("capacity", JsonValue::makeNumber(static_cast<double>(s.capacity)));
        data.set("evictions", JsonValue::makeNumber(static_cast<double>(s.evictCount)));
        data.set("loaded_shards",
                 JsonValue::makeNumber(static_cast<double>(s.loadedShards)));
        data.set("dynamic_active",
                 JsonValue::makeNumber(static_cast<double>(ds.activeCount)));
        data.set("dynamic_stationary",
                 JsonValue::makeNumber(static_cast<double>(ds.stationaryCount)));
        data.set("dynamic_archived",
                 JsonValue::makeNumber(static_cast<double>(ds.archivedCount)));
        ok.set("data", data);
        return ok;
    }
    if (name == "stmb_checkpoint") {
        JsonValue data = JsonValue::makeObject();
        data.set("success", JsonValue::makeBool(service_.checkpoint()));
        ok.set("data", data);
        return ok;
    }
    return fail("unknown_tool", "未知工具: " + name);
}

}  // namespace stmb
