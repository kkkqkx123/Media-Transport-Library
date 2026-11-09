# RK3588 采集+编码 / 收流+解码性能与延迟验证 · 第一轮实测（含 4K 补充）

日期：2026-11-07。板上持久目录 `~/code/mtl`（`bin/` 探针、`streams/` 预置流、`perf/` 原始结果）。
设计方案见 `docs/tests/rk3588-capture-encode-decode-perf-design.md`，本报告只归档结论与遗留问题。

> 4K 补充（同日）：采集链路存在源抖动导致的 STREAMON 后停流（问题 1），
> 按任务要求跳过采集性能测试，只补编码/解码/RGA 三项 4K 单点。

## 1 环境基线（`perf/env.json`）

- SoC `rockchip,rk3588`，8 核，8GB；内核 5.10.226；CPU governor ondemand（最大 1.8GHz）。
- MPP 为 2026-02-04 SDK 版本；rga_api 1.10.1；`mpi_enc_test` / `mpi_dec_test` 可用。
- HDMI 输入锁定 1080p60（2200x1125，60.01fps，polarities 正常，input status ok）。
- V4L2 当前格式 BGR3 1920x1080，驱动枚举支持 BGR3/NV24/NV16/NV12。

## 2 本轮结论（单次测量，待三轮收敛）

| 环节 | 输入 | 均值 | p50/p90/p99 | 状态 |
|---|---|---|---|---|
| MPP 编码 1080p CBR8M | 合成时钟流 120 帧 | 3.97ms | 4.00/4.08/4.25ms | 通过，远低于预算行 10~30ms |
| MPP 解码 1080p H264 | 同上转码 123AU，119 帧产出，0 反压 | 3.35ms | 3.55/3.57/5.31ms | 通过，低于预算行 10~30ms；首尾 4AU 无输出为 priming 现象 |
| RGA 自拷贝 1080p | 200 次 | 0.92ms | 917/937/988us | 通过 |
| RGA 缩放至 720p | 200 次 | 1.64ms | 1632/1652/1705us | 通过 |
| 空闲硬件负载 | rkvenc/rkvdec 0%，rkrga 仅自带测试负载 | — | — | 基线干净 |

预算拼表（发送侧典型值，采集段待补）：采集 ? ＋ RGA 约 1ms ＋ 编码约 4ms。
对照既有 70~170ms 端到端包络，编码与解码两段合计约 7~8ms（均值），余量充足。

## 2.1 4K 补充结论（单次测量为主，解码连跑 4 次确认稳定性）

| 环节 | 输入 | 均值 | p50/p90/p99 | 状态 |
|---|---|---|---|---|
| MPP 编码 4K30 CBR40M | 合成时钟流 60 帧 NV12（712MB），level 自动 51 | 14.15ms | 14085/14311/14435us | 通过，低于两帧间隔 66666us；rkvenc 负载接口读数 0%（短突发粒度不足，以耗时余量为准） |
| MPP 编码 4K60 CBR40M（摸高） | 同上，level 自动 52 | 14.10ms | 14083/14144/14195us | 通过，低于两帧间隔 33333us；单流 4K60 硬件可跟上 |
| MPP 解码 4K H264 | 上一步硬编码产出 64AU，59 帧产出，0 反压 | 6.4~6.5ms（3 次连跑稳定；另一次冷启动 2.9ms 未复现，按保守值采信） | 6456/6496/11358us（首帧 priming 约 21ms 推高 p99） | 通过，低于两帧间隔 66666us；no_output=5（SPS/PPS+priming，allowance 12 内） |
| RGA 自拷贝 4K | NV12 3840x2160，200 次 | 3.04ms | 3030/3056/3131us | 通过（阈值 12000us） |
| RGA 下缩放 4K→1080p | NV12 3840x2160→1920x1080，200 次 | 7.10ms | 7085/7107/7162us | 通过（阈值 12000us）；下缩放慢于自拷贝，系缩放滤波开销 |
| RGA 转换 BGR3→NV12 4K | BGR3 3840x2160（生产采集路径），200 次 | 4.68ms | 4653/4675/4722us | 通过（阈值 15000us） |
| 1080p 回归（新探针复测） | 同 round1 输入 | 编码 4.02ms / 解码 3.32ms / RGA 0.86ms | 与 round1 一致（±0.1ms） | 无回归；编码 level 自动 42 与生产一致 |

预算拼表（4K30 发送侧典型值，采集段跳过）：RGA（BGR→NV12）约 4.7ms ＋
编码约 14.2ms ≈ 19ms；解码约 6.5ms。编码+解码合计约 21ms（均值），
仍落在预算表编码 10~30ms / 解码 10~30ms 行内偏上，余量收窄但未超。
4K→1080p 下缩放 7.1ms 计入合成链时需单独留预算。

## 2.2 BGR3 格式补充（2026-11-09，补齐生产采集格式链路）

背景：生产 V4L2 采集实际协商格式为 BGR3（问题 2），生产链路走
BGR3 → RGA 转 NV12 → MPP 编码支路。round1 只有 4K RGA BGR3→NV12 单点，
本次补齐 RGA bgr 1080p 与**直通 BGR888 编码**（`mpp_enc_latency --infmt bgr3`，
MPP `MPP_FMT_BGR888` 直接吃 BGR dma-buf，跳过 RGA）两个口径，量化
"RGA 转换支路 vs 直通编码"的取舍。4K bgr3 输入由板上 ffmpeg 从既有
`clock_3840x2160.nv12` 转出（`-pix_fmt bgr24`，60 帧，1.5GB）。

| 环节 | 输入 | 均值 | p50/p90/p99 | 状态 |
|---|---|---|---|---|
| RGA 转换 BGR3→NV12 1080p | 200 次 | 2.37ms | 2373/2419/2529us | 通过；4K 同口径 4.68ms（round1 已测） |
| MPP 直通编码 1080p CBR8M BGR888 | clock_1920x1080.bgr3，120 帧 | 4.69ms | 4683/4837/4985us | 通过，低于预算行；比 NV12 编码（3.97ms）贵 ~0.7ms |
| MPP 直通编码 4K30 CBR40M BGR888 | clock_3840x2160.bgr3，60 帧，level 51 | 15.69ms | 15618/15924/16087us | 通过，低于两帧间隔 66666us；比 NV12 编码（14.15ms）贵 ~1.5ms |

链路对比（均值拼表）：

| 分辨率 | 生产支路 BGR→RGA→NV12→enc | 直通 BGR888→enc | 直通收益 |
|---|---|---|---|
| 1080p | 2.37 + 3.97 ≈ **6.3ms** | **4.69ms** | 省 ~1.6ms（-26%） |
| 4K30 | 4.68 + 14.15 ≈ **18.8ms** | **15.69ms** | 省 ~3.1ms（-17%） |

结论与口径说明：

1. BGR 输入帧是 NV12 的 1.5 倍大小（1080p 6.2MB vs 4.1MB），DMA 导入带宽
   更高，因此直通编码本身比 NV12 编码贵 0.7~1.5ms；但跳掉 RGA 转换步后
   **总链路仍净赚 1.6~3.1ms**，两档分辨率方向一致。
2. 生产改造点若要兑现直通收益：`tx_encode.c` 当前 `use_rga = !(NV12 && 单平面)`
   会把 BGR3 强制送 RGA；需让 `mpp_encoder` 支持 `prep:format BGR888` 直采
   dmabuf（探针已验证本版 MPP 支持 BGR888 编码出流，`bgr_direct.h264` 为其产物），
   并同步 VUI/色彩描述（full range BGR→YUV 由 MPP 内部完成）。
   **已落地（11-09）**：`codec_config.input_fourcc` + `mpp_encoder` BGR888 直采 +
   `tx_encode.c` BGR3 跳过 RGA，live_tx / dvs_media / playout_rx 交叉编译全部通过；
   板端实测（live_ingest 接 BGR3 采集源）待板卡可用后复测管线画像。
3. 4K 直通 15.7ms 与 NV12 口径一致地高于 <10ms 理论值（见
   `docs/tests/codec-latency-semantics.md` §五），格式选择不改变该结论；
   仍低于 4K30 两帧间隔，不构成吞吐瓶颈。
4. 判定阈值沿用：1080p 用 `enc_p99_max_us=33333`、`rga_bgr_convert_p99`
   (rga_4k 段 15000us 仅 4K) ；本轮 p99 全部通过，无新增阈值需求。

## 3 未完成项与问题清单

1. V4L2 采集间隔暂缺（已定位到信号源，非程序问题）。过程：首版探针 S_FMT 未填
   `num_planes`（对照实现要求置 1），NV12 被拒曾被误判为驱动行为；修正并对齐
   参考实现后复测，NV12/NV16/NV24 仍被拒绝（驱动真实行为），BGR3 协商成功、
   REQBUFS 获批 6 个、STREAMON 返回 0，但 poll 无帧。随后对照发现 `v4l2-ctl
   --stream-mmap` 同样失败（`DQBUF: Invalid argument`，dmesg 报 `timing is
   invalid`），而 dmesg 显示 `format_change` 与 `signal lock ok` 约每 1~2 秒交替
   出现：源端抖动导致驱动在 STREAMON 时刻判定失效。单次 `query-dv-timings`
   快照会返回缓存的 1080p60 值，具有误导性。待源端稳定后重采。
   **本轮 4K 按任务要求跳过采集性能测试，不阻塞编码/解码/RGA 结论。**
2. NV12/NV16/NV24 的 S_FMT 被拒绝为驱动真实行为（已排除程序因素）；BGR3 可协商，
   生产采集器应走 BGR3＋RGA 转换支路（与参考实现的格式决策一致）。
   4K RGA 已补 BGR3→NV12 实测 4.68ms，覆盖该支路。
   **bgr3 补充（11-09）**：另测得直通 BGR888 编码口径（见 §2.2），两档分辨率下
   跳过 RGA 净省 1.6~3.1ms；是否切直通取决于生产改造点 3（encoder 支持 BGR888
   dmabuf 直采），与问题 6 的 level 跟进可一并改造复测。
3. TX/RX 应用二进制板上缺失，管线画像与单板回环 E2E 暂不可跑，需交叉编译产物；
   解码侧已按预案用预置数据完成离线覆盖（含 4K）。4K 管线画像待 `live_ingest`
   产物到位后跑文件源（`--source file --mp4 <4K_MP4>`），本轮只到单点探针。
4. `mpi_enc_test` / `mpi_dec_test` 交叉对比输出为空，需带显式参数重跑一次互验。
5. 本轮为单次测量，阈值收敛需要两轮复测后再定（4K 阈值 `thresholds.json` 中
   `tx_encode_4k` / `rx_decode_4k` / `rga_4k` 为初值，已按本轮实测回填）。
6. 生产编码器 `h264:level` 固定 42 只够 1080p，4K 需 51/52：探针已按分辨率自动
   选择（本轮 4K30 level=51、4K60 level=52 均正常出流）；生产侧
   `app/rk3588_live_tx/codec/mpp_encoder.c` 需跟进同规则修改后复测 4K 管线。
7. 离线探针按 start code 切 AU 含 SPS/PPS 非帧 AU，`framed/au_fed` 永不到 100%
   （本轮 4K 为 59/64=92%），不按 MTL 管线 99% 判定；`rx_decode_perf.py` 已改为
   `put_fail==0 且 no_output≤12` 判定，设计方案 §5 同步更新。
8. 4K 解码均值存在冷热差异：同文件连跑 4 次，3 次稳定 6.4ms，首次 2.9ms 未复现；
   本报告按保守值 6.4ms 采信，待三轮收敛时观察 governor/时钟门控影响。

## 4 原始结果位置

板上 `~/code/mtl/perf/`：`env.json`、`cap_interval.csv`（空，采集跳过，待问题 1 解决后重采）、
`enc_cost.csv`、`dec_cost.csv`、`rga_cost.csv`、`round1_*.log`；4K 新增
`enc_4k_cost.csv`、`enc_4k60_cost.csv`、`dec_4k_cost.csv`、`rga_4k_cost.csv`、
`rga_4k_scale.csv`、`rga_4k_bgr.csv`、`rx4k/rx_decode.json`、`hw_4k_enc.json`；
bgr3 补充（11-09）新增 `rga_bgr_cost.csv`（1080p RGA BGR→NV12）、
`enc_bgr_cost.csv`（1080p 直通 BGR 编码）、`enc_4k_bgr_cost.csv`（4K30 直通），
板上 `~/code/mtl/streams/` 新增 `clock_3840x2160.bgr3`（1.5GB，60 帧 @30fps，
ffmpeg 由 NV12 转出）。CSV 与流文件保留在板上，不进仓库。

## 5 下一步

按序处理：等源端稳定后重采（问题 1）→ 两轮复测收敛阈值（含 4K 初值）→
生产编码器 level 跟进 51/52 规则后复测 4K 管线 → 交叉编译产物到板后跑
管线画像与 E2E 回环（含 4K 文件源 `--source file --mp4`）。
