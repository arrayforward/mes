#!/usr/bin/env bash
# ============================================================================
# mse/server/smoke.sh —— mse_server 端点冒烟验证(人工验证用,不进 ctest)
#
# 用法:./server/smoke.sh [端口]   (默认 18090;需已构建 build/mse_server.exe)
# 覆盖:静态资源、路径穿越、自省端点形状、写动词信任分级、异步 drain、
#       读动词视图求值、重启持久化。任一环节不符合预期即非零退出。
# ============================================================================
set -u
cd "$(dirname "$0")/.."
PORT="${1:-18090}"
BASE="http://127.0.0.1:$PORT"
EXE="build/mse_server.exe"
DB="$(mktemp -u /tmp/mse_smoke_XXXX.db)"
VDB="$(mktemp -u /tmp/mse_smoke_v_XXXX.db)"
FAIL=0

check() {  # check <描述> <实际> <期望子串>
    if [[ "$2" == *"$3"* ]]; then
        echo "  [OK] $1"
    else
        echo "  [失败] $1:期望含 '$3',实际: $2"; FAIL=1
    fi
}

"$EXE" --port "$PORT" --db "$DB" --voxel-db "$VDB" & SRV=$!
trap 'kill $SRV 2>/dev/null; rm -f "$DB" "$VDB"' EXIT
sleep 2

echo "== 静态资源 =="
check "GET /"               "$(curl -s -o /dev/null -w '%{http_code}' $BASE/)" 200
check "GET /static/index.html" "$(curl -s -o /dev/null -w '%{http_code}' $BASE/static/index.html)" 200
check "路径穿越被拒" "$(curl -s -o /dev/null -w '%{http_code}' --path-as-is $BASE/static/../CMakeLists.txt)" 403
check "未知路径 404"  "$(curl -s $BASE/nope)" '"error":"not found"'

echo "== 自省端点 =="
check "/meta/views 44 视图"        "$(curl -s $BASE/meta/views | grep -o '"view_id"' | wc -l)" 44
check "/meta/event-types 形状"     "$(curl -s $BASE/meta/event-types | head -c 200)" '"min_trust"'
check "/meta/attributes 形状"      "$(curl -s $BASE/meta/attributes | head -c 200)" '"semantic"'
check "/meta/anchors 形状"         "$(curl -s $BASE/meta/anchors | head -c 100)" '"path"'
check "/meta/ontologies 空库"      "$(curl -s $BASE/meta/ontologies)" '[]'
check "/meta/events 空库"          "$(curl -s $BASE/meta/events)" '[]'

echo "== 写动词:信任分级 =="
EVJSON="$(pwd)/.mse_smoke_ev.json"   # curl 为原生程序,须用 Windows 路径
python -c "import sys; open(sys.argv[1],'w',encoding='utf-8').write('{\"type\":\"PlcEdgeReported\",\"id\":\"工位01\",\"actor\":\"plc-gw\",\"工位占用\":\"占用\"}')" "$(cygpath -w "$EVJSON")"
check "无凭证 → L2 信任级不足"  "$(curl -s -X POST $BASE/events --data-binary "@$(cygpath -w "$EVJSON")")" '"status":"rejected"'
check "错凭证 → L2 信任级不足"  "$(curl -s -X POST $BASE/events -H 'X-MSE-Token: bogus' --data-binary "@$(cygpath -w "$EVJSON")")" '"layer":2'
check "plc-gw-token → accepted" "$(curl -s -X POST $BASE/events -H 'X-MSE-Token: plc-gw-token' --data-binary "@$(cygpath -w "$EVJSON")")" '"status":"accepted"'
check "POST /drain 落账 1 条"   "$(curl -s -X POST $BASE/drain)" '"drained":1'
check "/meta/events 新事件在前" "$(curl -s "$BASE/meta/events?limit=5")" 'PlcEdgeReported'

echo "== 读动词:视图求值 =="
OBS=$(python -c "import urllib.parse; print(urllib.parse.quote('计划员'))")
check "GET /views/V-PLAN-A3" "$(curl -s "$BASE/views/V-PLAN-A3?observer=$OBS" | grep -o '"view_id":"[^"]*"' | head -1)" '"view_id":"V-PLAN-A3"'

kill $SRV 2>/dev/null; wait $SRV 2>/dev/null

echo "== 重启持久化(不 --seed) =="
"$EXE" --port "$PORT" --db "$DB" --voxel-db "$VDB" & SRV=$!
sleep 2
check "重启后事件仍在" "$(curl -s $BASE/meta/events | grep -o '"event_id"' | wc -l)" 1
check "重启后视图仍在" "$(curl -s $BASE/meta/views | grep -o '"view_id"' | wc -l)" 44

kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
rm -f "$DB" "$VDB" "$EVJSON"
trap - EXIT
echo
if [[ $FAIL -eq 0 ]]; then echo "冒烟验证:全部通过"; else echo "冒烟验证:存在失败项"; exit 1; fi
