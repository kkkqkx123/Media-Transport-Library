#!/bin/bash
# TX/RX 单板正式验证：live_ingest(HDMI) + file_playout_rx 同板互通。
# 位置：app/rk3588_playout_rx/tests/；板端拷贝到 ~/code/mtl/bin 后直接运行。
# 不依赖仓库路径，只依赖板端二进制与标准工具。
#
# 用法：run_tx_rx_e2e.sh [秒数]
# 环境覆盖：SEC PORT IP UDP W H FPS PTP_DELAY
set -euo pipefail

SEC="${1:-${SEC:-60}}"
PORT="${PORT:-kernel:enP2p33s0}"
IP="${IP:-192.168.80.202}"
UDP="${UDP:-15000}"
W="${W:-1920}"; H="${H:-1080}"; FPS="${FPS:-60}"
BIN_DIR="${BIN_DIR:-$HOME/code/mtl/bin}"
RX_BIN="$BIN_DIR/file_playout_rx"
TX_BIN="$BIN_DIR/live_ingest"
RX_LOG="${RX_LOG:-/tmp/tx_rx_e2e_rx.log}"
TX_LOG="${TX_LOG:-/tmp/tx_rx_e2e_tx.log}"
START_DELAY=3

fail() { echo "FAIL: $*"; exit 1; }
note() { echo "  $*"; }

[ -x "$RX_BIN" ] || fail "缺 $RX_BIN"
[ -x "$TX_BIN" ] || fail "缺 $TX_BIN"
[ -c /dev/video0 ] || fail "无 /dev/video0（HDMI 采集缺失）"
v4l2-ctl -d /dev/video0 --get-dv-timings 2>/dev/null | grep -q "1920" \
  || echo "WARN: DV 时序非 1080p，以宽高帧率显式对齐继续"

if ! pgrep -f "MtlManager" >/dev/null 2>&1; then
  echo "起 MtlManager（后台）..."
  nohup /usr/local/bin/MtlManager >/tmp/mtlmanager2.log 2>&1 &
  sleep 2
fi
pgrep -f "MtlManager" >/dev/null || fail "MtlManager 未运行"

pkill -9 live_ingest 2>/dev/null || true
pkill -9 file_playout_rx 2>/dev/null || true
sleep 1
rm -f "$RX_LOG" "$TX_LOG"

echo "=== RX 起 ($SEC s, $PORT lip=$IP sip=$IP udp=$UDP ${W}x${H}@${FPS}) ==="
nohup "$RX_BIN" "$SEC" --port "$PORT" --lip "$IP" --sip "$IP" \
  --udp "$UDP" --width "$W" --height "$H" --fps "$FPS" \
  --ptp-mode monotonic --stats-interval 10 >"$RX_LOG" 2>&1 &
RX_PID=$!
sleep "$START_DELAY"
if ! kill -0 "$RX_PID" 2>/dev/null; then
  tail -n 20 "$RX_LOG"; fail "RX 启动失败（MTL start failed？先查 MtlManager 与 shm）"
fi

echo "=== TX 起 ($SEC s, HDMI, dip=$IP udp=$UDP) ==="
"$TX_BIN" "$SEC" --port "$PORT" --lip "$IP" --dip "$IP" --udp "$UDP" \
  --pacing tsc_narrow --log-level warning >"$TX_LOG" 2>&1
echo "TX 退出=$?"
wait "$RX_PID" || true

echo "=== 判定 ==="
TX_SUM="$(grep -a "tx_pipe:" "$TX_LOG" | tail -n 1)"
RX_SUM="$(grep -a "rx_pipe:" "$RX_LOG" | tail -n 1)"
echo "TX: $TX_SUM"
echo "RX: $RX_SUM"
[ -n "$TX_SUM" ] || fail "无 TX 汇总行"
[ -n "$RX_SUM" ] || fail "无 RX 汇总行"

python3 - "$RX_LOG" "$TX_LOG" <<'PY'
import re, sys
rx_log = open(sys.argv[1], errors="replace").read()
tx_log = open(sys.argv[2], errors="replace").read()
ok = True
def check(name, cond, detail=""):
    global ok
    print(("PASS" if cond else "FAIL") + f": {name} {detail}")
    if not cond: ok = False

m = re.search(r"rx_pipe: (\d+) pkt, (\d+) dec.*?(\d+) pb_push, (\d+) render.*?pb_overrun=(\d+) pb_repeat=(\d+) pb_underrun=(\d+), (\d+) put_fail", rx_log, re.S)
if not m:
    print("FAIL: RX 汇总行解析失败"); sys.exit(1)
pkt, dec, push, render, overrun, repeat, under, putfail = map(int, m.groups())
check("收流等速 dec/pkt>=0.99", pkt > 0 and dec / pkt >= 0.99, f"{dec}/{pkt}={dec/max(pkt,1)*100:.1f}%")
check("收流零丢包", "mtl_drop=+0" in rx_log and "br_drop=+0" in rx_log)
check("零坏帧", "corrupt=0" in rx_log and "incomplete=0" in rx_log)
check("解码无反压", putfail == 0, f"put_fail={putfail}")
mm = re.search(r"(\d+) captured, (\d+) encoded", tx_log)
if mm:
    cap, enc = int(mm.group(1)), int(mm.group(2))
    check("发送跟上帧率 enc/cap>=0.95", cap > 0 and enc / cap >= 0.95, f"{enc}/{cap}")
else:
    print("WARN: 无 TX captured/encoded 行"); ok = False
# 稳态欠载：首个 10s 窗口后不应再增长（启动排空属正常）
totals = re.findall(r"total: pkt=\d+ dec=\d+ render=\d+ pb_skip=\d+ pb_underrun=(\d+)", rx_log)
if len(totals) >= 2 and int(totals[0]) > 0:
    check("稳态欠载收敛", int(totals[-1]) - int(totals[0]) <= max(10, int(totals[0]) * 0.05),
          f"underrun {totals[0]}->{totals[-1]}")
print("INFO: render/dec=%.1f%%（latest 帧策略跳帧属预期，只看趋势）" % (render / max(dec, 1) * 100))
sys.exit(0 if ok else 1)
PY
RC=$?
echo ""
echo "人工确认（显示器旁）："
echo "  1) HDMI OUT 有连续画面、无黑屏/花屏/撕裂"
echo "  2) 画面内容随 HDMI 输入变化（摆动镜头或切信号可见）"
echo "  3) 对比 TX bps 与画面复杂度变化方向一致"
[ "$RC" -eq 0 ] && echo "OVERALL: PASS" || echo "OVERALL: FAIL"
exit "$RC"
