#!/bin/bash
# v4l2_stream_check.sh — HDMI RX 采集可用性一键检查 (板端执行)
#
# 按序执行: 输入状态 -> 时序快照 -> 试采 -> dmesg 定责, 退出码说明结论:
#   0 = 可采 (能跑采集验证); 1 = 格式被拒; 2 = 时序/停流/无信号 (源端问题, 稍后重试):
#   3 = 设备忙或不可用。
# 长命令全部收敛在本脚本, 文档只引用脚本不贴长命令。
set -u

DEV="${DEV:-/dev/video0}"
COUNT="${1:-30}"
RETRIES="${2:-3}"
LOG="${3:-}"

say() { echo "[v4l2check] $*"; }
log() { [ -n "$LOG" ] && echo "$*" >> "$LOG"; return 0; }

[ -e "$DEV" ] || { say "FAIL: $DEV 不存在"; exit 3; }
fuser "$DEV" >/dev/null 2>&1 && { say "FAIL: $DEV 被占用"; exit 3; }

say "输入状态:"
v4l2-ctl -d "$DEV" --list-inputs 2>&1 | grep -E "Input|Status"
# 物理连接与控制器状态 (路径为本板 rk_hdmirx, 缺失则跳过)
HDMIRX_SYS="/sys/bus/platform/devices/fdee0000.hdmirx-controller"
[ -r "$HDMIRX_SYS/extcon/extcon7/state" ] \
  && say "cable $(cat $HDMIRX_SYS/extcon/extcon7/state 2>/dev/null)"
[ -r "$HDMIRX_SYS/hdmirx/hdmirx/status" ] \
  && say "hdmirx status=$(cat $HDMIRX_SYS/hdmirx/hdmirx/status 2>/dev/null)"
# 锁存门: query-dv-timings 的退出码即有无信号, 比快照文本更可信
if ! QTIMS=$(v4l2-ctl -d "$DEV" --query-dv-timings 2>&1); then
  say "FAIL: 无信号 (QUERY_DV_TIMINGS 未锁存)"
  echo "$QTIMS" | grep -E "Active|Pixelclock" | head -3
  log "no-lock: [$QTIMS]"
  exit 2
fi
say "时序快照 (已锁存):"
echo "$QTIMS" | grep -E "Active|Pixelclock|Frame format"

n=0
while [ "$n" -lt "$RETRIES" ]; do
  n=$((n + 1))
  say "试采 $n/$RETRIES ..."
  MARK=$(dmesg | wc -l)
  FMT_OUT=$(v4l2-ctl -d "$DEV" \
    --set-fmt-video=width=1920,height=1080,pixelformat=BGR3 2>&1)
  say "S_FMT: ${FMT_OUT:-ok}"
  log "== try $n sfmt=[$FMT_OUT]"
  OUT=$(v4l2-ctl -d "$DEV" --stream-mmap --stream-count="$COUNT" 2>&1)
  RC=$?
  say "stream rc=$RC ${OUT:+out=$OUT}"
  log "== try $n stream rc=$RC out=[$OUT]"
  dmesg | tail -n +"$MARK" > /tmp/v4l2check_dmesg.txt
  log "$(cat /tmp/v4l2check_dmesg.txt)"
  # v4l2-ctl 打印 VIDIOC 错误时仍可能退出 0, 以输出文本为准 (大小写不敏感)
  if ! echo "$OUT $FMT_OUT" | grep -qiE "failed|error"; then
    say "PASS: 试采成功"
    grep -E "start_streaming|rcv frames" /tmp/v4l2check_dmesg.txt | tail -2
    exit 0
  fi
  if grep -q "timing is invalid" /tmp/v4l2check_dmesg.txt; then
    say "第 $n 次: timing invalid (源端问题), 10s 后重试"
  else
    say "第 $n 次失败, dmesg:"
    tail -5 /tmp/v4l2check_dmesg.txt
  fi
  [ "$n" -lt "$RETRIES" ] && sleep 10
done

say "FAIL: $RETRIES 次试采均失败 (源端/驱动, 非采集程序问题)"
exit 2
