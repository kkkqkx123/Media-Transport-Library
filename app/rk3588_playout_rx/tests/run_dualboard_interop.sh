#!/bin/bash
# 双板互通验证（未来任务 F1）：一板只发（TX 板），一板只收（RX 板），经网线或交换机互通。
# 本阶段不执行：当前只有 1 台开发板，前置检查会直接失败并提示缺什么。
# 有第二台板后用法：在【RX 板】上执行本脚本，它会 ssh 到 TX 板起发送，两侧日志分别留存后统一判定。
# 环境：TX_BOARD（TX 板 ssh 名或 IP）、RX_IF/RX_IP（本机网卡与地址）、TX_IF/TX_IP、SEC W H FPS UDP。
set -euo pipefail

TX_BOARD="${TX_BOARD:?缺 TX_BOARD（对端 TX 板 ssh 目标）}"
SEC="${SEC:-60}"
PORT_TX="${PORT_TX:-kernel:enP2p33s0}"
PORT_RX="${PORT_RX:-kernel:enP2p33s0}"
TX_IP="${TX_IP:?缺 TX_IP（TX 板网卡地址）}"
RX_IP="${RX_IP:?缺 RX_IP（本机 RX 网卡地址）}"
UDP="${UDP:-15000}"
W="${W:-1920}"; H="${H:-1080}"; FPS="${FPS:-60}"
BIN_DIR="${BIN_DIR:-$HOME/code/mtl/bin}"
OUT_DIR="${OUT_DIR:-/tmp/dualboard_interop}"
mkdir -p "$OUT_DIR"

fail() { echo "FAIL: $*"; exit 1; }

[ -x "$BIN_DIR/file_playout_rx" ] || fail "本机缺 $BIN_DIR/file_playout_rx"
ssh -o ConnectTimeout=5 "$TX_BOARD" "test -x $BIN_DIR/live_ingest" || fail "TX 板缺 live_ingest 或不可达"
ping -c 3 -W 2 "$TX_IP" >/dev/null || fail "TX 板 $TX_IP 不可达"
pgrep -f MtlManager >/dev/null || fail "本机 MtlManager 未运行"
ssh "$TX_BOARD" "pgrep -f MtlManager" >/dev/null || fail "TX 板 MtlManager 未运行"

RX_LOG="$OUT_DIR/rx.log"; TX_LOG="$OUT_DIR/tx.log"
rm -f "$RX_LOG" "$TX_LOG"
echo "=== RX 起（本机 $RX_IP 监听 TX 板 $TX_IP）==="
nohup "$BIN_DIR/file_playout_rx" "$SEC" --port "$PORT_RX" --lip "$RX_IP" --sip "$TX_IP" \
  --udp "$UDP" --width "$W" --height "$H" --fps "$FPS" \
  --ptp-mode monotonic --stats-interval 10 >"$RX_LOG" 2>&1 &
RX_PID=$!
sleep 3
kill -0 "$RX_PID" 2>/dev/null || { tail -n 20 "$RX_LOG"; fail "RX 启动失败"; }
echo "=== TX 起（$TX_BOARD）==="
ssh "$TX_BOARD" "$BIN_DIR/live_ingest $SEC --port $PORT_TX --lip $TX_IP --dip $RX_IP \
  --udp $UDP --pacing tsc_narrow --log-level warning" >"$TX_LOG" 2>&1
wait "$RX_PID" || true
echo "=== 判定（口径同单板 run_tx_rx_e2e.sh，另看跨机丢包）==="
grep -a "rx_pipe:" "$RX_LOG" | tail -n 1 || fail "无 RX 汇总行"
grep -aE "captured, .* encoded" "$TX_LOG" | tail -n 1 || fail "无 TX 汇总行"
