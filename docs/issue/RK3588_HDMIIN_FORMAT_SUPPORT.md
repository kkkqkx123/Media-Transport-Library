# RK3588 HDMI RX 采集格式支持与路径取舍

日期：2026-11-07。板上实测（RK3588，内核 5.10.226，`rk_hdmirx`，1080p60 源）。

## 1. V4L2 枚举与 S_FMT 协商不一致

- 枚举（`VIDIOC_ENUM_FMT`）报告：BGR3 / NV24 / NV16 / NV12。
- 实测：NV12、NV16、NV24 的 `S_FMT` 全部被拒绝（`EINVAL`），无论 `num_planes`
  是否置位；只有 BGR3 可协商成功（G_FMT 回读校验一致；UYVY/YUYV 同样被拒，
  见协商矩阵脚本输出）。
- 初版自研探针曾因 `S_FMT` 未填 `num_planes` 误判 NV12 被拒；修正后复测，
  拒绝依旧，确认为驱动真实行为，不是程序问题。
- 影响：发送侧采集只能拿到 BGR3，NV12 零拷贝直通在此板上不可达；
  生产采集器的 NV12 首选注定失败，回退到 BGR24（即 BGR3）是实际工作路径。
- 后续用新 SDK 复测：逐个 fourcc 做 `S_FMT`＋`G_FMT` 回读校验，
  以回读 fourcc 为准，不要信任枚举列表。

## 2. BGR 直投编码器验证与回退决策

- 验证：MPP VENC 接受 `prep:format=BGR888`，BGR24 原始帧直投可出流；
  30 帧 1080p 实测均值 4.16ms（同源 NV12 路径 3.97ms），输出可正常解码。
- 回退：与既有 RGA＋NV12 路径（RGA 转换约 0.9ms）相比无性能优势，
  且直投需在采集、编码器、管线三处加格式分支并改变创建顺序，
  回退到 RGA＋NV12，相关代码已撤销。
- 依据：板上 `~/code/mtl/streams/bgr_direct.h264`（BGR 直投产物，可解码）
  与 `enc_cost.csv` 保留备查。

## 3. 验证步骤与期望结果（本板，统一用脚本执行）

```bash
# 枚举（期望：列出 BGR3/NV24/NV16/NV12）
v4l2-ctl -d /dev/video0 --list-formats-ext

# 逐格式协商矩阵（期望见下表）
bash debug/v4l2_fmt_matrix.sh 5

# BGR 直投出流抽查（期望：30 帧全部产出，ffprobe 读回 30 帧）
~/code/mtl/bin/mpp_enc_latency --input ~/code/mtl/streams/clock_1920x1080.bgr3 \
  --w 1920 --h 1080 --frames 30 --bps 8000000 --infmt bgr3 --out /tmp/bgr_check.h264
ffprobe -v error -count_frames -show_entries stream=nb_read_frames \
  -of default=noprint_wrappers=1 /tmp/bgr_check.h264
```

期望结果汇总：

| 步骤 | 通过标准 |
|---|---|
| 枚举 | 四个 fourcc 全部列出 |
| 协商矩阵 | NV12/NV16/NV24 报 refused/coerced；BGR3 报 negotiated |
| BGR 直投抽查 | `nb_read_frames=30` |

任一步骤输出与上表不符，先记为环境变化，不要直接判程序问题。

## 4. RGA 转换开销定位

- RGA 自拷贝 1080p 均值 0.92ms，缩放到 720p 均值 1.64ms；
  落在 TX 预算内，不是瓶颈。RGA 只服务非 NV12 采集格式。
- 后续关注：RGA 池耗尽与 CPU 回退计数（`tx_pipe[1s]` 已有统计），
  而非单次 blit 耗时。
