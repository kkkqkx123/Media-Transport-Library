#!/bin/bash
# 板端硬件释放与测试环境准备/恢复（板上直跑，只依赖标准工具）。
# 解决的问题：Xorg 占 DRM master、PulseAudio 占声卡、残留进程占 video0/UDP、
#   重启后 hugepages 归零，导致任何 e2e 脚本都无法独立跑通。用法：
#   board_env.sh status                  # 只读：环境是否可测（0 可测/1 阻塞）
#   board_env.sh run -- ./run_tx_rx_e2e.sh 60   # 一条命令：准备→跑→恢复（含 Ctrl-C）
#   board_env.sh prepare all --yes       # 显式准备；restore 显式恢复
#   board_env.sh text                    # 批量会话：切文本态并保持（prepare 的持久版）
#   board_env.sh graphical               # 批量会话结束：切回图形态（restore 的显式版）
#   board_env.sh release                 # 急救：清应用级残留（不动桌面）
# 桌面 teardown 一律走 systemctl isolate（multi-user/graphical），不再逐个停服务：
# isolate 按依赖顺序停掉整个图形栈，比单停 lightdm 可靠，且已在文本态时自动 no-op。
# 必须经 ssh 在文本终端执行；勿在桌面图形终端内跑（isolate 会结束图形会话）。
# 状态记录在 /tmp/board_env.state（重启即失；已是文本态则 prepare 为 no-op）。
# 约定：仓库永远是源，板上永不改 src/，见 docs/dev/board-mtl-sync.md。
set -euo pipefail

VIDEO_DEV="${VIDEO_DEV:-/dev/video0}"
DRM_DEV="${DRM_DEV:-/dev/dri/card0}"
# 本轮测试占用的 UDP 端口（e2e 脚本侧 UDP_*/AUDIO_* 变量保持一致即可）。
ENV_UDP_PORTS="${ENV_UDP_PORTS:-15000 15010 15012}"
IP_IF="${IP_IF:-enP2p33s0}"
# MTL 大页需求（重启后归零，prepare 内自动配齐，status 内做门禁）。
HUGEPAGES_TARGET="${HUGEPAGES_TARGET:-2048}"
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
# 进程状态字符（/proc/PID/stat 第 3 域）：D=不可中断睡眠（驱动/IO 卡住，杀不掉）。
proc_state_of() { awk '{print $3}' "/proc/$1/stat" 2>/dev/null || echo "?"; }
# 当前是否在图形态（graphical.target 活跃即视为桌面态）。
is_graphical() { systemctl is-active -q graphical.target 2>/dev/null; }

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
        block "$DRM_DEV 被占用：$(for p in $h; do echo -n "$p($(name_of "$p")) "; done)（prepare display 切文本态释放）"
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
      block "声卡被占用：$(for p in $h; do echo -n "$p($(name_of "$p")) "; done)（prepare audio/all 切文本态释放）"
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
  if need_scope mem; then
    local huge
    huge=$(awk '/^HugePages_Total:/{print $2}' /proc/meminfo 2>/dev/null || echo 0)
    [ -n "$huge" ] || huge=0
    if [ "$huge" -ge "$HUGEPAGES_TARGET" ]; then
      pass "hugepages $huge/$HUGEPAGES_TARGET"
    else
      block "hugepages $huge/$HUGEPAGES_TARGET（重启后归零，prepare all --yes 自动配齐）"
    fi
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

record_state() { echo "ISOLATED_BY_US=$1" >"$STATE_FILE"; echo "SCOPE=$2" >>"$STATE_FILE"; }

# 配齐大页：重启后归零是每次首轮失败的主因，prepare 内必须保证。
ensure_hugepages() {
  local cur
  cur=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)
  if [ "${cur:-0}" -ge "$HUGEPAGES_TARGET" ]; then
    say "hugepages $cur/$HUGEPAGES_TARGET 就绪"; return 0
  fi
  say "配 hugepages：$cur → $HUGEPAGES_TARGET ..."
  echo "$HUGEPAGES_TARGET" > /proc/sys/vm/nr_hugepages 2>/dev/null || true
  cur=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)
  if [ "${cur:-0}" -lt "$HUGEPAGES_TARGET" ]; then
    say "一次配不足（碎片），sync + drop_caches 后重试一次..."
    sync 2>/dev/null || true
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
    echo "$HUGEPAGES_TARGET" > /proc/sys/vm/nr_hugepages 2>/dev/null || true
    cur=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)
  fi
  [ "${cur:-0}" -ge "$HUGEPAGES_TARGET" ] \
    || fail_hard "hugepages 仍不足（$cur/$HUGEPAGES_TARGET，内存碎片过高）；先停应用释放内存后重跑 prepare（本脚本不做重启）"
  say "hugepages $cur/$HUGEPAGES_TARGET 就绪"
}

# ---- prepare：按 scope 释放硬件；破坏性动作需 --yes ----
do_prepare() {
  local scope="${1:-all}" yes="${2:-0}"
  with_lock; trap unlock EXIT
  local plan_only=0
  [ "$yes" = 1 ] || plan_only=1
  say "准备测试环境（scope=$scope）..."
  # 1) 先做非破坏性：清残留 + 大页 + MtlManager 兜底
  pkill -INT live_ingest file_playout_rx 2>/dev/null || true
  sleep 1
  pkill -9 live_ingest file_playout_rx 2>/dev/null || true
  ensure_hugepages
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
    echo "[board_env] 将执行（破坏性，需 --yes）：systemctl isolate multi-user.target（整栈停桌面，释放 DRM master + 桌面 PulseAudio）"
    echo "[board_env] 当前持有者：DRM=$(for p in $(holders_of "$DRM_DEV"); do echo -n "$p($(name_of "$p")) "; done) PULSE=$(pgrep -x pulseaudio | tr '\n' ' ')"
    unlock; trap - EXIT
    return 2
  fi
  # 记录状态（幂等：切之前已是文本态则记 0，restore 不做多余动作）
  if is_graphical; then record_state 1 "$scope"; else record_state 0 "$scope"; fi
  if [ "$need_desktop" = 1 ] && ! is_graphical; then
    say "已是文本态，无需 isolate"
    need_desktop=0
  fi
  if [ "$need_desktop" = 1 ]; then
    say "切文本态释放 DRM/声卡：systemctl isolate multi-user.target ..."
    systemctl isolate multi-user.target || fail_hard "isolate multi-user.target 失败"
    local i=0
    while [ $i -lt 30 ] && { holders_of "$DRM_DEV" | grep -q . || pgrep -x pulseaudio >/dev/null 2>&1; }; do sleep 1; i=$((i + 1)); done
    if holders_of "$DRM_DEV" | grep -q . || pgrep -x pulseaudio >/dev/null 2>&1; then
      local h remained=""
      for p in $(holders_of "$DRM_DEV") $(pgrep -x pulseaudio 2>/dev/null || true); do
        [ -n "$p" ] || continue
        remained="$remained $p($(name_of "$p")/state $(proc_state_of "$p"))"
      done
      if echo "$remained" | grep -q "state D"; then
        fail_hard "桌面栈停不掉（持有者处 D 状态，驱动/IO 卡住）：$remained；需物理断电重启（按机房流程，本脚本不执行重启），之后重跑 prepare"
      fi
      fail_hard "桌面栈停不掉，仍被占用：$remained"
    fi
    say "文本态就绪（DRM/声卡已释放）"
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

# ---- restore：按记录状态恢复桌面（仅当本次 prepare 切过文本态） ----
do_restore() {
  [ -f "$STATE_FILE" ] || { say "无准备记录（$STATE_FILE 缺失），无需恢复"; return 0; }
  # shellcheck disable=SC1090
  . "$STATE_FILE"
  if [ "${ISOLATED_BY_US:-0}" = 1 ]; then
    say "恢复桌面：systemctl isolate graphical.target ..."
    systemctl isolate graphical.target || fail_hard "isolate graphical.target 失败"
    local i=0
    while [ $i -lt 30 ] && ! pgrep -x Xorg >/dev/null 2>&1; do sleep 1; i=$((i + 1)); done
    pgrep -x Xorg >/dev/null 2>&1 && say "桌面已恢复" || fail_hard "Xorg 未回来，手工检查（不要反复 stop/start，直接看 card0 持有者）"
  else
    say "本次未切文本态，无需恢复桌面"
  fi
  rm -f "$STATE_FILE"
}

# ---- text/graphical：批量会话的显式持久切换（prepare/restore 的持久版） ----
do_text() {
  do_prepare all 1
}
do_graphical() {
  if [ -f "$STATE_FILE" ]; then
    do_restore
  else
    say "切图形态：systemctl isolate graphical.target ..."
    systemctl isolate graphical.target || fail_hard "isolate graphical.target 失败"
    local i=0
    while [ $i -lt 30 ] && ! pgrep -x Xorg >/dev/null 2>&1; do sleep 1; i=$((i + 1)); done
    pgrep -x Xorg >/dev/null 2>&1 && say "桌面已恢复" || fail_hard "Xorg 未回来，手工检查"
  fi
}

# ---- run：prepare → 跑命令 → 必 restore（含 Ctrl-C；中断时先杀子命令） ----
do_run() {
  # 用法：board_env.sh run -- <cmd...>
  do_prepare all 1
  local rc=0 interrupted=0 child=0
  on_int_term() { interrupted=1; [ "$child" != 0 ] && kill -INT "$child" 2>/dev/null || true; }
  trap on_int_term INT TERM
  set +e
  "$@" &
  child=$!
  wait "$child"
  rc=$?
  child=0
  set -e
  trap - INT TERM
  do_restore || rc=$?
  if [ "$interrupted" = 1 ]; then say "被中断，已恢复环境"; exit 130; fi
  exit "$rc"
}

usage() {
  echo "用法：$0 status [scope] [--quiet] | prepare [scope] [--yes] | run -- <cmd...> | release | restore | text | graphical"
  echo "  scope: all|display|audio|capture|net|mem（默认 all，all 含 mem/hugepages 门禁）"
  echo "  例：$0 run -- ./run_tx_rx_e2e.sh 60"
  echo "  批量会话：$0 text（切文本态保持）→ 直跑各脚本 → $0 graphical（恢复桌面）"
  exit 1
}

valid_scope() {
  # valid_scope <scope>：all 或 display/audio/capture/net/mem 的逗号组合
  case ",$1," in *,all,*) [ "$1" = all ];;
    *) local t ok=1; IFS=,; for t in $1; do
         case "$t" in display|audio|capture|net|mem) ;; *) ok=0;; esac; done; unset IFS
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
  text) do_text;;
  graphical) do_graphical;;
  *) usage;;
esac
