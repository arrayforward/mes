// ============================================================================
// 文件: tests/agent/test_shell.cpp
// 模块: stmb_tests(stmb_shell 协议单元测试,离线,不调大模型 API)
// 覆盖范围:
//   1. schemas 命令:响应 ok 且 data 为 8 个工具的数组;
//   2. call 命令状态累积:stmb_put 写入 -> stmb_query 在同一 shell 会话中
//      命中(工具调用之间内存态持续);
//   3. vql 命令:STATS 与 FIND BLOCKS 执行结果正确;
//   4. 健壮性:坏 JSON / 未知 cmd / 缺参数均返回结构化错误且进程不退出,
//      后续请求仍正常响应。
// 测试思路: fork + 双向管道启动 stmb_shell 子进程(路径由 /proc/self/exe
//           推导:build/tests/test_shell -> build/src/agent/stmb_shell),
//           逐行写请求、逐行读响应并断言;结束时关闭管道并 waitpid。
//           #undef NDEBUG 保证 Release 下断言生效。
// ============================================================================
#ifdef NDEBUG
#undef NDEBUG  // 保证 Release 构建下 assert 仍然生效
#endif

#include "json.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using stmb::JsonValue;

// 伪代码:
//   1. 读 /proc/self/exe 得到当前测试二进制绝对路径;
//   2. 去掉文件名部分,拼接 "/../src/agent/stmb_shell" 返回。
std::string shellPath() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    assert(n > 0);
    buf[n] = '\0';
    std::string path(buf);
    const std::size_t slash = path.find_last_of('/');
    return path.substr(0, slash) + "/../src/agent/stmb_shell";
}

// 伪代码(双向管道子进程):
//   1. 建两条管道:父->子、子->父;
//   2. fork:子进程 dup2 到 stdin/stdout 后 execl stmb_shell;
//   3. 父进程保留写端/读端,fdopen 成 FILE* 便于按行读写。
struct ShellProc {
    pid_t pid = -1;
    FILE* toChild = nullptr;    // 父写入 -> 子 stdin
    FILE* fromChild = nullptr;  // 子 stdout -> 父读出
};

// 伪代码:
//   1. pipe 两条管道并 fork;
//   2. 子进程:重定向标准输入/输出到管道,execl(shellPath);
//   3. 父进程:关闭多余端,fdopen 两端,填入 out。
ShellProc spawnShell() {
    int parentToChild[2];
    int childToParent[2];
    assert(pipe(parentToChild) == 0);
    assert(pipe(childToParent) == 0);

    const pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        dup2(parentToChild[0], STDIN_FILENO);
        dup2(childToParent[1], STDOUT_FILENO);
        close(parentToChild[0]);
        close(parentToChild[1]);
        close(childToParent[0]);
        close(childToParent[1]);
        const std::string path = shellPath();
        execl(path.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);  // execl 失败兜底
    }
    close(parentToChild[0]);
    close(childToParent[1]);
    ShellProc proc;
    proc.pid = pid;
    proc.toChild = fdopen(parentToChild[1], "w");
    proc.fromChild = fdopen(childToParent[0], "r");
    assert(proc.toChild != nullptr && proc.fromChild != nullptr);
    return proc;
}

// 伪代码:
//   1. 向子进程写一行请求并 flush;
//   2. 从子进程读一行响应,断言读取成功;
//   3. 解析响应 JSON(断言合法)并返回。
JsonValue rpc(ShellProc& proc, const std::string& request) {
    assert(std::fwrite(request.data(), 1, request.size(), proc.toChild) == request.size());
    assert(std::fwrite("\n", 1, 1, proc.toChild) == 1);
    assert(std::fflush(proc.toChild) == 0);
    char buf[65536];
    assert(std::fgets(buf, sizeof(buf), proc.fromChild) != nullptr);
    std::string error;
    const std::optional<JsonValue> response = JsonValue::parse(buf, &error);
    assert(response.has_value());
    return *response;
}

// 伪代码:
//   1. 关闭写端(子进程见 EOF 退出),关闭读端;
//   2. waitpid 回收子进程。
void closeShell(ShellProc& proc) {
    std::fclose(proc.toChild);
    std::fclose(proc.fromChild);
    int status = 0;
    waitpid(proc.pid, &status, 0);
}

// 伪代码(协议全流程):
//   1. schemas:ok 且 data 为 8 元数组,含 stmb_query;
//   2. call stmb_put 写入 "house":ok 且 block_id 为 1;
//   3. call stmb_query 同区域:命中 1 块且 payload 为 "house"(状态累积);
//   4. vql STATS:blocks == 1;vql FIND BLOCKS:命中 1 块;
//   5. 坏 JSON / 未知 cmd / call 缺 name:ok=false,且后续请求仍正常
//     (进程未退出)。
void testProtocol() {
    ShellProc proc = spawnShell();

    JsonValue r = rpc(proc, R"({"cmd":"schemas"})");
    assert(r.at("ok").asBool());
    assert(r.at("data").isArray() && r.at("data").asArray().size() == 8);
    bool sawQuery = false;
    for (const JsonValue& tool : r.at("data").asArray()) {
        if (tool.at("function").at("name").asString() == "stmb_query") {
            sawQuery = true;
        }
    }
    assert(sawQuery);

    r = rpc(proc, R"({"cmd":"call","name":"stmb_put","arguments":{"region":[0,0,0,10,10,10],"payload":"house","timestamp":100}})");
    assert(r.at("ok").asBool());
    assert(r.at("data").at("block_id").asNumber() == 1.0);

    r = rpc(proc, R"({"cmd":"call","name":"stmb_query","arguments":{"region":[0,0,0,20,20,20],"time_range":[0,1000]}})");
    assert(r.at("ok").asBool());
    assert(r.at("data").asArray().size() == 1);
    assert(r.at("data").asArray()[0].at("payload").asString() == "house");

    r = rpc(proc, R"json({"cmd":"vql","query":"STATS"})json");
    assert(r.at("ok").asBool());
    assert(r.at("data").at("blocks").asNumber() == 1.0);

    r = rpc(proc, R"json({"cmd":"vql","query":"FIND BLOCKS IN REGION(0,0,0,10,10,10) DURING(0,1000)"})json");
    assert(r.at("ok").asBool());
    assert(r.at("data").asArray().size() == 1);

    // 健壮性:坏 JSON / 未知 cmd / 缺参数 -> 结构化错误,进程不退出
    r = rpc(proc, "{oops");
    assert(!r.at("ok").asBool() && r.at("error").has("code"));
    r = rpc(proc, R"({"cmd":"nope"})");
    assert(!r.at("ok").asBool());
    assert(r.at("error").at("code").asString() == "unknown_cmd");
    r = rpc(proc, R"({"cmd":"call"})");
    assert(!r.at("ok").asBool());
    r = rpc(proc, R"({"cmd":"vql","query":"STATS"})");  // 错误后仍正常响应
    assert(r.at("ok").asBool());

    closeShell(proc);
    std::cout << "[PASS] agent: stmb_shell 协议(schemas/call/vql/健壮性)\n";
}

}  // namespace

// 伪代码:
//   1. 调用协议测试函数(任一断言失败都会立即终止进程);
//   2. 全部通过后打印总结并返回 0。
int main() {
    testProtocol();
    std::cout << "ALL TESTS PASS (agent)\n";
    return 0;
}
