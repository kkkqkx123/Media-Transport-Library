#!/usr/bin/env bash
# Verify A-grade shared modules stay byte-identical across:
#   app/rk3588_common, app/rk3588_end2end, app/rk3588_mp4_dist
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

COMMON="$ROOT/app/rk3588_common"
E2E="$ROOT/app/rk3588_end2end"
MP4="$ROOT/app/rk3588_mp4_dist"

# A-grade modules: common src/include must match both apps' local copies.
pairs=(
  "ring_buffer.c|$COMMON/src/ring_buffer.c|$E2E/pipeline/ring_buffer.c|$MP4/shared/ring_buffer.c"
  "ring_buffer.h|$COMMON/include/rk3588_common/ring_buffer.h|$E2E/pipeline/ring_buffer.h|$MP4/shared/ring_buffer.h"
  "slab_pool.c|$COMMON/src/slab_pool.c|$E2E/hal/slab_pool.c|$MP4/shared/slab_pool.c"
  "slab_pool.h|$COMMON/include/rk3588_common/slab_pool.h|$E2E/hal/slab_pool.h|$MP4/shared/slab_pool.h"
  "timer.h|$COMMON/include/rk3588_common/timer.h|$E2E/hal/timer.h|$MP4/shared/timer.h"
)

fail=0
for entry in "${pairs[@]}"; do
  IFS='|' read -r name f1 f2 f3 <<<"$entry"
  h1="$(md5sum "$f1" | awk '{print $1}')"
  h2="$(md5sum "$f2" | awk '{print $1}')"
  h3="$(md5sum "$f3" | awk '{print $1}')"
  if [[ "$h1" != "$h2" || "$h1" != "$h3" ]]; then
    echo "MISMATCH: $name"
    echo "  common: $h1"
    echo "  e2e:   $h2"
    echo "  mp4:   $h3"
    fail=1
  else
    echo "OK: $name ($h1)"
  fi
done

if [[ $fail -ne 0 ]]; then
  echo "check_sync: FAILED" >&2
  exit 1
fi
echo "check_sync: all A-grade modules in sync"
