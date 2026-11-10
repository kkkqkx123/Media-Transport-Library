#!/bin/bash
# v4l2_fmt_matrix.sh — 逐格式协商矩阵 (板端执行)
#
# 对每个候选 fourcc 做 S_FMT + G_FMT 回读校验, 汇总驱动真实接受情况。
# 枚举列表不可信, 以本矩阵为准。
# 用法: [PROBE=/path/to/v4l2_cap_latency] bash v4l2_fmt_matrix.sh [count]
set -euo pipefail

BIN="${PROBE:-$HOME/code/mtl/bin/v4l2_cap_latency}"
COUNT="${1:-5}"

[ -x "$BIN" ] || { echo "probe not found: $BIN"; exit 1; }

echo "fourcc | result"
echo "-------+------------------------------"
for f in NV12 NV16 NV24 BGR3 UYVY YUYV; do
  OUT=$("$BIN" --pixfmt "$f" --no-stream 2>&1 \
        | grep -E "negotiated|keeping driver|S_FMT.*refused" | tr '\n' ';')
  echo "$f    | $OUT"
done
