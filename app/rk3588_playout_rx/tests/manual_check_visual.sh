#!/bin/bash
# 人工看屏核查包装器：复用 run_multinode_composite.sh 的场景，把“必须人眼确认”的部分变成按步骤执行的检查单。
# 效仿 debug/tests/display_compare.sh：机器跑流，人工逐项确认并留痕。
#
# 机器日志证明不了的三件事，必须人眼确认：
#   1. 像素正确性（曾出现日志全绿但屏幕横向 3 分块的前科）；
#   2. 持续稳定性（闪烁/撕裂/黑场不一定留日志）；
#   3. 音频可听性（ST30 帧数 + ALSA 建连不等于出声正确）。
#
# 用法：manual_check_visual.sh [T0|T1|T2] [hold秒数]
# 环境：IP PORT FILE_MP4 BIN_DIR OUT_DIR RUNNER（与 run_multinode_composite.sh 同名变量互通）
# 非交互（stdin 非 tty）时只跑流取证并标记 UNWITNESSED，不伪造人工结论。
set -euo pipefail

STAGE="${1:-T2}"
HOLD="${2:-120}"
IP="${IP:-192.168.80.202}"
PORT="${PORT:-kernel:enP2p33s0}"
FILE_MP4="${FILE_MP4:-/usr/local/test.mp4}"
BIN_DIR="${BIN_DIR:-$HOME/code/mtl/bin}"
OUT_DIR="${OUT_DIR:-/tmp/multinode_composite}"
RUNNER="${RUNNER:-$(cd "$(dirname "$0")" && pwd)/run_multinode_composite.sh}"
mkdir -p "$OUT_DIR"
WITNESS="$OUT_DIR/manual_witness_${STAGE}_$(date +%Y%m%d_%H%M%S).log"

fail() { echo "FAIL: $*"; return 1; }
note() { echo "  $*"; }
need() { [ -x "$1" ] || { fail "缺 $1"; return 1; }; }

RUN_PID=""
cleanup_visual() {
  trap - INT TERM EXIT
  if [ -n "${RUN_PID:-}" ] && kill -0 "$RUN_PID" 2>/dev/null; then
    kill -INT "$RUN_PID" 2>/dev/null || true
    for _ in 1 2 3 4 5; do kill -0 "$RUN_PID" 2>/dev/null || break; sleep 1; done
    kill -9 "$RUN_PID" 2>/dev/null || true
  fi
  pkill -INT live_ingest 2>/dev/null || true
  pkill -INT file_playout_rx 2>/dev/null || true
  sleep 1
  pkill -9 live_ingest 2>/dev/null || true
  pkill -9 file_playout_rx 2>/dev/null || true
}
trap cleanup_visual INT TERM EXIT

case "$STAGE" in
  T0|T1|T2) ;;
  *) echo "用法：$0 [T0|T1|T2] [hold秒数]"; exit 1 ;;
esac
[ -x "$RUNNER" ] || fail "缺 runner $RUNNER"
[ -c /dev/video0 ] || fail "无 /dev/video0（HDMI 采集缺失）"
pgrep -f MtlManager >/dev/null || fail "MtlManager 未运行"

# 期望现象检查单（与验证报告 §8 目视清单同源）。
checklist() {
  echo "—— 看屏检查单（逐项回答 y/n，现象记入见证文件）——"
  case "$STAGE" in
    T0) echo "1. 全屏为 HDMI 输入的 1:1 内容，随输入变化而变化" ;;
    T1) echo "1. 全屏为文件片源内容（非 HDMI 输入），持续播放不卡死" ;;
    T2) echo "1. 主画面全屏为 HDMI 输入内容" ;
        echo "2. 右下 PiP（约 480x270）为文件片源内容，持续变化" ;;
  esac
  echo "3. 全程无黑场/花屏/撕裂/横向分块（至少连续观察 60 秒）"
  if [ "$STAGE" = "T1" ]; then
    echo "4. 显示器扬声器出声连续、无卡顿杂音（无发声设备则答 s 跳过并注明）"
  fi
}

ask() {
  # ask <序号> <描述>：返回 0=通过，1=不通过，2=跳过。
  local no="$1" desc="$2" ans
  while true; do
    printf "检查项 %s [%s] (y=通过/n=不通过/s=跳过): " "$no" "$desc"
    read -r ans || ans="n"
    case "$ans" in
      y|Y) return 0 ;;
      n|N) return 1 ;;
      s|S) return 2 ;;
      *) echo "请输入 y/n/s" ;;
    esac
  done
}

{
echo "witness: stage=$STAGE hold=${HOLD}s time=$(date -u +%FT%TZ)"
echo "env: IP=$IP PORT=$PORT MP4=$FILE_MP4"
} >"$WITNESS"

if [ ! -t 0 ]; then
  echo "非交互终端：只跑流取证，不做人工结论（标记 UNWITNESSED）。"
  echo "result: UNWITNESSED (no tty)" >>"$WITNESS"
  OUT_DIR="$OUT_DIR" IP="$IP" PORT="$PORT" FILE_MP4="$FILE_MP4" BIN_DIR="$BIN_DIR" \
    "$RUNNER" "$STAGE" "$HOLD" || true
  echo "检查单未执行，见 checklist()；机器证据见本轮 $STAGE 日志。" >>"$WITNESS"
  echo "见证文件：$WITNESS"
  exit 0
fi

note "后台起 $STAGE 场景（hold ${HOLD}s），进入稳态后开始逐项确认…"
OUT_DIR="$OUT_DIR" IP="$IP" PORT="$PORT" FILE_MP4="$FILE_MP4" BIN_DIR="$BIN_DIR" \
  "$RUNNER" "$STAGE" "$HOLD" >"$OUT_DIR/manual_${STAGE}_run.log" 2>&1 &
RUN_PID=$!
sleep 20
if ! kill -0 "$RUN_PID" 2>/dev/null; then
  tail -n 20 "$OUT_DIR/manual_${STAGE}_run.log"
  RUN_PID=""
  fail "$STAGE 场景启动失败"
  echo "result: SETUP_FAILED" >>"$WITNESS"
  exit 1
fi

checklist
overall=0
case "$STAGE" in
  T0) items="1:全屏内容为HDMI输入 3:无黑场花屏撕裂" ;;
  T1) items="1:全屏为文件片源内容 3:无黑场花屏撕裂 4:扬声器出声正常" ;;
  T2) items="1:主画面为HDMI输入 2:右下PiP为文件片源 3:无黑场花屏撕裂分块" ;;
esac
for it in $items; do
  no="${it%%:*}"; desc="${it#*:}"
  if ask "$no" "$desc"; then
    echo "item $no ($desc): PASS" >>"$WITNESS"
    echo "  → 通过"
  else
    rc=$?
    if [ "$rc" -eq 2 ]; then
      echo "item $no ($desc): SKIP" >>"$WITNESS"
      echo "  → 跳过（已记录）"
    else
      echo "item $no ($desc): FAIL" >>"$WITNESS"
      echo "  → 不通过"
      overall=1
    fi
  fi
done

echo "等待场景自然结束（剩余时间）…"
wait "$RUN_PID" || true
RUN_PID=""
trap - EXIT
{
echo "--- 机器证据（本轮 $STAGE 日志判定行） ---"
grep -aE "^(PASS|FAIL|INFO)" "$OUT_DIR/manual_${STAGE}_run.log" | tail -n 20 || true
if [ "$overall" -eq 0 ]; then echo "result: WITNESSED_PASS"; else echo "result: WITNESSED_FAIL"; fi
} >>"$WITNESS"
echo "见证文件：$WITNESS"
exit "$overall"
