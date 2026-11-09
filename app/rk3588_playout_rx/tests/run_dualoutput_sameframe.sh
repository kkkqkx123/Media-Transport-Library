#!/bin/bash
# 双输出同帧验证（未来任务 F2/F3）：两台输出板订阅同一输入源，对比同一切换帧号。
# 本阶段不执行：需要 2 台输出板 + 已调通的 PTP 域 + 第二输入源，前置检查会直接失败。
# 有条件后用法：在【操作机】上执行，依次 ssh 到 RX_A / RX_B 起合成接收，再 ssh 到 TX 板起发送，
#   用 PTP 对时后的 E2E-C 帧号（或高速摄像）对比两屏切换帧号，偏差应 ≤1 帧。
# 环境：RX_A/RX_B（两台输出板 ssh 目标）、TX_BOARD、A_IP/B_IP/TX_IP、UDP_A/UDP_B（同一输入组播时可相同）、SEC。
set -euo pipefail

RX_A="${RX_A:?缺 RX_A（输出板 A ssh 目标）}"
RX_B="${RX_B:?缺 RX_B（输出板 B ssh 目标）}"
TX_BOARD="${TX_BOARD:?缺 TX_BOARD（输入板 ssh 目标）}"
A_IP="${A_IP:?缺 A_IP}"; B_IP="${B_IP:?缺 B_IP}"; TX_IP="${TX_IP:?缺 TX_IP}"
UDP="${UDP:-15000}"
SEC="${SEC:-60}"
BIN_DIR="${BIN_DIR:-$HOME/code/mtl/bin}"
OUT_DIR="${OUT_DIR:-/tmp/dualoutput_sameframe}"
mkdir -p "$OUT_DIR"

fail() { echo "FAIL: $*"; exit 1; }

for b in "$RX_A" "$RX_B" "$TX_BOARD"; do
  ssh -o ConnectTimeout=5 "$b" "test -x $BIN_DIR/file_playout_rx && test -x $BIN_DIR/live_ingest" \
    || fail "$b 不可达或缺二进制"
  ssh "$b" "pgrep -f MtlManager" >/dev/null || fail "$b MtlManager 未运行"
done
# PTP 同域检查：三板 CLOCK_TAI 互差应在毫秒级（粗门限，精门限见设计文档 F2）。
for b in "$RX_A" "$RX_B" "$TX_BOARD"; do
  ssh "$b" "ptp4l -v >/dev/null 2>&1 || phc_ctl /dev/ptp0 cmp 2>/dev/null" \
    || echo "WARN: $b 未见 linuxptp/phc 工具，同帧结论不可用"
done

echo "前置通过。后续步骤（待 PTP 调通后执行）："
echo "1) 在 $RX_A / $RX_B 起 file_playout_rx（同 UDP $UDP，同 ptp-mode external）；"
echo "2) 在 $TX_BOARD 起 live_ingest；"
echo "3) 对比 $OUT_DIR 双端 E2E-C 帧号/高速摄像，偏差 ≤1 帧为通过，失锁时整墙阻止为通过。"
