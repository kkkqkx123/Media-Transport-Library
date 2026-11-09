#!/bin/bash
# 单盒模拟多节点合成输出验证：2 TX 进程模拟 2 个输入节点，1 个合成 RX 模拟 1 个输出节点。
# 位置：app/rk3588_playout_rx/tests/；板端运行时把本文件拷到板上执行。
# 所需 JSON 配置由本脚本按环境变量在 $OUT_DIR 内生成，不依赖外部配置文件。
# 不依赖仓库路径，只依赖板端二进制与标准工具。只追加新文件，不改动既有脚本与生产代码。
#
# 阶段：
#   T0 基线：HDMI 单路回环（沿用 run_tx_rx_e2e.sh 判定口径，短时）。
#   T1 虚拟输入单路：文件帧源单发单收，验证虚拟节点链路与 ST30/ALSA 音频分支。
#   T2 双路合成：HDMI TX + 文件 TX 并发，对 2-lane compositor，验证合成输出。
#   T3 反例：非法多流配置（重复 sip:port、无 main）必须被配置校验明确拒绝。
#
# 用法：run_multinode_composite.sh [T0|T1|T2|T3|all] [秒数]
# 环境覆盖：IP PORT SEC FILE_MP4 BIN_DIR OUT_DIR UDP_HDMI UDP_FILE
set -euo pipefail

WHICH="${1:-all}"
SEC="${2:-${SEC:-40}}"
IP="${IP:-192.168.80.202}"
PORT="${PORT:-kernel:enP2p33s0}"
FILE_MP4="${FILE_MP4:-/usr/local/test.mp4}"
BIN_DIR="${BIN_DIR:-$HOME/code/mtl/bin}"
RX_BIN="$BIN_DIR/file_playout_rx"
TX_BIN="$BIN_DIR/live_ingest"
CFG_2LANE="$OUT_DIR/multinode_2lane.json"
UDP_HDMI="${UDP_HDMI:-15000}"
UDP_FILE="${UDP_FILE:-15010}"
AUDIO_FILE="${AUDIO_FILE:-15012}"
export UDP_HDMI UDP_FILE
OUT_DIR="${OUT_DIR:-/tmp/multinode_composite}"
mkdir -p "$OUT_DIR"

fail() { echo "FAIL: $*"; return 1; }
note() { echo "  $*"; }

preflight() {
  [ -x "$RX_BIN" ] || { fail "缺 $RX_BIN"; return 1; }
  [ -x "$TX_BIN" ] || { fail "缺 $TX_BIN"; return 1; }
  [ -c /dev/video0 ] || { fail "无 /dev/video0（HDMI 采集缺失）"; return 1; }
  [ -f "$FILE_MP4" ] || { fail "缺文件片源 $FILE_MP4"; return 1; }
  pgrep -f MtlManager >/dev/null || { fail "MtlManager 未运行"; return 1; }
  return 0
}

# HDMI 源稳定性门禁（见 docs/issue/RK3588_HDMIIN_SOURCE_INSTABILITY.md）：
# 近 5 分钟内驱动报过 timing invalid / signal not lock，说明源在抖动，此时开测必被截断。
hdmi_stable_check() {
  local now last_ts age
  now=$(awk '{print int($1)}' /proc/uptime)
  last_ts=$(dmesg 2>/dev/null | grep -aE 'timing is invalid|signal not lock' | tail -n 1 \
    | sed -n 's/^\[ *\([0-9]*\)\..*/\1/p')
  [ -z "$last_ts" ] && return 0
  age=$((now - last_ts))
  if [ "$age" -lt 300 ]; then
    fail "HDMI 源 ${age}s 前刚抖动（dmesg timing invalid/signal not lock），先按 issue 文档 §3 预检源再跑"
    return 1
  fi
  return 0
}

# 生成本轮 JSON：合成 RX 的 2-lane 描述 + 文件 TX 描述。fps 不写死：
# TX 按片源探针对齐，RX 用 ffprobe 同一片源对齐。
# 探针片源：输出 "宽 高 帧率"（RX lane 与 TX 会话都以它对齐）。
probe_file() {
  ffprobe -v error -select_streams v:0 \
    -show_entries stream=width,height,avg_frame_rate,r_frame_rate -of csv=p=0 "$FILE_MP4" 2>/dev/null \
    | awk -F, '{split($3,a,"/"); split($4,b,"/"); if (a[2]>0) fps=a[1]/a[2]; else if (b[2]>0) fps=b[1]/b[2]; else fps=24; printf "%d %d %.3f", $1, $2, fps}'
}

gen_rx_2lane() {
  # gen_rx_2lane <lane1_w> <lane1_h> <lane1_fps>
  python3 - "$CFG_2LANE" "$IP" "$PORT" "$UDP_HDMI" "$UDP_FILE" "$1" "$2" "$3" <<'PY'
import json, sys
out, ip, port, udp_hdmi, udp_file, l1w, l1h, l1fps = sys.argv[1:9]
cfg = {
  "rx_port": port, "rx_lip": ip, "rx_sip": ip,
  "audio_udp_port": 0,
  "drm_device": "/dev/dri/card0", "drm_connector": "HDMI-A-1",
  "drm_mode": "auto", "drm_buf_count": 5,
  "playout_delay_ms": 120, "ptp_mode": "monotonic",
  "st21_pacing": "tsc_narrow", "mtl_log_level": "warning",
  "stats_report_interval_s": 10,
  "cpu_bridge": 1, "cpu_decode": 4, "cpu_render": 5, "cpu_audio": 3,
  "pip_streams": [
    {"lane": 0, "main": True, "sip": ip, "udp_port": int(udp_hdmi),
     "audio_udp_port": 0, "width": 1920, "height": 1080, "fps": 60,
     "x": 0, "y": 0, "w": 1920, "h": 1080},
    {"lane": 1, "main": False, "sip": ip, "udp_port": int(udp_file),
     "audio_udp_port": 0, "width": int(l1w), "height": int(l1h), "fps": float(l1fps),
     "x": 1360, "y": 750, "w": 480, "h": 270},
  ],
}
json.dump(cfg, open(out, "w"), indent=2)
print(f"gen {out}: lanes={len(cfg['pip_streams'])}")
PY
}

gen_tx_file() {
  # gen_tx_file <out> <sec> <udp> <audio_udp>：文件 TX 描述（含 1MB 码流上限）。
  python3 - "$1" "$2" "$PORT" "$IP" "$FILE_MP4" "$3" "$4" <<'PY'
import json, sys
out, sec, port, ip, mp4, udp, aud = sys.argv[1:8]
cfg = {
  "test_time_s": int(sec),
  "interfaces": [{"name": port, "ip": ip}],
  "streams": [{"transport": {"udp_port": int(udp), "dip_addr": ip}}],
  "source": "file", "mp4_file": mp4, "loop": True,
  "audio_udp_port": int(aud), "codestream_size": 1048576,
  "log_level": "warning", "st21_pacing": "tsc_narrow",
}
json.dump(cfg, open(out, "w"), indent=2)
print(f"gen {out}: mp4={mp4} udp={udp}")
PY
}

kill_apps() {
  pkill -9 live_ingest 2>/dev/null || true
  pkill -9 file_playout_rx 2>/dev/null || true
  sleep 2
}

# T0：HDMI 单路回环，判定口径与 run_tx_rx_e2e.sh 一致。
t0_baseline() {
  local sec="$1" rx_log="$OUT_DIR/t0_rx.log" tx_log="$OUT_DIR/t0_tx.log"
  echo "=== T0 基线：HDMI 单路回环 ${sec}s ==="
  hdmi_stable_check || return 1
  kill_apps
  rm -f "$rx_log" "$tx_log"
  nohup $UNBUF "$RX_BIN" "$sec" --port "$PORT" --lip "$IP" --sip "$IP" \
    --udp "$UDP_HDMI" --width 1920 --height 1080 --fps 60 \
    --ptp-mode monotonic --stats-interval 10 >"$rx_log" 2>&1 &
  local rx_pid=$!
  sleep 3
  if ! kill -0 "$rx_pid" 2>/dev/null; then tail -n 20 "$rx_log"; fail "T0 RX 启动失败"; return 1; fi
  "$TX_BIN" "$sec" --port "$PORT" --lip "$IP" --dip "$IP" --udp "$UDP_HDMI" \
    --pacing tsc_narrow --log-level warning >"$tx_log" 2>&1
  wait "$rx_pid" || true
  SEC0="$sec" python3 - "$rx_log" "$tx_log" <<'PY'
import os, re, sys
rx, tx = open(sys.argv[1], errors="replace").read(), open(sys.argv[2], errors="replace").read()
sec = float(os.environ.get("SEC0", "30"))
ok = True
def check(n, c, d=""):
    global ok
    print(("PASS" if c else "FAIL") + f": T0 {n} {d}")
    ok = ok and c
m = re.search(r"rx_pipe: (\d+) pkt, (\d+) dec.*?pb_underrun=(\d+), (\d+) put_fail", rx, re.S)
if not m: print("FAIL: T0 无 RX 汇总行"); sys.exit(1)
pkt, dec, under, putfail = map(int, m.groups())
check("收流等速 dec/pkt>=0.99", pkt > 0 and dec / pkt >= 0.99, f"{dec}/{pkt}")
check("收流总量达标 pkt>=0.7 名义值（防抖动截断误过）", pkt >= 0.7 * 60 * max(sec - 3, 1), f"pkt={pkt}")
check("零丢包", "mtl_drop=+0" in rx and "br_drop=+0" in rx)
check("零坏帧", "corrupt=0" in rx and "incomplete=0" in rx)
check("解码无反压", putfail == 0)
mm = re.search(r"(\d+) captured, (\d+) encoded", tx)
if mm:
    cap, enc = int(mm.group(1)), int(mm.group(2))
    check("发送跟上 enc/cap>=0.95", cap > 0 and enc / cap >= 0.95, f"{enc}/{cap}")
    check("发送总量达标 enc>=0.8 名义值（防抖动截断误过）", enc >= 0.8 * 60 * sec, f"enc={enc}")
else: check("TX 有 captured/encoded 行", False)
sys.exit(0 if ok else 1)
PY
}

# T1：文件源单发单收（虚拟输入节点链路 + 音频分支）。
# TX 用渲染后的 JSON（含 1MB 码流上限）；RX fps 用 ffprobe 同一片源对齐。
# 判定用稳态窗口（去头尾）：供帧速率≈会话帧率、链路产出比、零错包。
t1_file_single() {
  local sec="$1" rx_log="$OUT_DIR/t1_rx.log" tx_log="$OUT_DIR/t1_tx.log"
  local tx_json="$OUT_DIR/t1_tx.json"
  echo "=== T1 文件源单路 ${sec}s ($FILE_MP4) ==="
  read -r w1 h1 fps1 <<<"$(probe_file)"
  [ -n "$fps1" ] || { w1=1920; h1=1080; fps1=24; }
  note "片源 $w1 x $h1 @ fps=$fps1"
  gen_tx_file "$tx_json" "$sec" "$UDP_FILE" "$AUDIO_FILE"
  kill_apps
  rm -f "$rx_log" "$tx_log"
  nohup $UNBUF "$RX_BIN" "$sec" --port "$PORT" --lip "$IP" --sip "$IP" \
    --udp "$UDP_FILE" --audio-udp "$AUDIO_FILE" --width "$w1" --height "$h1" --fps "$fps1" \
    --ptp-mode monotonic --stats-interval 10 >"$rx_log" 2>&1 &
  local rx_pid=$!
  sleep 3
  if ! kill -0 "$rx_pid" 2>/dev/null; then tail -n 20 "$rx_log"; fail "T1 RX 启动失败"; return 1; fi
  "$TX_BIN" --config "$tx_json" \
    >"$tx_log" 2>&1
  wait "$rx_pid" || true
  FPS1="$fps1" SEC1="$sec" python3 - "$rx_log" "$tx_log" <<'PY'
import os, re, sys
rx, tx = open(sys.argv[1], errors="replace").read(), open(sys.argv[2], errors="replace").read()
fps = float(os.environ.get("FPS1", "24"))
sec = float(os.environ.get("SEC1", "30"))
ok = True
def check(n, c, d=""):
    global ok
    print(("PASS" if c else "FAIL") + f": T1 {n} {d}")
    ok = ok and c
m = re.search(r"video exit \((\d+) frames, (\d+) drops\)", tx)
if m:
    fr, dr = int(m.group(1)), int(m.group(2))
    check("文件源供帧量≈会话帧率±10%", abs(fr - fps * sec) / (fps * sec) <= 0.10, f"frames={fr}")
    check("供帧零丢弃", dr == 0, f"drops={dr}")
else: check("TX 有 file_source video exit 行", False)
check("TX 按片源探针对齐", re.search(r"probe .*\u2192 \d+x\d+@", tx) is not None)
wins = re.findall(r"RX\[\d+\]: mtl_rx=\+(\d+) bridge=\+(\d+) dec=\+(\d+).*?mtl_drop=\+(\d+) br_drop=\+(\d+) corrupt=(\d+) incomplete=(\d+)", rx)
if len(wins) >= 3:
    steady = wins[1:-1]
    mr = sum(int(w[0]) for w in steady); dc = sum(int(w[2]) for w in steady)
    check("稳态收流等速 dec/mtl_rx>=0.99", mr > 0 and dc / mr >= 0.99, f"{dc}/{mr}")
    check("稳态零丢包", all(int(w[3]) == 0 and int(w[4]) == 0 for w in steady))
    check("稳态零坏帧", all(int(w[5]) == 0 and int(w[6]) == 0 for w in steady))
else: check(f"稳态窗口数>=3（去头尾），实际 {len(wins)}", False)
if "ALSA open failed" in rx or "continuing without audio" in rx:
    print("INFO: T1 音频分支降级为无音频（ALSA 不可用），视频结论不受影响")
else:
    print("INFO: T1 音频分支已建连（见 ST30/ALSA 行）")
sys.exit(0 if ok else 1)
PY
}

# T2：HDMI TX + 文件 TX 并发，对 2-lane compositor。
t2_composite() {
  local tx_sec="$1"
  local rx_sec=$((tx_sec + 20))
  local rx_log="$OUT_DIR/t2_rx.log" tx1_log="$OUT_DIR/t2_tx_hdmi.log" tx2_log="$OUT_DIR/t2_tx_file.log"
  local tx_json="$OUT_DIR/t2_tx_file.json"
  echo "=== T2 双路合成 TX=${tx_sec}s RX=${rx_sec}s ==="
  hdmi_stable_check || return 1
  read -r w1 h1 fps1 <<<"$(probe_file)"
  [ -n "$fps1" ] || { w1=1920; h1=1080; fps1=24; }
  note "文件源 $w1 x $h1 @ fps=$fps1"
  gen_rx_2lane "$w1" "$h1" "$fps1"
  gen_tx_file "$tx_json" "$tx_sec" "$UDP_FILE" 0
  kill_apps
  rm -f "$rx_log" "$tx1_log" "$tx2_log"
  nohup $UNBUF "$RX_BIN" "$rx_sec" --config "$CFG_2LANE" \
    --port "$PORT" --lip "$IP" --sip "$IP" \
    --ptp-mode monotonic >"$rx_log" 2>&1 &
  local rx_pid=$!
  # 合成器启动含 MTL 初始化与双 lane 会话建立，轮询等 marker（stdout 有缓冲，不可一次 grep 定死）。
  local waited=0
  while [ $waited -lt 40 ]; do
    sleep 2
    waited=$((waited + 2))
    kill -0 "$rx_pid" 2>/dev/null || { tail -n 30 "$rx_log"; fail "T2 RX 启动失败（进程已退出）"; return 1; }
    if grep -aq "rx_comp: 2 lane(s)" "$rx_log"; then break; fi
  done
  grep -aq "rx_comp: 2 lane(s)" "$rx_log" \
    || { tail -n 30 "$rx_log"; fail "T2 合成器未进入 2-lane（40s 超时）"; kill "$rx_pid"; return 1; }
  note "合成器已进入 2-lane，依次起两个 TX"
  nohup $UNBUF "$TX_BIN" --config "$tx_json" >"$tx2_log" 2>&1 &
  local tx2_pid=$!
  sleep 2
  "$TX_BIN" "$tx_sec" --port "$PORT" --lip "$IP" --dip "$IP" --udp "$UDP_HDMI" \
    --pacing tsc_narrow --log-level warning >"$tx1_log" 2>&1
  echo "HDMI TX 退出=$?"
  wait "$tx2_pid" || true
  wait "$rx_pid" || true
  TXSEC="$tx_sec" FPS1="$fps1" python3 - "$rx_log" "$tx1_log" "$tx2_log" <<'PY'
import os, re, sys
rx, tx1, tx2 = (open(p, errors="replace").read() for p in sys.argv[1:4])
tx_sec = float(os.environ.get("TXSEC", "40")); fps1 = float(os.environ.get("FPS1", "24"))
ok = True
def check(n, c, d=""):
    global ok
    print(("PASS" if c else "FAIL") + f": T2 {n} {d}")
    ok = ok and c
mm = re.search(r"(\d+) captured, (\d+) encoded", tx1)
if mm:
    cap, enc = int(mm.group(1)), int(mm.group(2))
    check("HDMI TX 跟上 enc/cap>=0.95", cap > 0 and enc / cap >= 0.95, f"{enc}/{cap}")
    check("HDMI TX 总量达标（防抖动截断误过）", enc >= 0.8 * 60 * tx_sec, f"enc={enc}")
else: check("HDMI TX 有 captured/encoded 行", False)
m = re.search(r"video exit \((\d+) frames, (\d+) drops\)", tx2)
if m:
    fr, dr = int(m.group(1)), int(m.group(2))
    check("文件 TX 供帧 drops=0", fr > 0 and dr == 0, f"frames={fr} drops={dr}")
else: check("文件 TX 有 video exit 行", False)
lanes = re.findall(r"RX\[(\d+)\] total: pkt=(\d+) dec=(\d+)", rx)
by_port = {}
for port, pkt, dec in lanes: by_port[port] = (int(pkt), int(dec))
expect = {os.environ.get("UDP_HDMI", "15000"): 60.0, os.environ.get("UDP_FILE", "15010"): fps1}
for port, lane_fps in expect.items():
    if port not in by_port: check(f"lane udp={port} 有统计", False); continue
    pkt, dec = by_port[port]
    check(f"lane udp={port} 等速 dec/pkt>=0.95", pkt > 0 and dec / pkt >= 0.95, f"{dec}/{pkt}")
    check(f"lane udp={port} 总量达标（防抖动截断误过）", pkt >= 0.7 * lane_fps * max(tx_sec - 10, 1), f"pkt={pkt}")
tail = rx[rx.rfind("=== RX compositor stats"):]
check("零丢包", "mtl_drop=+0" in tail and "br_drop=+0" in tail)
check("零坏帧", "corrupt=0" in tail and "incomplete=0" in tail)
ms = re.findall(r"composite_frames=(\d+)", rx)
last = ms[-1] if ms else "0"
check("有合成输出 composite_frames>0", int(last) > 0, f"composite_frames={last}（末值）")
sys.exit(0 if ok else 1)
PY
}

# T3：非法多流配置必须被明确拒绝（不跑流，校验先于 MTL 初始化）。
t3_negative() {
  echo "=== T3 配置反例（期望：明确拒绝，退出非零）==="
  local bad1="$OUT_DIR/t3_dup.json" bad2="$OUT_DIR/t3_nomain.json"
  python3 - "$CFG_2LANE" "$bad1" "$bad2" <<'PY'
import json, sys
src, dup_p, nomain_p = sys.argv[1:4]
cfg = json.load(open(src))
dup = json.loads(json.dumps(cfg))
dup["pip_streams"][1]["udp_port"] = dup["pip_streams"][0]["udp_port"]
json.dump(dup, open(dup_p, "w"))
nom = json.loads(json.dumps(cfg))
for s in nom["pip_streams"]: s["main"] = False
json.dump(nom, open(nomain_p, "w"))
PY
  local rc=0
  if "$RX_BIN" 5 --config "$bad1" --port "$PORT" --lip "$IP" --sip "$IP" \
      --ptp-mode monotonic >"$OUT_DIR/t3_dup.log" 2>&1; then
    fail "T3 重复端口配置未被拒绝"; rc=1
  else
    grep -aq "duplicate sip:port" "$OUT_DIR/t3_dup.log" \
      && echo "PASS: T3 重复端口被拒绝（duplicate sip:port）" \
      || { fail "T3 拒绝原因不是 duplicate sip:port"; rc=1; }
  fi
  if "$RX_BIN" 5 --config "$bad2" --port "$PORT" --lip "$IP" --sip "$IP" \
      --ptp-mode monotonic >"$OUT_DIR/t3_nomain.log" 2>&1; then
    fail "T3 无 main 配置未被拒绝"; rc=1
  else
    grep -aq "require exactly one main" "$OUT_DIR/t3_nomain.log" \
      && echo "PASS: T3 无 main 被拒绝（require exactly one main）" \
      || { fail "T3 拒绝原因不是 require exactly one main"; rc=1; }
  fi
  return $rc
}

preflight || exit 1
# file 重定向下 libc 全缓冲，轮询类检查必须实时可见：优先 stdbuf 行缓冲。
if command -v stdbuf >/dev/null 2>&1; then UNBUF="stdbuf -o0 -e0"; else UNBUF=""; fi
echo "OUT_DIR=$OUT_DIR SEC=$SEC IP=$IP PORT=$PORT MP4=$FILE_MP4"
case "$WHICH" in
  T0) t0_baseline "$SEC" ;;
  T1) t1_file_single "$SEC" ;;
  T2) t2_composite "$SEC" ;;
  T3) t3_negative ;;
  all) t0_baseline 30 && t1_file_single 30 && t2_composite "$SEC" && t3_negative ;;
  *) echo "用法：$0 [T0|T1|T2|T3|all] [秒数]"; exit 1 ;;
esac
