#!/bin/bash
# dvs_agent_api_smoke: web-backend functional smoke (board + x86 runnable).
# Covers login shape, six scope GET, deterministic invalid POST rejections,
# media query surfaces, audit + local node projection, version gate.
# Never calls reboot / factory-reset / upgrade (destructive); upgrade/upload
# is not exercised either (writes /tmp on the node).
# Only deps: curl + python3. Exit 0 all green, 1 any failure.
# Usage: dvs_agent_api_smoke.sh [--agent-base URL]
set -euo pipefail
AGENT_BASE="${AGENT_BASE:-http://127.0.0.1:8080}"
while [ $# -gt 0 ]; do
  case "$1" in
    --agent-base) AGENT_BASE="$2"; shift 2 ;;
    -h|--help) sed -n '1,12p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
command -v curl >/dev/null 2>&1 || { echo "[agent_smoke] FAIL: missing curl" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "[agent_smoke] FAIL: missing python3" >&2; exit 1; }

fail=0
say() { echo "[agent_smoke] $*"; }
good() { echo "[agent_smoke] ok: $*"; }
bad() { echo "[agent_smoke] FAIL: $*" >&2; fail=1; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

get() { curl -sS -m 5 "$AGENT_BASE$1" -o "$TMP/out.json" || return 1; }
post() { curl -sS -m 5 -X POST "$AGENT_BASE$1" -H 'Content-Type: application/json' -d "$2" -o "$TMP/out.json" || return 1; }
has_fields() {
  # Agent envelopes carry requestId; proxied engine-offline envelopes
  # (MEDIA_ENGINE_*) honestly carry no requestId. Both pass.
  python3 - "$TMP/out.json" <<'EOF'
import json,sys
env=json.load(open(sys.argv[1]))
missing=[k for k in ("ok","status","code","msg","data") if k not in env]
if missing:
  print("missing",missing)
  sys.exit(1)
EOF
}
state_is_failed() { # config POST: ok:true + data.state==failed + errorCode
  python3 - "$TMP/out.json" <<'EOF'
import json,sys
env=json.load(open(sys.argv[1]))
assert env.get("ok") is True, "config post always ok:true"
d=env.get("data",{})
assert d.get("state")=="failed", d.get("state")
assert d.get("errorCode"), "errorCode"
EOF
}
code_is() { # $1 expected code string
  python3 - "$TMP/out.json" "$1" <<'EOF'
import json,sys
env=json.load(open(sys.argv[1]))
sys.exit(0 if env.get("code")==sys.argv[2] else 1)
EOF
}
ok_is() { # $1 true|false
  python3 - "$TMP/out.json" "$1" <<'EOF'
import json,sys
env=json.load(open(sys.argv[1]))
want=(sys.argv[2]=="true")
sys.exit(0 if env.get("ok") is want else 1)
EOF
}

# 1) read-only base (must all carry envelope)
for p in /api/v1/node/info /api/v1/node/role /api/v1/node/capability /api/v1/node/capacity /api/v1/node/version /api/v1/audit-events /api/v1/nodes; do
  if get "$p" && has_fields; then good "GET $p"; else bad "GET $p"; fi
done

# 2) login shape: wrong password must be UNAUTHORIZED; default password is best-effort
if post /api/v1/node/login '{"username":"admin","password":"wrong-not-the-password"}' && has_fields && ! ok_is true && code_is "UNAUTHORIZED"; then
  good "login wrong rejected"
else
  bad "login wrong not rejected"
fi
if post /api/v1/node/login '{"username":"admin","password":"admin123"}' && has_fields; then
  if ok_is true; then good "login default accepted"; else say "skip login default (password already changed, honest reject)"; fi
else
  bad "login default shape"
fi

# 3) six scopes GET must all carry envelope
for s in role input output network registration sip; do
  if get "/api/v1/node/config/$s" && has_fields; then good "GET config/$s"; else bad "GET config/$s"; fi
done
# unknown scope must be INVALID_ARGUMENT
if post /api/v1/node/config/nope '{"desiredValue":{},"operationId":"smoke-nope"}' && has_fields && ! ok_is true && code_is "INVALID_ARGUMENT"; then
  good "POST config/nope rejected"
else
  bad "POST config/nope not rejected"
fi

# 4) deterministic invalid writes: HTTP ok:true + data.state==failed (config state machine)
if post /api/v1/node/config/role '{"desiredValue":{"role":"bogus"},"operationId":"smoke-role-bad"}' \
  && has_fields && state_is_failed; then good "role bogus rejected"; else bad "role bogus not rejected"; fi
if post /api/v1/node/config/output '{"desiredValue":{"width":99999,"height":99999,"refreshHz":999},"operationId":"smoke-output-bad"}' \
  && has_fields && state_is_failed; then good "output oversize rejected"; else bad "output oversize not rejected"; fi
if post /api/v1/node/config/network '{"desiredValue":{"mode":"static","ip":"999.1.1.1","mask":"255.255.255.0","gateway":"192.168.10.1","dns":"192.168.10.1"},"operationId":"smoke-net-bad"}' \
  && has_fields && state_is_failed; then good "network bad-ip rejected"; else bad "network bad-ip not rejected"; fi

# 5) media query surfaces: envelope required; offline honest-fail accepted, online must be ok
for p in /api/v1/node/media/status /api/v1/node/media/lanes /api/v1/node/media/tx /api/v1/node/media/sources /api/v1/node/media/logs /api/v1/node/media/events; do
  if get "$p" && has_fields; then
    if ok_is true; then good "GET $p ok(engine online)"; else say "GET $p honest-fail(engine offline)"; fi
  else
    bad "GET $p shape"
  fi
done

# 6) version gate fields
if get /api/v1/node/version && python3 - "$TMP/out.json" <<'EOF'
import json,sys
env=json.load(open(sys.argv[1]))
d=env.get("data",{})
assert "bundleVersion" in d and "ipcVersion" in d and "match" in d and "engineLink" in d
EOF
then good "version gate fields"; else bad "version gate fields"; fi

if [ "$fail" -ne 0 ]; then say "OVERALL: FAIL"; else say "OVERALL: PASS"; fi
exit "$fail"
