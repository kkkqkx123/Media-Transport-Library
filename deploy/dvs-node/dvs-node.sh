#!/bin/sh
# dvs-node: supervisor for the co-released DVS node bundle.
#
# Starts dvs_media (controlled mode) first, then dvs_agent against the same
# IPC socket. Both binaries belong to the VERSION file in this directory;
# the agent confirms the pairing at startup via the hello handshake and
# serves it at GET /api/v1/node/version. No extra runtime dependencies.
set -eu

HERE=$(dirname "$0")
BUNDLE_VERSION=$(cat "$HERE/VERSION")

SOCK=${DVS_MEDIA_SOCK:-/run/dvs/media.sock}
CFG=${DVS_MEDIA_CONFIG:-}
PORT=${DVS_AGENT_PORT:-8080}
DB=${DVS_AGENT_DB:-/var/lib/dvs/agent.db}
WEB_ROOT=${DVS_WEB_ROOT:-/usr/share/dvs/web}

MEDIA_BIN=${DVS_MEDIA_BIN:-dvs_media}
AGENT_BIN=${DVS_AGENT_BIN:-dvs_agent}

if [ -z "$CFG" ]; then
  echo "dvs-node: DVS_MEDIA_CONFIG is not set (static base config for dvs_media)" >&2
  exit 1
fi

mkdir -p "$(dirname "$SOCK")" "$(dirname "$DB")"

"$MEDIA_BIN" --version | grep -q "$BUNDLE_VERSION" || {
  echo "dvs-node: media binary is not bundle $BUNDLE_VERSION" >&2
  exit 1
}
"$AGENT_BIN" --version | grep -q "$BUNDLE_VERSION" || {
  echo "dvs-node: agent binary is not bundle $BUNDLE_VERSION" >&2
  exit 1
}

"$MEDIA_BIN" --config "$CFG" --ipc-sock "$SOCK" &
MEDIA_PID=$!
# Wait until the engine serves the socket (bounded, then fail fast).
for _ in $(seq 1 50); do
  if [ -S "$SOCK" ]; then break; fi
  sleep 0.1
done

DVS_MEDIA_SOCK="$SOCK" "$AGENT_BIN" --db "$DB" --port "$PORT" \
  --media-sock "$SOCK" --web-root "$WEB_ROOT" &
AGENT_PID=$!

cleanup() {
  kill "$AGENT_PID" "$MEDIA_PID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup TERM INT

wait "$AGENT_PID" "$MEDIA_PID"
