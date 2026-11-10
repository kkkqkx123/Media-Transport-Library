#!/bin/bash
# 带音频文件片源生成器：T1/ST30-ALSA 端到端验证的固定输入。
# 本地与板端通用；默认输出进板端持久目录 ~/code/mtl/streams/
# （数据态，见 docs/dev/board-mtl-sync.md §1），不进 git。
#
# 背景：T1 无音频轨时音频断言全部 SKIP、有音频轨全静音时 FAIL，
# 无声片源会让“通过”失去含金量（ST30 feeder/ALSA 消费/peak 统计全不跑）。
# 本脚本 pin 住一路“有声且非静音”片源，替代无人管理的旧默认路径。
# T2 文件路音频 by-design 关闭（合成器音频取 main lane=HDMI），只验视频合成，
# 故本片源对 T2 的价值是 PiP 画面持续变化可目视；T3 不跑流，不受影响。
#
# 用法：gen_av_file_source.sh [--out FILE] [--duration S] [--vcodec h264|h265] [--force]
#   缺省 60s，覆盖 T1/T2 默认 40s 无需 loop；manual_check 120s hold 会 loop 一次，
#   正好顺带 exercised 播放列表切换的 TAI 重置。
#   --vcodec h265 产出 HEVC 片源，供 H265 文件源端到端使用（默认输出名带 _hevc 后缀）。
set -euo pipefail

VCODEC="${VCODEC:-h264}"
OUT_ARG=""
DUR=60
FORCE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT_ARG="$2"; shift 2 ;;
    --duration) DUR="$2"; shift 2 ;;
    --vcodec) VCODEC="$2"; shift 2 ;;
    --force) FORCE=1; shift ;;
    *) echo "用法：$0 [--out FILE] [--duration S] [--vcodec h264|h265] [--force]"; exit 1 ;;
  esac
done
case "$VCODEC" in
  h264|h265) ;;
  *) echo "用法：--vcodec 须为 h264 或 h265"; exit 1 ;;
esac

if [ -n "$OUT_ARG" ]; then
  OUT="$OUT_ARG"
elif [ -n "${OUT:-}" ]; then
  OUT="$OUT"
elif [ "$VCODEC" = "h265" ]; then
  OUT="$HOME/code/mtl/streams/multinode_av_1080p30_hevc.mp4"
else
  OUT="$HOME/code/mtl/streams/multinode_av_1080p30.mp4"
fi

if [ -s "$OUT" ] && [ "$FORCE" = 0 ]; then
  echo "已存在 $OUT，跳过（--force 强制再生）"
  exit 0
fi
command -v ffmpeg >/dev/null 2>&1 || { echo "缺 ffmpeg（生成片源用）"; exit 1; }
command -v ffprobe >/dev/null 2>&1 || { echo "缺 ffprobe（校验片源用）"; exit 1; }
# 注：此处不用 `ffmpeg -encoders | grep -q` 管道写法——`grep -q` 首行匹配即关管道，
# 大输出的 ffmpeg 会得 SIGPIPE，在 `pipefail` 下误判为失败。改用变量承接后匹配。
ENC_LIST="$(ffmpeg -hide_banner -encoders 2>/dev/null || true)"
if [ "$VCODEC" = "h265" ]; then
  case "$ENC_LIST" in
    *libx265*) ;;
    *) echo "缺 libx265 编码器（ffmpeg 未含 libx265）"; exit 1 ;;
  esac
else
  case "$ENC_LIST" in
    *libx264*) ;;
    *) echo "缺 libx264 编码器（ffmpeg 未含 libx264）"; exit 1 ;;
  esac
fi

mkdir -p "$(dirname "$OUT")"
# testsrc2 动起来（PiP 目视可辨冻结）+ 440Hz 正弦（peak 远高于 T1 的 500 门限）。
# 视频 4Mbps/30fps：单 AU 十几 KB，远低于 TX 侧 1MB 码流上限，不触发供帧丢弃。
# CFR 30fps + AAC 48kHz 立体声：file_source 走直通路径，免重采样。
if [ "$VCODEC" = "h265" ]; then
  VCODEC_OPTS="-c:v libx265 -preset veryfast -pix_fmt yuv420p -b:v 4M -maxrate 6M -bufsize 8M -g 30 -r 30"
else
  VCODEC_OPTS="-c:v libx264 -preset veryfast -pix_fmt yuv420p -b:v 4M -maxrate 6M -bufsize 8M -g 30 -bf 2 -r 30"
fi
ffmpeg -y -v error \
  -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=$DUR" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=$DUR" \
  $VCODEC_OPTS \
  -c:a aac -b:a 128k -ac 2 -ar 48000 \
  -shortest -movflags +faststart "$OUT.tmp.mp4"
mv -f "$OUT.tmp.mp4" "$OUT"

# 校验口径与 T1 门禁同源：有视频轨（1920x1080 且编码符合预期）、有音频轨、音频非全静音。
W=$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$OUT" | tr -d '[:space:]')
H=$(ffprobe -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$OUT" | tr -d '[:space:]')
[ "$W" = 1920 ] && [ "$H" = 1080 ] || { echo "FAIL: 视频不是 1920x1080（得 ${W}x${H}）"; exit 1; }
VC=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$OUT" | tr -d '[:space:]')
if [ "$VCODEC" = "h265" ]; then
  [ "$VC" = "hevc" ] || { echo "FAIL: 视频编码不是 hevc（得 $VC）"; exit 1; }
else
  [ "$VC" = "h264" ] || { echo "FAIL: 视频编码不是 h264（得 $VC）"; exit 1; }
fi
ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$OUT" \
  | grep -q . || { echo "FAIL: 片源无音频轨"; exit 1; }
VOL=$(ffmpeg -v info -i "$OUT" -map 0:a:0 -af volumedetect -f null /dev/null 2>&1 \
  | grep -E "mean_volume|max_volume" || true)
echo "$VOL"
MAXV=$(echo "$VOL" | sed -n 's/.*max_volume: *\(-*[0-9.]*\).*/\1/p')
[ -n "$MAXV" ] && [ "$MAXV" != "-inf" ] || { echo "FAIL: 片源音频全静音"; exit 1; }
ls -l "$OUT"
echo "PASS: 带音频片源就绪 $OUT"
