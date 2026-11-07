# debug — RK3588 采集/编解码最小复现程序

板端单文件 C 程序，`gcc` 直接编译，不经 meson，不依赖 MTL。
用途是在完整管线跑起来之前，先确认每一跳的单帧耗时与上限形态。
对应验证方案见 `docs/tests/rk3588-capture-encode-decode-perf-design.md`。

## 程序一览

| 文件 | 测量对象 | 依赖 |
|---|---|---|
| `v4l2_cap_latency.c` | V4L2 采集帧间隔与抖动 | 仅内核头（`linux/videodev2.h`） |
| `mpp_enc_frame_latency.c` | MPP 编码提交到输出耗时 | `librockchip_mpp` |
| `mpp_dec_frame_latency.c` | MPP 解码送入到取出耗时 | `librockchip_mpp` |
| `rga_blit_latency.c` | RGA 单次 blit 耗时 | `librga` + `libdrm`（dma32 堆） |

## 编译（板端执行）

板上建持久目录 `~/code/mtl`，二进制与数据长期保留，不用 `/tmp`：

```bash
mkdir -p ~/code/mtl/bin ~/code/mtl/streams
gcc debug/v4l2_cap_latency.c -o ~/code/mtl/bin/v4l2_cap_latency
gcc debug/mpp_enc_frame_latency.c -o ~/code/mtl/bin/mpp_enc_latency -lrockchip_mpp
gcc debug/mpp_dec_frame_latency.c -o ~/code/mtl/bin/mpp_dec_latency -lrockchip_mpp
gcc debug/rga_blit_latency.c -o ~/code/mtl/bin/rga_blit_latency -lrga -ldrm
```

## 运行

```bash
# 采集：默认 /dev/video0，采 300 帧
~/code/mtl/bin/v4l2_cap_latency --count 300 --csv ~/code/mtl/cap_interval.csv

# 编码：需要 NV12 文件（tests/perf/gen_test_stream.sh 生成）；BGR 直通加 --infmt bgr3
~/code/mtl/bin/mpp_enc_latency --input ~/code/mtl/streams/clock_1920x1080.nv12 \
  --w 1920 --h 1080 --frames 120 --bps 8000000 --csv ~/code/mtl/enc_cost.csv
~/code/mtl/bin/mpp_enc_latency --input ~/code/mtl/streams/clock_1920x1080.bgr3 \
  --w 1920 --h 1080 --frames 30 --bps 8000000 --infmt bgr3 --out ~/code/mtl/bgr_direct.h264

# 编码 4K：先生成 4K NV12（60 帧 @30fps），再硬编码产出 H264 供解码用
bash tests/perf/gen_test_stream.sh --4k --out ~/code/mtl/streams
~/code/mtl/bin/mpp_enc_latency --input ~/code/mtl/streams/clock_3840x2160.nv12 \
  --w 3840 --h 2160 --frames 60 --fps 30 --bps 40000000 \
  --out ~/code/mtl/streams/clock_3840x2160.h264 --csv ~/code/mtl/enc_4k_cost.csv

# 解码：需要 H264 Annex B 文件
~/code/mtl/bin/mpp_dec_latency --input ~/code/mtl/streams/clock_1920x1080.h264 \
  --csv ~/code/mtl/dec_cost.csv
# 解码 4K（推荐经 rx_decode_perf.py 走阈值判定）
~/code/mtl/bin/mpp_dec_latency --input ~/code/mtl/streams/clock_3840x2160.h264 \
  --csv ~/code/mtl/dec_4k_cost.csv

# RGA：1920x1080 NV12 自缩放
~/code/mtl/bin/rga_blit_latency --w 1920 --h 1080 --loop 200 --csv ~/code/mtl/rga_cost.csv
# RGA 4K：自拷贝 / 下缩放至 1080p / BGR3→NV12（生产采集路径）
~/code/mtl/bin/rga_blit_latency --w 3840 --h 2160 --loop 200 --csv ~/code/mtl/rga_4k_cost.csv
~/code/mtl/bin/rga_blit_latency --w 3840 --h 2160 --dw 1920 --dh 1080 --loop 200 --csv ~/code/mtl/rga_4k_scale.csv
~/code/mtl/bin/rga_blit_latency --w 3840 --h 2160 --srcfmt bgr3 --loop 200 --csv ~/code/mtl/rga_4k_bgr.csv
```

每个程序输出一行汇总（均值、p50/p90/p99），外加可选 CSV（`--csv FILE`）。
结果口径与 `tests/perf` 脚本的判定阈值对齐。

## 4K 补充（与 1080p 同口径，分辨率由参数指定）

- 编码：`--w 3840 --h 2160 --fps 30 --bps 40000000`（4K30 主测）或
  `--fps 60 --bps 40000000`（4K60 上限）；`--level` 留空即按分辨率自动选择
  （≤1080p 取 42 与生产一致，4K 取 51/52），也可用 `--level 51` 显式覆盖。
  输入 NV12 由 `tests/perf/gen_test_stream.sh --4k` 生成（60 帧 @30fps，
  约 746MB）；`--out OUT.h264` 可直接产出解码输入，避免板上 libx264 软编码。
- 解码：输入为上一步产出的 4K H264，`--max-au` 按文件 AU 数截断。
- RGA：`--w 3840 --h 2160` 自拷贝、`--dw 1920 --dh 1080` 下缩放、
  `--srcfmt bgr3` 覆盖生产 BGR 采集→NV12 路径（默认 `nv12` 保持旧行为）。
