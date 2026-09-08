#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# mse/tools/run_all_playbooks.py —— H5 验证剧本库总跑
#
# 顺序执行并汇总退出码:
#   ui_playbook.py   仅在给了外部地址时跑(依赖 mse_demo 灌的背景数据):
#                    python mse/tools/run_all_playbooks.py 127.0.0.1:18080
#   pb2_supply_chain.py / pb3_quality_compliance.py /
#   pb4_plan_disruption.py / pb5_ops_integration.py
#                    全部自管理 Server(自建临时库,自造数据),始终执行。
# 退出码 0 = 全部剧本通过。
# ============================================================================
import os
import subprocess
import sys

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
SELF_MANAGED = [
    "pb2_supply_chain.py",
    "pb3_quality_compliance.py",
    "pb4_plan_disruption.py",
    "pb5_ops_integration.py",
]


def run(script, args):
    print("\n" + "#" * 66)
    print(f"# 跑剧本:{script} {' '.join(args)}")
    print("#" * 66)
    p = subprocess.run([sys.executable, os.path.join(HERE, script)] + args)
    return p.returncode


def main():
    results = {}
    if len(sys.argv) > 1:
        results["ui_playbook.py"] = run("ui_playbook.py", [sys.argv[1]])
    else:
        print("(未给外部地址,跳过 ui_playbook;它依赖 mse_demo 灌的背景数据,"
              "用法:python mse/tools/run_all_playbooks.py host:port)")
    for script in SELF_MANAGED:
        results[script] = run(script, [])

    print("\n" + "=" * 66)
    print("剧本库总账:")
    for script, rc in results.items():
        print(f"  {'PASS' if rc == 0 else 'FAIL'}  {script} (exit={rc})")
    print("=" * 66)
    sys.exit(0 if all(rc == 0 for rc in results.values()) else 1)


if __name__ == "__main__":
    main()
