#!/usr/bin/env python3
# ============================================================================
# 文件: agent/deepseek_agent.py
# 用途: 大模型联调的 Python 代理:DeepSeek API(OpenAI 兼容端点)与
#       stmb_shell(C++ 时空体素记忆服务)之间的工具调用桥。
# 架构:
#   deepseek-v4-flash <-> 本脚本(urllib,工具调用循环)
#       <-> stdin/stdout JSON Lines <-> stmb_shell(C++, stmb_vql dispatch)
# 流程:
#   1. 启动 stmb_shell 子进程(stdin/stdout 管道),先取 tools schema;
#   2. system prompt 声明角色与约定(工具操作时空体素记忆,毫秒时间戳,
#      中文回答);
#   3. 用户输入 -> 追加 messages -> 调 chat/completions;
#   4. 有 tool_calls 就逐个发给 shell 执行,结果以 tool role 回填,
#      循环直到模型给出最终文本;
#   5. 工具调用过程(调用名 + 参数 + 结果摘要)打印到 stderr 便于观察。
# 依赖: 纯标准库(urllib/json/os/sys/argparse);API key 只从环境变量
#       DEEPSEEK_API_KEY 读取,绝不硬编码。
# ============================================================================
import argparse
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

SYSTEM_PROMPT = (
    "你是时空体素记忆服务的助手。你可以通过工具查询/写入/观测一个时空体素"
    "记忆系统:记忆块绑定三维区域(AABB)和毫秒时间戳,支持按区域与时间"
    "范围查询、时间回溯、版本历史、观测提交(会自动做变化分类与多源仲裁)、"
    "动态运动物体查询、统计与落盘。所有时间戳一律使用毫秒整数。请用中文回答。"
)


# 伪代码:
#   1. 组装请求体(model/messages/tools/tool_choice);
#   2. POST 到 <base>/chat/completions(Bearer 鉴权,超时 60s);
#   3. HTTP 错误打印响应体并以非零码退出;正常返回解析后的 JSON。
def chat(base_url, api_key, model, messages, tools):
    body = {
        "model": model,
        "messages": messages,
        "tool_choice": "auto",
    }
    if tools:
        body["tools"] = tools
    request = urllib.request.Request(
        base_url.rstrip("/") + "/chat/completions",
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + api_key,
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        print(f"[agent] API HTTP {exc.code}: {detail}", file=sys.stderr)
        sys.exit(1)
    except urllib.error.URLError as exc:
        print(f"[agent] 网络错误: {exc.reason}", file=sys.stderr)
        sys.exit(1)


# 伪代码(JSON Lines 子进程封装):
#   1. Popen 启动 stmb_shell(stdin/stdout 管道,文本模式行缓冲);
#   2. rpc:写入一行请求 JSON -> 读一行响应 -> json.loads 返回;
#   3. close:关闭 stdin 并等待子进程退出。
class StmbShell:
    def __init__(self, shell_args):
        self.proc = subprocess.Popen(
            shell_args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    def rpc(self, request):
        self.proc.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("stmb_shell 意外退出")
        return json.loads(line)

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


# 伪代码(单轮问答):
#   1. 把用户输入追加进 messages;
#   2. 循环(上限 10 轮):调 API;
#   3. 若返回 tool_calls:逐个打印到 stderr、发给 shell 执行、以 tool role
#      回填结果(紧凑 JSON),继续循环;
#   4. 否则打印最终文本并返回。
def run_conversation(base_url, api_key, model, shell, tools, messages, user_text):
    messages.append({"role": "user", "content": user_text})
    for _ in range(10):
        response = chat(base_url, api_key, model, messages, tools)
        message = response["choices"][0]["message"]
        messages.append(message)
        tool_calls = message.get("tool_calls") or []
        if not tool_calls:
            return message.get("content", "")
        for call in tool_calls:
            name = call["function"]["name"]
            raw_args = call["function"].get("arguments") or "{}"
            try:
                args = json.loads(raw_args)
            except json.JSONDecodeError:
                args = {}
            print(f"[tool] {name} {raw_args}", file=sys.stderr)
            result = shell.rpc({"cmd": "call", "name": name, "arguments": args})
            summary = json.dumps(result, ensure_ascii=False)
            print(f"[tool] <= {summary[:300]}", file=sys.stderr)
            messages.append({
                "role": "tool",
                "tool_call_id": call["id"],
                "content": summary,
            })
    return "(工具调用轮数超限,中止)"


# 伪代码:
#   1. 解析参数;--help 由 argparse 自动处理(无需 key);
#   2. 校验 DEEPSEEK_API_KEY(缺失打印提示退出码 2);
#   3. 组装 stmb_shell 启动参数(--data/--capacity/分片透传)并启动;
#   4. 取 schemas;构造 messages(system prompt);
#   5. --query 单轮,否则 REPL(输入 quit 退出);结束关闭子进程。
def main():
    parser = argparse.ArgumentParser(
        description="DeepSeek 大模型 <-> stmb 时空体素记忆服务 联调代理")
    parser.add_argument("--query", help="单轮提问;缺省进入 REPL(quit 退出)")
    parser.add_argument("--model", default="deepseek-v4-flash",
                        help="模型名(默认 deepseek-v4-flash)")
    parser.add_argument("--base-url", default="https://api.deepseek.com/v1",
                        help="OpenAI 兼容端点(默认 DeepSeek)")
    parser.add_argument("--shell",
                        default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                             "..", "build", "src", "agent", "stmb_shell"),
                        help="stmb_shell 可执行文件路径")
    parser.add_argument("--data", help="持久化数据目录(透传给 stmb_shell)")
    parser.add_argument("--capacity", help="内存块容量上限(透传)")
    parser.add_argument("--shard-cell-size", help="分片边长(与 --shard-time-span 同给开启分片)")
    parser.add_argument("--shard-time-span", help="分片时间桶跨度毫秒(透传)")
    args = parser.parse_args()

    api_key = os.environ.get("DEEPSEEK_API_KEY")
    if not api_key:
        print("错误: 未设置环境变量 DEEPSEEK_API_KEY,请先导出再运行,例如:\n"
              "  export DEEPSEEK_API_KEY=sk-...", file=sys.stderr)
        sys.exit(2)

    shell_args = [args.shell]
    if args.data:
        shell_args += ["--data", args.data]
    if args.capacity:
        shell_args += ["--capacity", args.capacity]
    if args.shard_cell_size and args.shard_time_span:
        shell_args += ["--shard-cell-size", args.shard_cell_size,
                       "--shard-time-span", args.shard_time_span]

    shell = StmbShell(shell_args)
    try:
        schemas = shell.rpc({"cmd": "schemas"})
        tools = schemas.get("data", []) if schemas.get("ok") else []
        print(f"[agent] 已连接 stmb_shell,工具数: {len(tools)},模型: {args.model}",
              file=sys.stderr)
        messages = [{"role": "system", "content": SYSTEM_PROMPT}]
        if args.query:
            print(run_conversation(args.base_url, api_key, args.model,
                                   shell, tools, messages, args.query))
        else:
            print("stmb 联调 REPL(输入 quit 退出)")
            while True:
                try:
                    user_text = input("> ").strip()
                except EOFError:
                    break
                if user_text.lower() == "quit":
                    break
                if not user_text:
                    continue
                print(run_conversation(args.base_url, api_key, args.model,
                                       shell, tools, messages, user_text))
    finally:
        shell.close()


if __name__ == "__main__":
    main()
