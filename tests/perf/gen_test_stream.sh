#!/bin/bash
# 合成预置流：无 HDMI 时的解码上限与回归输入，NV12 与 H264 各一版。
# 板上持久目录 ~/code/mtl/streams，输出长期保留。
set -euo pipefail
command -v ffmpeg >/dev/null 2>&1 || { echo "缺 ffmpeg（生成预置流用）"; exit 1; }

OUT="$HOME/code/mtl/streams"
FPS=60
N=120
GEN4K=0
N4K=60
FPS4K=30

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --fps) FPS="$2"; shift 2 ;;
    --frames) N="$2"; shift 2 ;;
    --4k) GEN4K=1; shift ;;
    --frames4k) N4K="$2"; shift 2 ;;
    --fps4k) FPS4K="$2"; shift 2 ;;
    *) echo "usage: $0 [--out DIR] [--fps FPS] [--frames N] [--4k] [--frames4k N] [--fps4k FPS]"; exit 1 ;;
  esac
done
mkdir -p "$OUT"

gen() {
  name="$1"; src="$2"; w="$3"; h="$4"
  nv12="$OUT/${name}_${w}x${h}.nv12"
  h264="$OUT/${name}_${w}x${h}.h264"
  if [ -s "$nv12" ] && [ -s "$h264" ]; then
    echo "[gen] $name ${w}x${h} 已存在，跳过（删文件后重跑可强制再生）"
    return 0
  fi
  echo "[gen] $name ${w}x${h} fps=$FPS n=$N"
  ffmpeg -y -v error -f lavfi -i "$src=size=${w}x${h}:rate=$FPS:duration=$((N / FPS))" \
    -frames:v "$N" -pix_fmt nv12 -f rawvideo "$nv12"
  ffmpeg -y -v error -f lavfi -i "$src=size=${w}x${h}:rate=$FPS:duration=$((N / FPS))" \
    -frames:v "$N" -c:v libx264 -preset veryfast -pix_fmt yuv420p "$h264"
  ls -l "$nv12" "$h264"
}

# 时钟条纹（testsrc 自带时间码）：编码基线，最接近真实场景
gen clock "testsrc" 1920 1080
# 静态色条：零帧间差异，编码器空闲开销
gen bars "smptehdbars" 1920 1080
# 运动块：码控行为观察
gen motion "testsrc2" 1920 1080
# 720p 一档：预算对比
gen clock720 "testsrc" 1280 720

# 4K 一档（opt-in：文件大、libx264 慢，默认不生成）：
# 只生成时钟条纹 60 帧 @30fps，NV12 约 746MB，H264 由 ffmpeg 软编码生成。
# 更快的 H264 获取方式见 README：用 mpp_enc_latency 硬编码 NV12 得到。
if [ "$GEN4K" = 1 ]; then
  SAV_FPS="$FPS"; SAV_N="$N"
  FPS="$FPS4K"; N="$N4K"
  gen clock "testsrc" 3840 2160
  FPS="$SAV_FPS"; N="$SAV_N"
fi

echo "[gen] done -> $OUT"
