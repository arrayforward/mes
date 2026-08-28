// ============================================================================
// 文件: tests/vql/test_vql.cpp
// 模块: stmb_tests(VQL 与 Function Calling 单元测试)
// 覆盖范围:
//   1. 迷你 JSON:各类型 round-trip、转义、嵌套、紧凑/缩进序列化,
//      非法输入报错(不崩溃);
//   2. VQL 解析:全部语句形式、大小写混合、可选子句组合;语法错误与
//      语义错误(未知关键字、STATE 非法值、REGION 参数个数错)均有明确
//      位置化错误信息;
//   3. VQL 执行:对真实服务执行各语句,结果数据正确;AT 回溯返回旧版本;
//      SUMMARY 返回粗+细;
//   4. Function Calling:toolSchemas 结构完整(8 个工具、必填字段、JSON
//      Schema 类型);8 个工具逐个 dispatch round-trip;缺参/错类型/未知
//      工具均返回结构化错误。
// 测试思路: 真实 StmbService(LOD 配置)写入测试数据后执行;
//           #undef NDEBUG 保证 Release 下断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "functions.h"
#include "json.h"
#include "vql.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>

namespace {

using namespace stmb;

// 伪代码:
//   1. 向已构造的服务写入:L0 骨架 "skeleton"(大区域)、L2 细节
//      "detail-a"/"detail-b";
//   2. 对骨架 confirm 转 Stable 后做一次变更(v2),供历史/回溯用例使用。
void populateLod(StmbService& service) {
    MemoryBlock skel;
    skel.region = AABB{{0.0, 0.0, 0.0}, {100.0, 100.0, 100.0}};
    skel.payload = "skeleton";
    skel.timestamp = 100;
    skel.level = 0;
    service.put(skel);                                        // id1
    MemoryBlock da;
    da.region = AABB{{9.0, 9.0, 9.0}, {11.0, 11.0, 11.0}};
    da.payload = "detail-a";
    da.timestamp = 200;
    da.level = 2;
    service.put(da);                                          // id2
    MemoryBlock db;
    db.region = AABB{{19.0, 9.0, 9.0}, {21.0, 11.0, 11.0}};
    db.payload = "detail-b";
    db.timestamp = 300;
    db.level = 2;
    service.put(db);                                          // id3
    assert(service.confirm(1, 400));                          // -> Stable
    assert(service.reportChange(1, "skeleton-v2", 0.9, 500)); // -> Changing
    assert(service.confirm(1, 600));                          // -> Stable v2
}

// 伪代码(本地构造 LOD 服务):StmbService 不可拷贝/移动,测试在各自函数内
//   直接构造并调用 populateLod 填充数据。

// 伪代码(JSON):
//   1. 各类型 round-trip:构造含 null/bool/number/string/array/object 的
//      嵌套对象,dump 后 reparse,逐字段断言一致;
//   2. 转义:含引号/反斜杠/换行的字符串 round-trip;
//   3. 缩进序列化:dump(2) 含换行,reparse 仍一致;
//   4. 非法输入(未闭合、缺冒号、空数字、尾部多余)全部返回空 optional
//      且 error 非空,不崩溃。
void testJson() {
    JsonValue obj = JsonValue::makeObject();
    obj.set("name", JsonValue::makeString("体素\"引号\"\\反斜杠\n换行"));
    obj.set("value", JsonValue::makeNumber(-12.5));
    obj.set("flag", JsonValue::makeBool(true));
    obj.set("nothing", JsonValue::makeNull());
    JsonValue arr = JsonValue::makeArray();
    arr.push(JsonValue::makeNumber(1.0));
    arr.push(JsonValue::makeString("two"));
    JsonValue nested = JsonValue::makeObject();
    nested.set("k", JsonValue::makeBool(false));
    arr.push(nested);
    obj.set("list", arr);

    const std::optional<JsonValue> back = JsonValue::parse(obj.dump(), nullptr);
    assert(back.has_value());
    assert(back->at("name").asString() == "体素\"引号\"\\反斜杠\n换行");
    assert(back->at("value").asNumber() == -12.5);
    assert(back->at("flag").asBool());
    assert(back->at("nothing").isNull());
    assert(back->at("list").asArray().size() == 3);
    assert(back->at("list").asArray()[2].at("k").asBool() == false);

    const std::string pretty = obj.dump(2);
    assert(pretty.find('\n') != std::string::npos);
    assert(JsonValue::parse(pretty, nullptr).has_value());

    for (const char* bad : {"{", "[1,", "\"abc", "{\"a\" 1}", "{\"a\":}",
                            "[1 2]", "nul", "true false"}) {
        std::string error;
        assert(!JsonValue::parse(bad, &error).has_value());
        assert(!error.empty());
    }
    std::cout << "[PASS] vql: 迷你 JSON round-trip/转义/非法输入\n";
}

// 伪代码(VQL 解析):
//   1. 全部语句形式(5 种)+ 大小写混合 + 可选子句组合,断言执行 ok;
//   2. 错误路径:未知关键字、STATE 非法值、REGION 参数个数错、缺 DURING、
//      末尾多余 token:断言 ok=false 且 error 含「位置」或「语义错误」。
void testVqlParse() {
    StmbService service(ServiceConfig{5000, 1.0, 1000, 1.0, 0, "", 0.0, 0, 8,
                                      {100.0, 10.0, 1.0}, 0, 0, 0});
    populateLod(service);
    VqlEngine engine(service);

    for (const char* sql : {
             "FIND BLOCKS IN REGION(0,0,0,50,50,50) DURING(0,1000)",
             "find blocks in region(0,0,0,50,50,50) during(0,1000) level 2",
             "FIND BLOCKS IN REGION(0,0,0,50,50,50) DURING(0,1000) STATE Stable",
             "FIND BLOCKS IN REGION(0,0,0,50,50,50) DURING(0,1000) PAYLOAD ~ 'detail'",
             "FIND BLOCKS IN REGION(0,0,0,50,50,50) DURING(0,1000) AT 100 LIMIT 1",
             "FIND BLOCKS IN REGION(0,0,0,50,50,50) DURING(0,1000) SUMMARY",
             "FIND BLOCKS IN REGION(0,0,0,50,50,50) DURING(0,1000) LEVEL 0 STATE Stable PAYLOAD ~ 'skeleton' AT 550 LIMIT 5",
             "FIND HISTORY OF 1",
             "STATS",
         }) {
        const VqlResult r = engine.execute(sql);
        assert(r.ok);
    }

    for (const char* sql : {
             "DELETE BLOCKS",
             "FIND BLOCKS IN REGION(0,0,0,50,50) DURING(0,1000)",     // 参数个数
             "FIND BLOCKS IN REGION(0,0,0,50,50,50)",                 // 缺 DURING
             "FIND BLOCKS IN REGION(0,0,0,50,50,50) DURING(0,1000) STATE Weird",
             "FIND BLOCKS IN REGION(0,0,0,50,50,50) DURING(0,1000) FOO",
             "FIND HISTORY OF",
         }) {
        const VqlResult r = engine.execute(sql);
        assert(!r.ok);
        assert(!r.error.empty());
        assert(r.error.find("位置") != std::string::npos ||
               r.error.find("语义错误") != std::string::npos);
    }
    std::cout << "[PASS] vql: 语法解析(合法形式/错误位置化提示)\n";
}

// 伪代码(VQL 执行):
//   1. FIND BLOCKS 基本查询:命中 2 个细节块(L2 缺省最细);
//   2. LEVEL 0:命中骨架;STATE Stable 过滤;PAYLOAD ~ 'detail-a' 过滤;
//   3. AT 100 回溯:骨架返回 v1 旧 payload "skeleton"(而非当前的 v2);
//   4. SUMMARY:返回粗层骨架 + 下钻细节(粗+细同列);
//   5. FIND HISTORY OF 1:两个版本;STATS 含 blocks 字段;
//   6. 动态语句在独立动态服务上执行(见 Function Calling 用例共用)。
void testVqlExecute() {
    StmbService service(ServiceConfig{5000, 1.0, 1000, 1.0, 0, "", 0.0, 0, 8,
                                      {100.0, 10.0, 1.0}, 0, 0, 0});
    populateLod(service);
    VqlEngine engine(service);

    VqlResult r = engine.execute(
        "FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000)");
    assert(r.ok && r.data.asArray().size() == 2);  // 缺省最细层级

    r = engine.execute(
        "FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) LEVEL 0");
    assert(r.data.asArray().size() == 1);
    assert(r.data.asArray()[0].at("payload").asString() == "skeleton-v2");

    r = engine.execute(
        "FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) LEVEL 0 STATE Stable");
    assert(r.data.asArray().size() == 1);
    r = engine.execute(
        "FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) LEVEL 0 STATE Pending");
    assert(r.data.asArray().empty());

    r = engine.execute(
        "FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) PAYLOAD ~ 'detail-a'");
    assert(r.data.asArray().size() == 1);
    assert(r.data.asArray()[0].at("payload").asString() == "detail-a");

    r = engine.execute(
        "FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) LEVEL 0 AT 100");
    assert(r.data.asArray().size() == 1);
    assert(r.data.asArray()[0].at("payload").asString() == "skeleton");  // 旧版本
    r = engine.execute(
        "FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) LEVEL 0 AT 700");
    assert(r.data.asArray()[0].at("payload").asString() == "skeleton-v2");

    r = engine.execute(
        "FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) SUMMARY");
    assert(r.data.asArray().size() == 3);  // 骨架 + 2 细节(先粗后细)

    r = engine.execute("FIND HISTORY OF 1");
    assert(r.data.asArray().size() == 2);
    assert(r.data.asArray()[0].at("payload").asString() == "skeleton");
    assert(r.data.asArray()[1].at("payload").asString() == "skeleton-v2");

    r = engine.execute(
        "FIND BLOCKS IN REGION(0,0,0,100,100,100) DURING(0,1000) LIMIT 1");
    assert(r.data.asArray().size() == 1);

    r = engine.execute("STATS");
    assert(r.ok && r.data.has("blocks") && r.data.at("blocks").asNumber() == 3.0);
    std::cout << "[PASS] vql: 执行(过滤/AT 回溯/SUMMARY/HISTORY/STATS)\n";
}

// 伪代码(Function Calling):
//   1. toolSchemas:断言是 8 元数组,每个工具含 type/function/name/
//      description/parameters,JSON Schema 类型字段存在;
//   2. 逐个 dispatch round-trip:stmb_put 写块 -> stmb_query 命中 ->
//      stmb_observe 提交观测 -> stmb_history -> stmb_query_at 回溯 ->
//      stmb_dynamic_query -> stmb_stats -> stmb_checkpoint;
//   3. 错误路径:缺 region、region 长度错、block_id 类型错、未知工具:
//      断言 {ok:false, error:{code}} 结构。
void testFunctionCalling() {
    StmbService service(ServiceConfig{100, 10.0, 1000, 2.0, 0, "", 0.0, 0, 8,
                                      {}, 1000, 60000, 0});
    FunctionRegistry registry(service);

    const JsonValue schemas = registry.toolSchemas();
    assert(schemas.isArray() && schemas.asArray().size() == 8);
    for (const JsonValue& tool : schemas.asArray()) {
        assert(tool.at("type").asString() == "function");
        assert(tool.has("function"));
        assert(!tool.at("function").at("name").asString().empty());
        assert(!tool.at("function").at("description").asString().empty());
        assert(tool.at("function").at("parameters").at("type").asString() == "object");
    }

    auto call = [&registry](const std::string& name, const std::string& argsText) {
        const std::optional<JsonValue> args = JsonValue::parse(argsText, nullptr);
        assert(args.has_value());
        return registry.dispatch(name, *args);
    };

    JsonValue r = call("stmb_put",
        R"({"region":[0,0,0,10,10,10],"payload":"house","timestamp":100,"confidence":0.8})");
    assert(r.at("ok").asBool());
    const std::uint64_t blockId =
        static_cast<std::uint64_t>(r.at("data").at("block_id").asNumber());
    assert(blockId == 1);

    r = call("stmb_query", R"({"region":[0,0,0,20,20,20],"time_range":[0,1000]})");
    assert(r.at("ok").asBool() && r.at("data").asArray().size() == 1);
    assert(r.at("data").asArray()[0].at("payload").asString() == "house");

    r = call("stmb_observe",
        R"({"block_id":1,"payload":"shop","confidence":0.9,"source_id":1,"timestamp":200})");
    assert(r.at("ok").asBool());
    // 块尚处 Pending 即收到矛盾观测:管线判 Conflict 挂起
    assert(r.at("data").at("type").asString() == "Conflict");

    r = call("stmb_history", R"({"block_id":1})");
    assert(r.at("ok").asBool());

    r = call("stmb_query_at", R"({"block_id":1,"timestamp":100})");
    assert(r.at("ok").asBool());
    assert(r.at("data").at("payload").asString() == "house");

    service.reportMoving(1, "car", AABB{{4.0, 4.0, 4.0}, {6.0, 6.0, 6.0}},
                         {0.0, 0.0, 0.0}, 100, 1);
    r = call("stmb_dynamic_query", R"({"region":[0,0,0,20,20,20],"time_range":[0,1000]})");
    assert(r.at("ok").asBool() && r.at("data").asArray().size() == 1);
    assert(r.at("data").asArray()[0].at("class").asString() == "car");

    r = call("stmb_stats", "{}");
    assert(r.at("ok").asBool());
    assert(r.at("data").at("blocks").asNumber() == 1.0);
    assert(r.at("data").at("dynamic_active").asNumber() == 1.0);

    r = call("stmb_checkpoint", "{}");
    assert(r.at("ok").asBool());  // 未开持久化返回 success:false 但调用合法

    // 错误路径
    r = call("stmb_query", R"({"time_range":[0,1000]})");
    assert(!r.at("ok").asBool() && r.at("error").has("code"));
    r = call("stmb_query", R"({"region":[0,0,0,10,10],"time_range":[0,1000]})");
    assert(!r.at("ok").asBool());
    r = call("stmb_history", R"({"block_id":"abc"})");
    assert(!r.at("ok").asBool());
    r = call("stmb_xxx", "{}");
    assert(!r.at("ok").asBool());
    assert(r.at("error").at("code").asString() == "unknown_tool");
    std::cout << "[PASS] vql: Function Calling(schemas/8 工具/结构化错误)\n";
}

}  // namespace

// 伪代码:
//   1. 依次调用四个测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testJson();
    testVqlParse();
    testVqlExecute();
    testFunctionCalling();
    std::cout << "ALL TESTS PASS (vql)\n";
    return 0;
}
