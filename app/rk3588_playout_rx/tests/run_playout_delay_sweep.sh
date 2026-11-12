#!/bin/bash
# playout_delay 人工扫参：单板回环下逐档切换 RX 播出等待，人眼判定跳帧可接受性。
# 位置：app/rk3588_playout_rx/tests/；板端同步到 ~/code/mtl/src/playout-tests/ 并拷可执行副本进 ~/code/mtl/bin/ 后直接运行。
# 效仿 run_tx_rx_e2e.sh（单板回环判定）+ manual_check_visual.sh（逐项 y/n/s + 见证文件）。
# 不依赖仓库路径，只依赖板端二进制与标准工具（bash/python3）。
#
# 用法：run_playout_delay_sweep.sh ["80 20 8 2 0"] [每档秒数] [hdmi|file]
# 环境覆盖：SEC PORT IP UDP W H FPS CODEC TX_SRC FILE_MP4 OUT_DIR BIN_DIR OBSERVE
# 注：TX 源以 $3 为准（$3 未给才看 TX_SRC 环境），避免经 run -- 包裹时环境未透传导致静默跑错源。
#   TX_SRC=hdmi|file（默认 hdmi；file 用 testsrc 有声片源，运动连续更适合看抖动）
#   FILE_MP4 默认同 manual_check_visual.sh（streams/ 受管有声片源）
#   OBSERVE 为每档起播后进入提问前的观察秒数（默认 15，需 < SEC）
# 板端同步示例：
#   scp app/rk3588_playout_rx/tests/run_playout_delay_sweep.sh board:~/code/mtl/src/playout-tests/
#   ssh board "cp ~/code/mtl/src/playout-tests/run_playout_delay_sweep.sh ~/code/mtl/bin/ && chmod +x ~/code/mtl/bin/run_playout_delay_sweep.sh"
set -euo pipefail

DELAYS="${1:-${DELAYS:-80 20 8 2 0}}"
SEC="${2:-${SEC:-40}}"
TX_SRC="${3:-${TX_SRC:-hdmi}}"
PORT="${PORT:-kernel:enP2p33s0}"
IP="${IP:-192.168.80.202}"
UDP="${UDP:-15000}"
W="${W:-1920}"; H="${H:-1080}"
if [ "$TX_SRC" = "file" ]; then FPS_DEFAULT=30; else FPS_DEFAULT=60; fi
FPS="${FPS:-$FPS_DEFAULT}"
CODEC="${CODEC:-h264}"
case "$CODEC" in h264|h265) ;; *) echo "FAIL: bad CODEC=$CODEC"; exit 1 ;; esac
case "$TX_SRC" in hdmi|file) ;; *) echo "FAIL: bad TX_SRC=$TX_SRC (hdmi|file)"; exit 1 ;; esac
FILE_MP4="${FILE_MP4:-$HOME/code/mtl/streams/multinode_av_1080p30.mp4}"
BIN_DIR="${BIN_DIR:-$HOME/code/mtl/bin}"
OUT_DIR="${OUT_DIR:-/tmp/playout_sweep_${TX_SRC}}"
OBSERVE="${OBSERVE:-15}"
RX_BIN="$BIN_DIR/file_playout_rx"
TX_BIN="$BIN_DIR/live_ingest"
START_DELAY=3
mkdir -p "$OUT_DIR"
WITNESS="$OUT_DIR/playout_sweep_witness_$(date +%Y%m%d_%H%M%S).log"

fail() { echo "FAIL: $*"; exit 1; }
note() { echo "  $*"; }

RX_PID=""; TX_PID=""
cleanup_pair() {
  if [ -n "${RX_PID:-}" ] && kill -0 "$RX_PID" 2>/dev/null; then
    kill -INT "$RX_PID" 2>/dev/null || true
    for _ in 1 2 3 4 5; do kill -0 "$RX_PID" 2>/dev/null || break; sleep 1; done
    kill -9 "$RX_PID" 2>/dev/null || true
  fi
  RX_PID=""
  if [ -n "${TX_PID:-}" ] && kill -0 "$TX_PID" 2>/dev/null; then
    kill -INT "$TX_PID" 2>/dev/null || true
    for _ in 1 2 3; do kill -0 "$TX_PID" 2>/dev/null || break; sleep 1; done
    kill -9 "$TX_PID" 2>/dev/null || true
  fi
  TX_PID=""
  pkill -INT live_ingest 2>/dev/null || true
  pkill -INT file_playout_rx 2>/dev/null || true
  sleep 1
  pkill -9 live_ingest 2>/dev/null || true
  pkill -9 file_playout_rx 2>/dev/null || true
}
trap cleanup_pair INT TERM EXIT

preflight() {
  [ -x "$RX_BIN" ] || fail "缺 $RX_BIN"
  [ -x "$TX_BIN" ] || fail "缺 $TX_BIN"
  BOARD_ENV="${BOARD_ENV:-$BIN_DIR/board_env.sh}"
  [ -x "$BOARD_ENV" ] || fail "缺 $BOARD_ENV（与本脚本同批同步到 bin/）"
  if [ "$TX_SRC" = "file" ]; then
    [ -f "$FILE_MP4" ] || fail "缺片源 $FILE_MP4（先跑 gen_av_file_source.sh，或 TX_SRC=hdmi）"
    SCOPE="display,net"
  else
    SCOPE="display,capture,net"
  fi
  # shellcheck disable=SC2086
  ENV_UDP_PORTS="$UDP" "$BOARD_ENV" status $SCOPE --quiet \
    || fail "测试环境未就绪（$BOARD_ENV status 查阻塞项，或 $BOARD_ENV run -- $0 \"$DELAYS\" $SEC 一条龙）"
  if ! pgrep -f "MtlManager" >/dev/null 2>&1; then
    echo "起 MtlManager（后台）..."
    nohup /usr/local/bin/MtlManager >/tmp/mtlmanager2.log 2>&1 &
    sleep 2
  fi
  pgrep -f "MtlManager" >/dev/null || fail "MtlManager 未运行"
  cleanup_pair
  sleep 1
}

ask() {
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

summarize() {
  python3 - "$1" "$2" "$3" <<'PY'
import re, sys
rx_log = open(sys.argv[1], errors="replace").read()
tx_log = open(sys.argv[2], errors="replace").read()
delay = sys.argv[3]
m = re.search(r"rx_pipe: (\d+) pkt, (\d+) dec.*?(\d+) pb_push, (\d+) render.*?pb_skip=(\d+) pb_overrun=(\d+) pb_repeat=(\d+) pb_underrun=(\d+), (\d+) put_fail", rx_log, re.S)
if not m:
    print(f"delay={delay}ms NO_RX_SUMMARY"); sys.exit(0)
pkt, dec, push, render, skip, overrun, repeat, under, putfail = map(int, m.groups())
totals = re.findall(r"total: pkt=\d+ dec=\d+ render=\d+ pb_skip=\d+ pb_underrun=(\d+)", rx_log)
ugrow = f"{totals[0]}->{totals[-1]}" if len(totals) >= 2 else str(under)
skipr = skip / max(dec, 1) * 100
renr = render / max(dec, 1) * 100
mm = re.search(r"(\d+) captured, (\d+) encoded", tx_log)
if mm:
    txs = f"{mm.group(2)}/{mm.group(1)}"
else:
    mf = re.search(r"video exit \((\d+) frames", tx_log)
    txs = f"file:{mf.group(1)}f" if mf else "n/a"
em = re.search(r"e2e total n=\d+ min=(\d+) avg=(\d+) max=(\d+) us", rx_log)
e2e = f"{em.group(2)}us(avg)" if em else "n/a"
verdict = "OK" if (pkt > 0 and dec / pkt >= 0.99 and putfail == 0 and overrun == 0) else "CHECK"
# 稳态欠载 + 跳帧率联合门限：只做提示，不做 PASS/FAIL（人眼结论为准）
if len(totals) >= 2 and int(totals[-1]) - int(totals[0]) > max(10, int(totals[0]) * 0.05):
    verdict = "UNDERRUN_GROW"
if skipr > 5.0:
    verdict = "SKIP_HIGH"
print(f"delay={delay}ms tx={txs} dec/pkt={dec}/{pkt} render/dec={renr:.1f}% "
      f"skip={skip}({skipr:.1f}%) under={under}(grow {ugrow}) overrun={overrun} "
      f"repeat={repeat} put_fail={putfail} e2e={e2e} => {verdict}")
PY
}

preflight
{
echo "witness: delays=\"$DELAYS\" sec=$SEC tx_src=$TX_SRC fps=$FPS codec=$CODEC time=$(date -u +%FT%TZ)"
echo "env: IP=$IP PORT=$PORT UDP=$UDP MP4=$FILE_MP4"
} >"$WITNESS"
TTY=0; [ -t 0 ] && TTY=1
[ "$TTY" -eq 0 ] && echo "非交互终端：只跑流取证，不做人工结论（标记 UNWITNESSED）。"

echo "=== playout_delay 扫参：$DELAYS（每档 ${SEC}s，TX_SRC=$TX_SRC）==="
for D in $DELAYS; do
  case "$D" in ''|*[!0-9]*) fail "bad delay '$D'" ;; esac
  RX_LOG="$OUT_DIR/delay_${D}ms_rx.log"
  TX_LOG="$OUT_DIR/delay_${D}ms_tx.log"
  CFG_JSON="$(mktemp /tmp/playout_delay_${D}ms_XXXXXX.json)"
  printf '{"playout_delay_ms": %d}\n' "$D" >"$CFG_JSON"
  rm -f "$RX_LOG" "$TX_LOG"

  echo ""
  echo "=== 档位 delay=${D}ms TX_SRC=${TX_SRC}（RX --config 只覆写 playout_delay_ms，其余走 CLI）==="
  # shellcheck disable=SC2086
  nohup stdbuf -oL -eL "$RX_BIN" "$SEC" --config "$CFG_JSON" --port "$PORT" --lip "$IP" --sip "$IP" \
    --udp "$UDP" --width "$W" --height "$H" --fps "$FPS" --codec "$CODEC" \
    --ptp-mode monotonic --stats-interval 10 >"$RX_LOG" 2>&1 &
  RX_PID=$!
  sleep "$START_DELAY"
  if ! kill -0 "$RX_PID" 2>/dev/null; then
    tail -n 20 "$RX_LOG"; RX_PID=""
    echo "档位 ${D}ms: RX 启动失败，跳过（见 $RX_LOG）" | tee -a "$WITNESS"
    rm -f "$CFG_JSON"; cleanup_pair; continue
  fi

  if [ "$TX_SRC" = "file" ]; then
    # shellcheck disable=SC2086
    timeout -s INT $((SEC + 60)) stdbuf -oL -eL "$TX_BIN" "$SEC" --source file --mp4 "$FILE_MP4" \
      --port "$PORT" --lip "$IP" --dip "$IP" --udp "$UDP" \
      --codec "$CODEC" --pacing tsc_narrow --log-level warning >"$TX_LOG" 2>&1 &
  else
    # shellcheck disable=SC2086
    timeout -s INT $((SEC + 60)) stdbuf -oL -eL "$TX_BIN" "$SEC" --port "$PORT" --lip "$IP" --dip "$IP" --udp "$UDP" \
      --codec "$CODEC" --pacing tsc_narrow --log-level warning >"$TX_LOG" 2>&1 &
  fi
  TX_PID=$!

  # 防呆：轮询等待 TX 源标记（MTL 初始化加 probe 要十几秒），超时或 TX 退出仍无标记才跳过本档。
  if [ "$TX_SRC" = "file" ]; then want_src="file_source:"; else want_src="v4l2_capturer"; fi
  for _ in $(seq 1 20); do
    grep -qa "$want_src" "$TX_LOG" 2>/dev/null && break
    kill -0 "$TX_PID" 2>/dev/null || break
    sleep 1
  done
  if ! grep -qa "$want_src" "$TX_LOG" 2>/dev/null; then
    echo "档位 ${D}ms: TX 源断言失败（want $want_src），跳过（见 $TX_LOG）" | tee -a "$WITNESS"
    cleanup_pair; sleep 1; rm -f "$CFG_JSON"; continue
  fi

  if [ "$TTY" -eq 1 ]; then
    echo "看屏 ${OBSERVE}s 后逐项确认（流仍在跑，显示器接板载 HDMI 输出）…"
    sleep "$OBSERVE"
    if ! kill -0 "$RX_PID" 2>/dev/null; then
      echo "RX 已提前退出，见 $RX_LOG"
    fi
    echo "—— delay=${D}ms 看屏检查单 ——"
    echo "1. 有连续画面、无超过半秒的冻结"
    echo "2. 无黑场/花屏/撕裂/横向分块（一票否决项）"
    echo "3. 运动平滑可接受（自然内容几乎无感；testsrc/摇镜轻微抖动可注明接受）"
    overall=0
    for it in "1:连续画面无久冻" "2:无黑场花屏撕裂" "3:运动平滑可接受"; do
      no="${it%%:*}"; desc="${it#*:}"
      if ask "$no" "$desc"; then
        echo "delay ${D}ms item $no ($desc): PASS" >>"$WITNESS"
        echo "  → 通过"
      else
        rc=$?
        if [ "$rc" -eq 2 ]; then
          echo "delay ${D}ms item $no ($desc): SKIP" >>"$WITNESS"
          echo "  → 跳过（已记录）"
        else
          echo "delay ${D}ms item $no ($desc): FAIL" >>"$WITNESS"
          echo "  → 不通过"
          overall=1
        fi
      fi
    done
    if [ "$overall" -eq 0 ]; then echo "delay ${D}ms result: WITNESSED_PASS" >>"$WITNESS"
    else echo "delay ${D}ms result: WITNESSED_FAIL" >>"$WITNESS"; fi
    printf "备注（可空，回车跳过）: "; read -r cm || cm=""
    [ -n "$cm" ] && echo "delay ${D}ms note: $cm" >>"$WITNESS"
  else
    echo "delay ${D}ms result: UNWITNESSED (no tty)" >>"$WITNESS"
  fi

  echo "等待本档自然结束（剩余时间）…"
  wait "$TX_PID" 2>/dev/null || true
  TX_PID=""
  if [ -n "$RX_PID" ]; then
    kill -INT "$RX_PID" 2>/dev/null || true
    for _ in 1 2 3 4 5; do kill -0 "$RX_PID" 2>/dev/null || break; sleep 1; done
    kill -9 "$RX_PID" 2>/dev/null || true
    wait "$RX_PID" 2>/dev/null || true
    RX_PID=""
  fi
  rm -f "$CFG_JSON"
  pkill -9 live_ingest 2>/dev/null || true
  pkill -9 file_playout_rx 2>/dev/null || true
  sleep 2
  summarize "$RX_LOG" "$TX_LOG" "$D" | tee -a "$WITNESS"
done

trap - INT TERM EXIT
echo ""
echo "=== 汇总（机器行，人眼结论以见证文件为准） ==="
grep -a "^delay=" "$WITNESS" || true
echo "见证文件：$WITNESS"
echo "判定参考：skip/dec<=1% 且欠载稳态收敛 → 自然内容通常可接受；1~5% 偶发可察（看用途）；>5% 或欠载持续增长/overrun>0 → 不可接受；黑场/花屏/撕裂一票否决。"
