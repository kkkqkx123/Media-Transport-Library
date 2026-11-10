#!/bin/bash
# dvs_node_smoke: single-shot bundle + IPC + agent HTTP gate (board runnable).
# Only std tools + curl + python3. Never touches reboot/factory-reset/upgrade.
# Usage: dvs_node_smoke.sh [--agent-base URL] [--sock PATH]
#        [--media-bin PATH] [--agent-bin PATH] [--version-file PATH]
# Env overrides: AGENT_BASE, DVS_MEDIA_SOCK, MEDIA_BIN, AGENT_BIN, VERSION_FILE.
set -euo pipefail

AGENT_BASE="${AGENT_BASE:-http://127.0.0.1:8080}"
SOCK="${DVS_MEDIA_SOCK:-/run/dvs/media.sock}"
MEDIA_BIN="${MEDIA_BIN:-dvs_media}"
AGENT_BIN="${AGENT_BIN:-dvs_agent}"
VERSION_FILE="${VERSION_FILE:-}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROBE="$SCRIPT_DIR/dvs_ipc_probe.py"

fail=0
say() { echo "[dvs_node_smoke] $*"; }
bad() { echo "[dvs_node_smoke] FAIL: $*" >&2; fail=1; }
good() { echo "[dvs_node_smoke] ok: $*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --agent-base) AGENT_BASE="$2"; shift 2 ;;
    --sock) SOCK="$2"; shift 2 ;;
    --media-bin) MEDIA_BIN="$2"; shift 2 ;;
    --agent-bin) AGENT_BIN="$2"; shift 2 ;;
    --version-file) VERSION_FILE="$2"; shift 2 ;;
    -h|--help) sed -n '1,12p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

# 1) bundle version files
EXPECT=""
if [ -n "$VERSION_FILE" ] && [ -f "$VERSION_FILE" ]; then
  EXPECT="$(tr -d ' \t\r\n' < "$VERSION_FILE")"
elif [ -f "$SCRIPT_DIR/../../deploy/dvs-node/VERSION" ]; then
  EXPECT="$(tr -d ' \t\r\n' < "$SCRIPT_DIR/../../deploy/dvs-node/VERSION")"
fi
for b in "$MEDIA_BIN" "$AGENT_BIN"; do
  if ! command -v "$b" >/dev/null 2>&1 && [ ! -x "$b" ]; then
    bad "missing binary: $b"; continue
  fi
  out="$("$b" --version 2>&1 || true)"
  echo "$out"
  if [ -n "$EXPECT" ]; then
    case "$out" in
      *"$EXPECT"*) good "$b bundle $EXPECT" ;;
      *) bad "$b version mismatch (expect $EXPECT): $out" ;;
    esac
  else
    good "$b --version ran (no VERSION file to compare)"
  fi
done

# 2) IPC read-only verbs (skip gracefully when engine not up)
if [ -S "$SOCK" ]; then
  for verb in hello capability query_state; do
    if python3 "$PROBE" --sock "$SOCK" --type "$verb" --request-id "smoke-$verb" >/tmp/dvs_ipc_$verb.json 2>&1; then
      good "ipc $verb ok"
    else
      rc=$?
      if [ "$rc" -eq 2 ]; then
        bad "ipc $verb honest-reject (ok:false): $(cat /tmp/dvs_ipc_$verb.json)"
      else
        bad "ipc $verb transport fail: $(cat /tmp/dvs_ipc_$verb.json)"
      fi
    fi
  done
else
  say "skip ipc verbs (no socket $SOCK; engine down is honest, agent must report offline)"
fi

# 3) agent HTTP: version/info/capability/media/status + frontend
need_curl() { command -v curl >/dev/null 2>&1 || { bad "missing curl"; exit 1; }; }
need_py() { command -v python3 >/dev/null 2>&1 || { bad "missing python3"; exit 1; }; }
need_curl; need_py

get_ok() { # $1 path -> prints body, returns curl status
  curl -sS -m 5 "$AGENT_BASE$1" || return 1
}
check_envelope() { # $1 file: must have ok/status/code/msg/data; requestId optional
  # Agent-generated envelopes carry requestId; proxied engine-offline
  # envelopes (MEDIA_ENGINE_*) honestly carry no requestId. Both pass.
  python3 - "$1" <<'EOF'
import json,sys
env=json.load(open(sys.argv[1]))
missing=[k for k in ("ok","status","code","msg","data") if k not in env]
sys.exit(1 if missing else 0)
EOF
}

for p in /api/v1/node/version /api/v1/node/info /api/v1/node/capability /api/v1/node/media/status; do
  f="/tmp/dvs_smoke$(echo "$p" | tr '/' '_').json"
  if ! get_ok "$p" >"$f" 2>/tmp/dvs_curl_err.log; then
    bad "GET $p curl fail: $(cat /tmp/dvs_curl_err.log)"; continue
  fi
  if check_envelope "$f"; then
    if python3 - "$f" <<'EOF' 2>/dev/null; then
import json,sys
env=json.load(open(sys.argv[1]))
assert env.get("ok") is False
assert str(env.get("code","")).startswith("MEDIA_ENGINE_")
EOF
      say "GET $p honest-fail(engine offline)"
    else
      good "GET $p envelope"
    fi
  else
    bad "GET $p bad envelope: $(head -c 500 "$f")"
  fi
done

# version gate: bundle/ipc fields must exist; match must be bool; offline without engine is valid
if ! python3 - /tmp/dvs_smoke_api_v1_node_version.json <<'EOF' 2>/dev/null; then
import json
env=json.load(open("/tmp/dvs_smoke_api_v1_node_version.json"))
d=env.get("data",{})
assert "bundleVersion" in d and "ipcVersion" in d and "match" in d, "version fields"
assert isinstance(d["match"],bool), "match bool"
assert "engineLink" in d, "engineLink"
EOF
  bad "version gate fields invalid"
else
  good "version gate fields"
fi

# frontend: root redirect + node-admin page must be 200
for p in / /pages/node-admin/node-admin.html; do
  code="$(curl -sS -m 5 -o /dev/null -w "%{http_code}" "$AGENT_BASE$p" 2>/dev/null || echo 000)"
  if [ "$code" = 200 ]; then good "frontend $p 200"; else bad "frontend $p http=$code"; fi
done

if [ "$fail" -ne 0 ]; then say "OVERALL: FAIL"; else say "OVERALL: PASS"; fi
exit "$fail"
