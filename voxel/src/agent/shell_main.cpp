// ============================================================================
// 文件: shell_main.cpp
// 模块: stmb_shell(JSON Lines 协议外壳,链接 stmb_vql)
// 用途: 大模型联调的 C++ 端点:一个贯穿会话的 StmbService 实例,通过
//       stdin/stdout 的 JSON Lines 协议向 Python 代理暴露能力。
// 协议(每行一个请求/响应,stdout 只出协议 JSON,日志走 stderr):
//   {"cmd":"schemas"}                          -> {"ok":true,"data":<tools 数组>}
//   {"cmd":"vql","query":"FIND ..."}           -> {"ok":bool,"data":...|"error":...}
//   {"cmd":"call","name":"stmb_query","arguments":{...}}
//                                              -> dispatch 原样结果 {ok,data|error}
//   坏 JSON / 未知 cmd                          -> {"ok":false,"error":{code,message}}
//   任何错误都不退出进程,继续读下一行。
// 启动参数:
//   --data DIR              开启持久化(数据目录)
//   --capacity N            内存块容量上限(默认 1024)
//   --shard-cell-size X     分片边长(与 --shard-time-span 同给时开分片模式)
//   --shard-time-span MS    分片时间桶跨度(毫秒)
// 设计思路:
//   1. 解析参数组装 ServiceConfig,构造单个服务实例贯穿会话——工具调用
//      之间状态在 shell 进程内持续累积(联调核心诉求);
//   2. 主循环按行读请求,逐行写紧凑 JSON 响应并立即 flush;
//   3. 一切错误就地包装成协议错误返回,进程不退出。
// 架构角色: 联调架构的 C++ 末端,不依赖 agent 侧任何实现。
// ============================================================================
#include "functions.h"
#include "json.h"
#include "vql.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

// 伪代码:
//   1. 组装 {ok:false, error:{code, message}} 的协议错误对象。
stmb::JsonValue makeError(const std::string& code, const std::string& message) {
    stmb::JsonValue root = stmb::JsonValue::makeObject();
    root.set("ok", stmb::JsonValue::makeBool(false));
    stmb::JsonValue err = stmb::JsonValue::makeObject();
    err.set("code", stmb::JsonValue::makeString(code));
    err.set("message", stmb::JsonValue::makeString(message));
    root.set("error", err);
    return root;
}

// 伪代码:
//   1. 打印用法到 stderr。
void printUsage(const char* argv0) {
    std::cerr << "用法: " << argv0 << " [--data DIR] [--capacity N]"
              << " [--shard-cell-size X --shard-time-span MS]\n";
}

}  // namespace

// 伪代码:
//   1. 解析启动参数到 ServiceConfig(未知参数打印用法返回 2);
//   2. 构造服务 / VqlEngine / FunctionRegistry,向 stderr 打一行就绪提示;
//   3. 按行读 stdin:解析 JSON(失败回协议错误),按 cmd 分派
//      schemas / vql / call,逐行向 stdout 写紧凑 JSON 并 flush;
//   4. stdin 结束(EOF)退出 0。
int main(int argc, char** argv) {
    stmb::ServiceConfig config{1024, 10.0, 1000, 1.0, 0, "", 0.0, 0, 8,
                               {}, 0, 0, 0};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "参数 " << name << " 缺少值\n";
                printUsage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--data") {
            config.dataDir = needValue("--data");
        } else if (arg == "--capacity") {
            config.capacity = std::strtoull(needValue("--capacity").c_str(),
                                            nullptr, 10);
        } else if (arg == "--shard-cell-size") {
            config.shardCellSize = std::strtod(needValue("--shard-cell-size").c_str(),
                                               nullptr);
        } else if (arg == "--shard-time-span") {
            config.shardTimeSpanMs =
                std::strtoll(needValue("--shard-time-span").c_str(), nullptr, 10);
        } else if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "未知参数: " << arg << "\n";
            printUsage(argv[0]);
            return 2;
        }
    }

    stmb::StmbService service(config);
    stmb::VqlEngine engine(service);
    stmb::FunctionRegistry registry(service);
    std::cerr << "stmb_shell 就绪:dataDir=" << (config.dataDir.empty() ? "(内存)" : config.dataDir)
              << " capacity=" << config.capacity
              << " shard=" << (config.shardCellSize > 0.0 ? "on" : "off") << "\n";

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::string parseError;
        const std::optional<stmb::JsonValue> request =
            stmb::JsonValue::parse(line, &parseError);
        stmb::JsonValue response;
        if (!request.has_value() || !request->isObject()) {
            response = makeError("bad_json",
                                 "请求不是合法 JSON object: " + parseError);
        } else if (!request->has("cmd") || !request->at("cmd").isString()) {
            response = makeError("missing_param", "缺少 cmd 字段(字符串)");
        } else {
            const std::string cmd = request->at("cmd").asString();
            if (cmd == "schemas") {
                response = stmb::JsonValue::makeObject();
                response.set("ok", stmb::JsonValue::makeBool(true));
                response.set("data", registry.toolSchemas());
            } else if (cmd == "vql") {
                if (!request->has("query") || !request->at("query").isString()) {
                    response = makeError("missing_param", "vql 命令缺少 query 字符串");
                } else {
                    const stmb::VqlResult r =
                        engine.execute(request->at("query").asString());
                    response = stmb::JsonValue::makeObject();
                    response.set("ok", stmb::JsonValue::makeBool(r.ok));
                    if (r.ok) {
                        response.set("data", r.data);
                    } else {
                        response.set("error", stmb::JsonValue::makeString(r.error));
                    }
                }
            } else if (cmd == "call") {
                if (!request->has("name") || !request->at("name").isString()) {
                    response = makeError("missing_param", "call 命令缺少 name 字符串");
                } else {
                    const stmb::JsonValue& args =
                        request->has("arguments") ? request->at("arguments")
                                                  : stmb::JsonValue::makeObject();
                    response = registry.dispatch(request->at("name").asString(), args);
                }
            } else {
                response = makeError("unknown_cmd", "未知 cmd: " + cmd);
            }
        }
        std::cout << response.dump() << "\n" << std::flush;
    }
    return 0;
}
