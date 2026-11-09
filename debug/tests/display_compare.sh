#!/bin/bash
# 显示 A/B 对比：A=HDMI直通，B=完整链路。板端运行，看屏记录。
# 前置：~/code/mtl/bin/ 下有 hdmi_passthrough、file_playout_rx、
# live_ingest、run_tx_rx_e2e.sh（缺谁会直接告诉你怎么补）。
set -euo pipefail

BIN="$HOME/code/mtl/bin"
PASS_SEC="${PASS_SEC:-20}"
CHAIN_SEC="${CHAIN_SEC:-30}"

need() { [ -x "$BIN/$1" ] || { echo "缺 $BIN/$1"; echo "补：$2"; exit 1; }; }
need hdmi_passthrough "gcc $HOME/code/mtl/debug/tests/hdmi_passthrough.c -o $BIN/hdmi_passthrough -I/usr/include/libdrm -ldrm"
need file_playout_rx "交叉编译 app/rk3588_playout_rx 后同步二进制"
need live_ingest "交叉编译 app/rk3588_live_tx 后同步二进制"
need run_tx_rx_e2e.sh "同步 app/rk3588_playout_rx/tests/run_tx_rx_e2e.sh"

echo "=== A: HDMI直通 ${PASS_SEC}s（采集直接送显，无编解码无网络）==="
echo "看屏：应为 HDMI-IN 的 1:1 原样，有问题记下来源侧。"
"$BIN/hdmi_passthrough" --duration "$PASS_SEC"
echo ""
echo "A 结束。记下所见（正常/黑场/分块/撕裂）：________"
echo "按回车跑 B（完整链路）…"; read -r _

echo "=== B: 完整链路 ${CHAIN_SEC}s（采集→编码→ST2110→解码→送显）==="
"$BIN/run_tx_rx_e2e.sh" "$CHAIN_SEC" || true

echo ""
echo "=== 对比结论（看屏填写）==="
echo "A 直通：正常 / 异常（现象：________）"
echo "B 链路：正常 / 异常（现象：________）"
echo "A正常+B异常 → 查编码/网络/解码/呈现；A同样异常 → 查采集源/驱动/显示。"
