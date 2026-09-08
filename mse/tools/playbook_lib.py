#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# mse/tools/playbook_lib.py —— H5 验证剧本公共库
#
# 两份职责:
#   1) HTTP 帮助函数:api/post/view/row_of/btn_of/events_of/events_all,
#      与 H5 界面发同一批请求(POST /events、GET /views/{id}、GET /meta/events);
#   2) Server:自管理 mse_server 进程(临时库 + 随机端口 + 就绪轮询 +
#      stop/restart),供"自造全部数据"的剧本(pb2~pb5)做崩溃恢复验证。
#
# 校验计数:g_checks/g_fails 全局累计,check() 打印 [OK]/[FAIL],
# summary() 打印总账并 sys.exit(0/1)。剧本只 import,不自行计数。
#
# 用法(自管理剧本):
#   from playbook_lib import Server, set_base, post, view, check, banner, summary
#   with Server() as s:
#       set_base(s.base)
#       ...剧本...
#   summary()
# 打外部服务器(如 ui_playbook):
#   set_base("http://127.0.0.1:18080")
# ============================================================================
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

# 控制台 UTF-8:GBK 终端不乱码,也避免特殊字符(如 ↔)触发 UnicodeEncodeError
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

BASE = None            # 剧本先 set_base(...) 再调用帮助函数
TOKEN = "ui-token"     # 默认凭证(L2;服务器另有 plc-gw-token/erp-token,L1)

g_checks = 0
g_fails = 0


def check(ok, label, detail=""):
    global g_checks, g_fails
    g_checks += 1
    if ok:
        print(f"  [OK] {label}")
    else:
        g_fails += 1
        print(f"  [FAIL] {label}  {detail}")


def banner(title):
    print("=" * 62)
    print(f"{title} → {BASE}")
    print("=" * 62)


def summary():
    print("\n" + "=" * 62)
    print(f"剧本验证:checks={g_checks} fails={g_fails}")
    print("=" * 62)
    sys.exit(0 if g_fails == 0 else 1)


def set_base(base):
    """绑定帮助函数的请求目标('host:port' 或 'http://host:port')。"""
    global BASE
    BASE = base if base.startswith("http") else "http://" + base


def api(method, path, body=None, token=TOKEN):
    data = None
    if body is not None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(BASE + path, data=data, method=method)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("X-MSE-Token", token)
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read().decode("utf-8"))


def post(type_, id_, kvs, actor, space=None, token=TOKEN, **syskeys):
    """发起事件候选。kvs = 扁平属性写入(应用到 id);syskeys 可带
    space/time/evidence/corrects/idempotency_key/writes(显式多目标形态,
    给了 writes 时 id_ 传 None 或与 writes 目标一致)。"""
    payload = {"type": type_, "actor": actor, **kvs}
    if id_ is not None:
        payload["id"] = id_
    if space:
        payload["space"] = space
    payload.update(syskeys)
    return api("POST", "/events", payload, token)


def drain():
    """异步队列手动落账(settlement=async 的类型已验未结,drain 后成事实)。"""
    return api("POST", "/drain")


def view(vid, **params):
    q = urllib.parse.urlencode(params)
    return api("GET", f"/views/{urllib.parse.quote(vid)}" + ("?" + q if q else ""))


def view_text(vid, **params):
    """视图原始响应文本(崩溃恢复逐字节比对用)。"""
    q = urllib.parse.urlencode(params)
    req = urllib.request.Request(
        BASE + f"/views/{urllib.parse.quote(vid)}" + ("?" + q if q else ""))
    req.add_header("X-MSE-Token", TOKEN)
    with urllib.request.urlopen(req) as resp:
        return resp.read().decode("utf-8")


def row_of(v, rid):
    for r in v.get("rows", []):
        if r.get("id") == rid:
            return r
    return None


def btn_of(row, type_):
    for b in (row or {}).get("buttons", []):
        if b["type"] == type_:
            return b
    return None


def action_of(v, type_):
    for a in v.get("actions", []):
        if a["type"] == type_:
            return a
    return None


def events_all(limit=500):
    return api("GET", f"/meta/events?limit={limit}")


def events_of(type_):
    return [e for e in events_all() if e["type"] == type_]


def node_ids(v):
    return {n.get("id") for n in v.get("nodes", [])}


def has_edge(v, from_, key, to):
    return any(e.get("from") == from_ and e.get("key") == key and e.get("to") == to
               for e in v.get("edges", []))


# ============================================================================
# Server:自管理 mse_server(临时库、随机端口、就绪轮询、崩溃恢复重启)
# ============================================================================
class Server:
    """自管理 mse_server 进程。

    exe 查找顺序:环境变量 MSE_SERVER → 仓库相对路径 <repo>/build/mse_server.exe
    (tools/ 的上两级即仓库根)→ PATH。启动:tempfile.mkdtemp 临时目录,
    socket 探测随机空闲端口,Popen 拉起
    `mse_server.exe --port P --db T/a.db --voxel-db T/av.db`(stdout 进文件),
    轮询 GET /meta/views 直到 200 或 10s 超时。stop() 终止进程;
    restart() 停后用同一 db 重启(崩溃恢复验证用,投影由日志重放逐比特重建)。
    """

    def __init__(self, exe=None):
        self.exe = exe or self._find_exe()
        self.tmp = tempfile.mkdtemp(prefix="mse_playbook_")
        self.db = os.path.join(self.tmp, "a.db")
        self.vdb = os.path.join(self.tmp, "av.db")
        self.log_path = os.path.join(self.tmp, "server.out")
        self.proc = None
        self.port = None
        self.base = None
        self._start()

    @staticmethod
    def _find_exe():
        cand = os.environ.get("MSE_SERVER")
        if cand and os.path.isfile(cand):
            return cand
        repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        cand = os.path.join(repo, "build", "mse_server.exe")
        if os.path.isfile(cand):
            return cand
        cand = shutil.which("mse_server.exe") or shutil.which("mse_server")
        if cand:
            return cand
        raise RuntimeError(
            "找不到 mse_server.exe(试过 MSE_SERVER、<repo>/build、PATH)")

    @staticmethod
    def _free_port():
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]

    def _start(self):
        self.port = self._free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self._log = open(self.log_path, "ab")
        self.proc = subprocess.Popen(
            [self.exe, "--port", str(self.port),
             "--db", self.db, "--voxel-db", self.vdb],
            stdout=self._log, stderr=subprocess.STDOUT)
        deadline = time.time() + 10.0
        while time.time() < deadline:
            if self.proc.poll() is not None:
                self._log.flush()
                raise RuntimeError(
                    f"mse_server 启动即退出(code={self.proc.returncode}),"
                    f"日志见 {self.log_path}")
            try:
                with urllib.request.urlopen(self.base + "/meta/views",
                                            timeout=1) as resp:
                    if resp.status == 200:
                        return
            except Exception:
                time.sleep(0.1)
        self.stop()
        raise RuntimeError(f"mse_server 10s 未就绪,日志见 {self.log_path}")

    def stop(self):
        """终止进程(模拟停机;库文件留在临时目录供 restart 重放)。"""
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        if getattr(self, "_log", None) and not self._log.closed:
            self._log.close()

    def restart(self):
        """停后用同一 db 重启(新随机端口;base 随之更新)。"""
        self.stop()
        self._start()

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.cleanup()
        return False
