#!/bin/bash
# dvs_platform board smoke: live-process validation for the queue/fanout
# paths (list-backed up_q, move-only UpgradeJob, by-ref fanout capture).
# Starts dvs_platform on a scratch DB, drives upgrade enqueue + ops prepare
# over HTTP, and asserts the worker drained the queue and fanout ran without
# crashing. Safe on a live board: fake member/node ids only take the
# skip paths (no traffic sent, nothing rebooted).
#
# Board use (bin/ holds an executable copy, see docs/dev/board-mtl-sync.md):
#   BIN_DIR=~/code/mtl/bin ~/code/mtl/bin/verify_board_smoke.sh [--port N]
# Outputs (logs + scratch db) stay under ~/code/mtl/perf/verify-<date>/.
set -uo pipefail

PORT="${PORT:-18081}"
TOKEN="board-smoke-token"
while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="$2"; shift 2 ;;
    -h|--help) sed -n '1,12p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

BIN_DIR="${BIN_DIR:-$HOME/code/mtl/bin}"
PLATFORM_BIN="${PLATFORM_BIN:-$BIN_DIR/dvs_platform}"
command -v curl >/dev/null 2>&1 || { echo "FAIL: missing curl"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "FAIL: missing python3"; exit 1; }
[ -x "$PLATFORM_BIN" ] || { echo "FAIL: missing $PLATFORM_BIN"; exit 1; }

DAY="$(date +%Y%m%d)"
OUT="$HOME/code/mtl/perf/verify-$DAY"
mkdir -p "$OUT"
DB="$OUT/platform_smoke.db"
LOG="$OUT/platform_smoke.log"
BASE="http://127.0.0.1:$PORT"
rm -f "$DB" "$LOG"

fail=0
say() { echo "[platform_smoke] $*"; }
good() { echo "[platform_smoke] ok: $*"; }
bad() { echo "[platform_smoke] FAIL: $*" >&2; fail=1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# --- start platform on scratch db ---
DVS_API_TOKEN="$TOKEN" nohup "$PLATFORM_BIN" --db "$DB" --port "$PORT" \
  >"$LOG" 2>&1 &
PPID_SMOKE=$!
cleanup() {
  kill -INT "$PPID_SMOKE" 2>/dev/null || true
  sleep 1
  kill -9 "$PPID_SMOKE" 2>/dev/null || true
}
trap 'cleanup; rm -rf "$TMP"' INT TERM EXIT

for _ in $(seq 1 50); do
  grep -q "listening on" "$LOG" 2>/dev/null && break
  kill -0 "$PPID_SMOKE" 2>/dev/null || { bad "platform exited early"; tail -n 20 "$LOG"; exit 1; }
  sleep 0.2
done
grep -q "listening on" "$LOG" || { bad "platform did not listen"; tail -n 20 "$LOG"; exit 1; }
good "platform listening (db=$DB)"

auth_post() { # path json -> $TMP/out.json (with token)
  curl -sS -m 5 -X POST "$BASE$1" -H 'Content-Type: application/json' \
    -H "Authorization: Bearer $TOKEN" -d "$2" -o "$TMP/out.json";
}

# --- 1) auth gate (no token must 401) ---
curl -sS -m 5 -X POST "$BASE/api/v1/ops/prepare" -H 'Content-Type: application/json' \
  -d '{"operationId":"x"}' -o "$TMP/out.json" || bad "no-token request failed at transport"
if python3 - "$TMP/out.json" <<'EOF'
import json,sys
env=json.load(open(sys.argv[1]))
assert env.get("code")=="UNAUTHORIZED", env
EOF
then good "unauthorized prepare rejected"
else bad "unauthorized prepare not rejected"; fi

# --- 2) upgrade enqueue + worker drain (list queue, move-only job) ---
auth_post "/api/v1/nodes/upgrade" \
  '{"targetNodeIds":["smoke-node"],"filename":"fw.bin","sha256":"abc","fwVersion":"v9"}' \
  || bad "upgrade enqueue transport failed"
TASK="$(python3 - "$TMP/out.json" <<'EOF'
import json,sys
env=json.load(open(sys.argv[1]))
assert env.get("ok") is True, env
print(env["data"]["taskId"])
EOF
)" || bad "upgrade enqueue not ok:true"
[ -n "${TASK:-}" ] && good "upgrade enqueued task=$TASK"
sleep 3  # worker: front/move-out, ws miss, no doc -> upgrade-skip audit
if python3 - "$DB" <<'EOF'
import sqlite3,sys
rows=[r[0] for r in sqlite3.connect(sys.argv[1]).execute("SELECT event FROM audit_log")]
assert any('"upgrade-skip":"smoke-node"' in e for e in rows), rows[-5:]
print("worker drained queue, skip-audited")
EOF
then good "upgrade worker drained queue (skip path)"
else bad "worker did not drain queue (see $LOG)"; fi

# --- 3) fanout ×20 (by-ref ids capture over spawned+joined threads) ---
OK_N=0
for i in $(seq 1 20); do
  auth_post "/api/v1/ops/prepare" \
    "{\"operationId\":\"smoke-op-$i\",\"members\":[\"smoke-n1\",\"smoke-n2\"],\"volume\":50}" \
    || { bad "prepare #$i transport failed"; break; }
  if python3 - "$TMP/out.json" <<'EOF'
import json,sys
env=json.load(open(sys.argv[1]))
assert env.get("ok") is True, env
missing={m["node"]: m["reason"] for m in env["data"].get("missing", [])}
assert missing.get("smoke-n1")=="unknown-node", env
assert missing.get("smoke-n2")=="unknown-node", env
EOF
  then OK_N=$((OK_N+1))
  else bad "prepare #$i unexpected envelope"; break; fi
done
[ "$OK_N" -eq 20 ] && good "20× prepare accepted (missing=unknown-node)" || bad "only $OK_N/20 prepares ok"
sleep 2  # let detached fanout threads finish
kill -0 "$PPID_SMOKE" 2>/dev/null && good "platform alive after 20× fanout" || bad "platform died during fanout"
SKIP_N="$(python3 - "$DB" <<'EOF'
import sqlite3,sys
rows=[r[0] for r in sqlite3.connect(sys.argv[1]).execute("SELECT event FROM audit_log")]
print(sum(1 for e in rows if '"fanout-skip"' in e))
EOF
)"
[ "${SKIP_N:-0}" -ge 40 ] && good "fanout ran in threads (skip audits=$SKIP_N)" || bad "fanout audits missing ($SKIP_N)"

cleanup
trap 'rm -rf "$TMP"' INT TERM EXIT
echo ""
[ "$fail" -eq 0 ] && echo "OVERALL: PASS (logs+db: $OUT)" || echo "OVERALL: FAIL (logs+db: $OUT)"
exit "$fail"
