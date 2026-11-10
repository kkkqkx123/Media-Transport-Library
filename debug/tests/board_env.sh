#!/bin/bash
# 板端硬件释放与测试环境准备/恢复（板上直跑，只依赖标准工具）。
# 解决的问题：Xorg 占 DRM master、PulseAudio 占声卡、残留进程占 video0/UDP，
#   导致任何 e2e 脚本都无法独立跑通。用法（三步即完整环境）：
#   board_env.sh status                  # 只读：环境是否可测（0 可测/1 阻塞）
#   board_env.sh run -- ./run_tx_rx_e2e.sh 60   # 一条命令：准备→跑→恢复（含 Ctrl-C）
#   board_env.sh prepare all --yes       # 显式准备；restore 显式恢复
#   board_env.sh release                 # 急救：清应用级残留（不动桌面）
# 状态记录在 /tmp/board_env.state（重启即失，默认即桌面态，无需恢复）。
# 约定：仓库永远是源，板上永不改 src/，见 docs/dev/board-mtl-sync.md。
set -euo pipefail

VIDEO_DEV="${VIDEO_DEV:-/dev/video0}"
DRM_DEV="${DRM_DEV:-/dev/dri/card0}"
# 本轮测试占用的 UDP 端口（e2e 脚本侧 UDP_*/AUDIO_* 变量保持一致即可）。
ENV_UDP_PORTS="${ENV_UDP_PORTS:-15000 15010 15012}"
IP_IF="${IP_IF:-enP2p33s0}"
STATE_FILE="${STATE_FILE:-/tmp/board_env.state}"
LOCK_DIR="${LOCK_DIR:-/tmp/board_env.lock}"

say() { echo "[board_env] $*"; }
fail_hard() { echo "[board_env] FAIL: $*" >&2; exit 1; }

# 取某设备的持有者 PID 列表（fuser 输出流向因版本而异，stdout+stderr 一起扫；
# PID 后常跟访问位后缀如 1395m，必须剥掉，否则 Xorg 这类 mmap 持有者会被漏检）。
holders_of() {
  fuser "$1" 2>&1 | tr ' \t' '\n' | grep -E '^[0-9]+[a-z]*$' | sed 's/[a-z]*$//' | sort -un || true
}
name_of() { ps -o comm= -p "$1" 2>/dev/null | tr -d ' ' || echo "?"; }

with_lock() {
  if mkdir "$LOCK_DIR" 2>/dev/null; then
    echo $$ >"$LOCK_DIR/pid"
    return 0
  fi
  local oldpid
  oldpid=$(cat "$LOCK_DIR/pid" 2>/dev/null || echo "")
  if [ -n "$oldpid" ] && kill -0 "$oldpid" 2>/dev/null; then
    fail_hard "另一 board_env.sh (pid $oldpid) 正在执行，先等它结束"
  fi
  rm -rf "$LOCK_DIR"; mkdir "$LOCK_DIR"; echo $$ >"$LOCK_DIR/pid"
}
unlock() { rm -rf "$LOCK_DIR"; }

# ---- status：只读，按 scope 检查，quiet 下只打印阻塞项 ----
do_status() {
  local scope="${1:-all}" quiet="${2:-0}" blocked=0
  block() { echo "[board_env] BLOCKED: $*"; blocked=1; }
  pass() { [ "$quiet" = 1 ] || echo "[board_env] ok: $*"; }
  need_scope() { case ",$scope," in *",$1,"*|*,all,*) return 0;; esac; return 1; }

  if need_scope display; then
    if [ ! -e "$DRM_DEV" ]; then block "无 $DRM_DEV"; else
      local h
      h=$(holders_of "$DRM_DEV")
      if [ -n "$h" ]; then
        block "$DRM_DEV 被占用：$(for p in $h; do echo -n "$p($(name_of "$p")) "; done)（prepare display 停桌面释放）"
      else pass "$DRM_DEV 空闲"; fi
    fi
    if [ -e /sys/class/drm/card0-HDMI-A-1/status ]; then
      [ "$(cat /sys/class/drm/card0-HDMI-A-1/status)" = connected ] \
        && pass "HDMI-A-1 connected" || block "HDMI-A-1 未连接"
    fi
  fi
  if need_scope capture; then
    if [ ! -e "$VIDEO_DEV" ]; then block "无 $VIDEO_DEV"; else
      local h
      h=$(holders_of "$VIDEO_DEV")
      if [ -n "$h" ]; then
        block "$VIDEO_DEV 被占用：$(for p in $h; do echo -n "$p($(name_of "$p")) "; done)（release/prepare capture 释放）"
      else pass "$VIDEO_DEV 空闲"; fi
      if command -v v4l2-ctl >/dev/null 2>&1; then
        v4l2-ctl -d "$VIDEO_DEV" --query-dv-timings >/dev/null 2>&1 \
          && pass "HDMI 输入有时序锁存" || block "HDMI 输入无信号（query-dv-timings 未锁存，先查源）"
      fi
    fi
  fi
  if need_scope audio; then
    if command -v aplay >/dev/null 2>&1; then
      aplay -l 2>/dev/null | grep -q '^card' && pass "ALSA 播设备存在" || block "无 ALSA 播放设备"
    fi
    local h
    h=$(holders_of /dev/snd/pcmC*D*p 2>/dev/null || true)
    # glob 未展开时 holders_of 会报错，改用 fuser 逐个已存在节点
    h=$(for n in /dev/snd/pcm*; do [ -e "$n" ] || continue; holders_of "$n"; done | sort -u)
    if [ -n "$h" ]; then
      block "声卡被占用：$(for p in $h; do echo -n "$p($(name_of "$p")) "; done)（prepare audio/all 释放）"
    else pass "声卡 PCM 空闲"; fi
    if pgrep -x pulseaudio >/dev/null 2>&1; then
      block "PulseAudio 运行中（pid $(pgrep -x pulseaudio | tr '\n' ' ')），直开 hw 前需释放（prepare audio/all）"
    else pass "无 PulseAudio 竞争"; fi
  fi
  if need_scope net; then
    if ! ip -o addr show "$IP_IF" 2>/dev/null | grep -q 'inet '; then
      block "网卡 $IP_IF 无 IPv4 地址"
    else pass "$IP_IF 地址就绪"; fi
    for port in $ENV_UDP_PORTS; do
      local h
      h=$(holders_of "${port}/udp")
      [ -n "$h" ] \
        && block "UDP $port 被占用：$(for p in $h; do echo -n "$p($(name_of "$p")) "; done)" \
        || pass "UDP $port 空闲"
    done
  fi
  pgrep -f MtlManager >/dev/null 2>&1 && pass "MtlManager 运行中" || block "MtlManager 未运行"
  if [ "$blocked" -ne 0 ]; then
    [ "$quiet" = 1 ] || say "环境未就绪（见上 BLOCKED 行）：board_env.sh prepare all --yes 或 run -- <cmd>"
    return 1
  fi
  [ "$quiet" = 1 ] || say "环境就绪（scope=$scope）"
  return 0
}

# ---- release：只清应用级残留，不碰桌面（Xorg/Pulse） ----
do_release() {
  with_lock; trap unlock EXIT
  say "清残留进程（live_ingest/file_playout_rx/探针类）..."
  pkill -INT live_ingest file_playout_rx hdmi_passthrough drm_smoke 2>/dev/null || true
  sleep 1
  pkill -9 live_ingest file_playout_rx hdmi_passthrough drm_smoke 2>/dev/null || true
  pkill -9 -f mpp_.._latency 2>/dev/null || true
  pkill -9 -f v4l2_cap_latency 2>/dev/null || true
  pkill -9 mpi_dec_test 2>/dev/null || true
  say "按设备释放 $VIDEO_DEV 持有者..."
  local h
  h=$(holders_of "$VIDEO_DEV")
  if [ -n "$h" ]; then kill -INT $h 2>/dev/null || true; sleep 2; h=$(holders_of "$VIDEO_DEV"); fi
  # shellcheck disable=SC2086
  [ -n "$h" ] && fuser -k "$VIDEO_DEV" 2>/dev/null || true
  say "释放 UDP 端口：$ENV_UDP_PORTS..."
  for port in $ENV_UDP_PORTS; do fuser -k "${port}/udp" 2>/dev/null || true; done
  sleep 1
  say "release 完成（桌面/Xorg 未动；显示/声卡被桌面占用时用 prepare）"
  unlock; trap - EXIT
}

record_state() { echo "LIGHTDM_WAS_ACTIVE=$1" >"$STATE_FILE"; echo "SCOPE=$2" >>"$STATE_FILE"; }

# ---- prepare：按 scope 释放硬件；破坏性动作需 --yes ----
do_prepare() {
  local scope="${1:-all}" yes="${2:-0}"
  with_lock; trap unlock EXIT
  local plan_only=0
  [ "$yes" = 1 ] || plan_only=1
  say "准备测试环境（scope=$scope）..."
  # 1) 先做非破坏性：清残留 + MtlManager 兜底
  pkill -INT live_ingest file_playout_rx 2>/dev/null || true
  sleep 1
  pkill -9 live_ingest file_playout_rx 2>/dev/null || true
  if ! pgrep -f MtlManager >/dev/null 2>&1; then
    say "起 MtlManager..."
    nohup /usr/local/bin/MtlManager >/tmp/mtlmanager2.log 2>&1 &
    sleep 2
  fi
  pgrep -f MtlManager >/dev/null || fail_hard "MtlManager 起不来"
  # 2) 破坏性动作：先公示，无 --yes 只打印计划
  local need_desktop=0
  case ",$scope," in *,display,*|*,audio,*|*,all,*)
    if holders_of "$DRM_DEV" | grep -q . || pgrep -x pulseaudio >/dev/null 2>&1; then need_desktop=1; fi;;
  esac
  if [ "$need_desktop" = 1 ] && [ "$plan_only" = 1 ]; then
    echo "[board_env] 将执行（破坏性，需 --yes）：systemctl stop lightdm（释放 DRM master + 桌面 PulseAudio）"
    echo "[board_env] 当前持有者：DRM=$(for p in $(holders_of "$DRM_DEV"); do echo -n "$p($(name_of "$p")) "; done) PULSE=$(pgrep -x pulseaudio | tr '\n' ' ')"
    unlock; trap - EXIT
    return 2
  fi
  # 记录状态（幂等：已是文本态则记 0，restore 不做多余动作）
  if systemctl is-active lightdm >/dev/null 2>&1; then record_state 1 "$scope"; else record_state 0 "$scope"; fi
  if [ "$need_desktop" = 1 ]; then
    say "停桌面释放 DRM/声卡：systemctl stop lightdm..."
    systemctl stop lightdm || fail_hard "stop lightdm 失败"
    local i=0
    while [ $i -lt 20 ] && holders_of "$DRM_DEV" | grep -q .; do sleep 1; i=$((i + 1)); done
    holders_of "$DRM_DEV" | grep -q . && fail_hard "$DRM_DEV 仍被占用：$(holders_of "$DRM_DEV")"
    say "DRM 已释放"
  fi
  case ",$scope," in *,capture,*|*,all,*)
    local h
    h=$(holders_of "$VIDEO_DEV")
    if [ -n "$h" ]; then
      # shellcheck disable=SC2086
      kill -INT $h 2>/dev/null || true; sleep 2; h=$(holders_of "$VIDEO_DEV")
      # shellcheck disable=SC2086
      [ -n "$h" ] && { [ "$plan_only" = 1 ] && { echo "[board_env] 将执行：fuser -k $VIDEO_DEV（持有者 $h）"; unlock; trap - EXIT; return 2; }; fuser -k "$VIDEO_DEV" 2>/dev/null || true; sleep 1; }
    fi
    holders_of "$VIDEO_DEV" | grep -q . && fail_hard "$VIDEO_DEV 仍被占用"
    ;;
  esac
  for port in $ENV_UDP_PORTS; do fuser -k "${port}/udp" 2>/dev/null || true; done
  sleep 1
  say "准备完成"
  unlock; trap - EXIT
}

# ---- restore：按记录状态恢复桌面 ----
do_restore() {
  [ -f "$STATE_FILE" ] || { say "无准备记录（$STATE_FILE 缺失），无需恢复"; return 0; }
  # shellcheck disable=SC1090
  . "$STATE_FILE"
  if [ "${LIGHTDM_WAS_ACTIVE:-0}" = 1 ]; then
    say "恢复桌面：systemctl start lightdm..."
    systemctl start lightdm || fail_hard "start lightdm 失败"
    local i=0
    while [ $i -lt 30 ] && ! pgrep -x Xorg >/dev/null 2>&1; do sleep 1; i=$((i + 1)); done
    pgrep -x Xorg >/dev/null 2>&1 && say "桌面已恢复" || fail_hard "Xorg 未回来，手工检查"
  else
    say "准备前即文本态，无需恢复桌面"
  fi
  rm -f "$STATE_FILE"
}

# ---- run：prepare → 跑命令 → 必 restore（含 Ctrl-C） ----
do_run() {
  # 用法：board_env.sh run -- <cmd...>
  do_prepare all 1
  local rc=0 interrupted=0
  on_int_term() { interrupted=1; }
  trap on_int_term INT TERM
  set +e
  "$@"
  rc=$?
  set -e
  trap - INT TERM
  do_restore || rc=$?
  if [ "$interrupted" = 1 ]; then say "被中断，已恢复环境"; exit 130; fi
  exit "$rc"
}

usage() {
  echo "用法：$0 status [scope] [--quiet] | prepare [scope] [--yes] | run -- <cmd...> | release | restore"
  echo "  scope: all|display|audio|capture|net（默认 all）"
  echo "  例：$0 run -- ./run_tx_rx_e2e.sh 60"
  exit 1
}

valid_scope() {
  # valid_scope <scope>：all 或 display/audio/capture/net 的逗号组合
  case ",$1," in *,all,*) [ "$1" = all ];;
    *) local t ok=1; IFS=,; for t in $1; do
         case "$t" in display|audio|capture|net) ;; *) ok=0;; esac; done; unset IFS
         [ "$ok" = 1 ] && [ -n "$1" ];;
  esac
}
CMD="${1:-}"; shift || true
case "$CMD" in
  status)
    SCOPE=all; QUIET=0
    for a in "$@"; do case "$a" in --quiet) QUIET=1;; *) valid_scope "$a" && SCOPE="$a" || usage;; esac; done
    do_status "$SCOPE" "$QUIET";;
  prepare)
    SCOPE=all; YES=0
    for a in "$@"; do case "$a" in --yes) YES=1;; *) valid_scope "$a" && SCOPE="$a" || usage;; esac; done
    do_prepare "$SCOPE" "$YES";;
  run)
    [ "${1:-}" = "--" ] || usage; shift
    [ $# -ge 1 ] || usage
    do_run "$@";;
  release) do_release;;
  restore) do_restore;;
  *) usage;;
esac
