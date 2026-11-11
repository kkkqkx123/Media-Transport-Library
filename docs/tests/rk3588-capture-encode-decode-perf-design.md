# RK3588 采集+编码 / 收流+解码性能与延迟验证方案

目标：量化两条预算占用，为分布式视频墙端到端时延预算提供实测依据。

- 发送侧：HDMI 采集 + RGA 转换 + MPP 编码耗时（含抖动与上限形态）。
- 接收侧：MTL 收流 + MPP 解码 + 呈现耗时（含抖动缓冲与解码吞吐）。
- 端到端：将两者拼成完整预算表，与既有整墙时延包络对照，确认余量。

对照基线是既有端到端技术方案中的时延预算表（采集 16~33ms、编码 10~30ms、
网络 1~5ms、抖动缓冲 20~50ms、解码 10~30ms、合成 5~10ms、显示 8~16ms，
合计 70~170ms）。本方案不改动该表，只负责把其中的采集、编码、收流、解码
四项从经验值换成板端实测值。

测试思想沿用 zlmediakit 的分层做法（环境基线、硬件单点上限、管线负载画像、
系统组合负载），但测量对象换成 MTL 两端应用自带的统计口径，延迟部分为本次新增。

## 1 现状能力盘点

发送端（`app/rk3588_live_tx`）已有可直接复用的统计：

- 采集：`video_fps`、`cap_interval_us`（中位帧间隔）、`effective_fps`、
  `full_events`（采集池满）、`ts_clamp` / `ts_fallback`（驱动时间戳异常计数）。
- RGA：`calls` / `fails` / `pool_exhausted` / `cpu_fallbacks`、`time_ema_us`。
- 编码：`encode_fps`、`encode_cost_us`（提交到输出的 EMA）、
  `latency_p50/p90/p99_us`（近 300 帧窗口分位）、`avg_qp` / `bps_rt`、
  `mpp_buf_now/peak`、各类丢帧计数。`tx_pipe[1s]` 每秒打印一行，
  可直接作为采样源，无需改代码即可做黑盒测量。

接收端（`app/rk3588_playout_rx`）已有可直接复用的统计：

- 收流：`mtl_rx` 包数、`bridge` 帧数、`mtl_drop` / `br_drop` / `corrupt` /
  `incomplete`，周期打印在 `RX stats` 块中。
- 呈现与缓冲：`dec` / `render` 帧数、`pb_skip` / `pb_underrun`（抖动缓冲
  跳帧与欠载）、`put_fail`（解码反压）。
- 端到端：`rx_e2e_stats` 记录 SEI 携带的采集 TAI 戳到 DRM 呈现时刻的差值，
  输出 min/avg/max/count/skew_reject。单板回环时收发同机可用单调时钟直接相减；
  双板对比时需要 PTP 对齐，RX 已支持外部 `time_now` 注入。

当前缺口（本方案要补的）：

- 解码单帧耗时没有统计。编码侧有提交到输出的 cost EMA 与分位，
  解码侧只有计数器。需要新增送入到取出的逐帧打点，才能拆出解码预算。
- 发送侧采集戳与编码输出之间没有逐帧配对。`cap_interval_us` 与
  `encode_cost_us` 是两套独立统计，算和时只能按均值相加，抖动叠加关系不清。
  本方案先用均值相加加分位上界做保守估计，逐帧配对作为后续增强。
- 跨机时钟：双板延迟对比依赖 PTP。若 PTP 未就绪，只能测单板回环延迟，
  跨机部分用单板值加网络包络估计。

## 2 采集指标定义

所有指标均为运行态只读采样，不插桩数据面。单位与口径在脚本阈值文件中统一。

发送侧（黑盒解析 `tx_pipe[1s]` 日志行，外加硬件负载采样）：

| 指标 | 含义 | 判定用途 |
|---|---|---|
| video_fps / effective_fps | 驱动实际帧率与 G_PARM 回读帧率 | 信号是否锁相 ITU 1080p，采集是否掉帧 |
| cap_interval_us | 中位帧间隔 | 采集抖动，目标 16666us 上下小幅波动 |
| full_events 增量 | 采集池满次数 | 编码跟不上采集的直接证据 |
| ts_clamp / ts_fallback 增量 | 时间戳修正计数 | 驱动时间戳是否可信，不可信则 E2E 戳同步打折 |
| rga time_ema_us | RGA 单次转换 EMA | 转换耗时预算 |
| rga pool_exhausted / cpu_fallbacks | RGA 池耗尽与 CPU 回退 | 是否触 RGA 上限 |
| encode_fps | 编码输出帧率 | 是否跟上目标帧率（按 95% 判定） |
| encode_cost_us EMA | 提交到输出均值 | 编码耗时预算主项 |
| latency p50/p90/p99 | 近帧窗口分位 | 编码抖动，p99 决定缓冲下限 |
| avg_qp / bps_rt | 实际 QP 与码率 | 码控是否收敛，压力是否导致画质崩 |
| ring_drop / sink_drop / oversize | 各级丢帧 | 瓶颈定位到哪一级队列 |
| rkvenc load avg/max | 硬件编码器负载 | 区分硬件上限与软件管道瓶颈（max 达 90% 才算硬件触顶） |
| rkrga load avg/max | RGA 负载 | 与编码争用判断 |
| CPU pct | 整机 CPU | 排除 CPU 干扰 |

接收侧（黑盒解析 `RX stats` 块与 E2E-C 行，外加解码打点）：

| 指标 | 含义 | 判定用途 |
|---|---|---|
| mtl_rx / bridge / dec / render 增量 | 各级产出帧数 | 收流链条是否等速，有无级联丢失 |
| mtl_drop / br_drop / corrupt / incomplete | 收流丢包与坏帧 | 网络与组包质量 |
| pb_skip / pb_underrun | 抖动缓冲跳帧与欠载 | playout_delay 是否偏小 |
| put_fail | 解码反压 | 解码是否成为瓶颈 |
| decode cost EMA 与 p50/p90/p99 | 送入到取出耗时 | 解码耗时预算主项（本次新增打点） |
| E2E-C min/avg/max/count | 采集戳到呈现差值 | 端到端实测值 |
| skew_reject | 时钟偏斜拒绝数 | 非零说明跨机时钟不可比，本轮 E2E 作废 |
| rkvdec load avg/max | 硬件解码器负载 | 解码触顶判断 |

## 3 数据源提供方式

三种数据源，按可复现性从低到高排列。延迟预算以真实 HDMI 源为准，
预置数据源用于解码上限与回归对比。

### 3.1 真实 HDMI 源（默认，主结论来源）

当前开发板 hdmiin 已有 1080p 信号输入。发送端走默认 HDMI 路径：

```bash
./build/live_ingest 30 --udp 15000 --dip <对端IP>
```

板上 `/dev/video0` 实测 NV12/NV16/NV24 的 S_FMT 均被驱动拒绝（`num_planes` 置位后
复测依旧，为驱动真实行为），协商只能落在 BGR3（UYVY/YUYV 同样被拒，矩阵脚本已覆盖）。发送侧
保持 RGA+NV12 路径：BGR3 经 RGA 转 NV12 后送编码器，实测转换约 1ms。BGR 直投
编码器虽在板上验证可行（`prep:format=BGR888` 可出流、可解码），但与 RGA 路径
相比无性能优势，维持现状以避免额外分支。格式取舍详见 issue 归档。每次测试前
除 `v4l2-ctl --get-fmt-video` 与 DV 时序查询外，必须按 issue 归档做源端预检：
dmesg 10 秒静默窗口、两次时序快照一致、显式固定 BGR3 后试采（最多重试 3 次）。
注意试采本身就可能撞上 `DQBUF: Invalid argument`：S_FMT 成功后报此错说明
STREAMON 之后源抖动致驱动停流（环境问题，重试）；S_FMT 已失败后报此错只是
队列没建对（看 S_FMT 那行即可）。分类不清先查 issue，不要改程序。
信号有效是 L2/L3 的前置条件，无信号时发送端重试、接收端采不到数，
此时只能跑预置数据源部分。

### 3.2 预置文件源（解码上限与无信号回归）

发送端支持文件帧源插件，MP4 解复用后喂给同一发送宿：

```bash
./build/live_ingest 30 --source file --mp4 <FILE> --loop --udp 15000 --dip <对端IP>
```

接收端解码上限用预置 H264 Annex B 文件直测（`debug/mpp_dec_frame_latency`
与 `tests/perf/rx_decode_perf.py --mode offline`），输入由
`tests/perf/gen_test_stream.sh` 用 ffmpeg 合成（时钟条纹、静态色条、
运动块、噪点四种图案，NV12 与 H264 各一版），或从 HDMI 录制一帧转储。
该方式隔离了采集抖动，测出的是解码器本身的吞吐与单帧耗时。

合成图案的选择沿用 zlmediakit 的经验：时钟条纹做编码基线（P 帧高效，
最接近真实场景），噪点叠加运动块做上限压力（帧间差异大）。

4K 档（3840x2160，本方案新增）：只生成时钟条纹一种图案，60 帧 @30fps
（NV12 约 746MB），H264 优先用板上 `mpp_enc_latency --out` 硬编码产出
（比板上 libx264 软编码快一个数量级，且与生产编码器同源）；
`gen_test_stream.sh --4k` 按需生成，默认 1080p/720p 流程不受影响。
4K 编码主测 30fps（CBR 40M，对齐 `live_ingest` 默认码率），60fps 作上限摸高；
4K 解码输入即上一步产出的 H264，`--max-au` 按实际 AU 数截断。
生产编码器 `h264:level` 固定 42 只够 1080p60（MaxFS 8192 MBs），
4K 需 51（≤30fps）/ 52（>30fps）：探针按分辨率自动选择，
生产侧跟进改为同规则（见 §8）。

### 3.3 MTL 网络流（收流测试输入，可替换为预置数据）

收流测试默认输入是发送端发出的真实 ST2110-22 组播流：

```bash
./build/file_playout_rx 30 --udp 15000 --sip <发送端IP> --width 1920 --height 1080 --fps 60
```

当网络或发送端不可用时，允许降级为预置数据：发送端改用文件源发射
（仍是 MTL 流，只是内容可复现），或接收端改离线回放此前转储的
`rx_capture.h264`（`FILE_PLAYOUT_DUMP_RX` 机制）。验证报告必须注明
本轮用的是哪种输入，三种输入的解码耗时结论不可直接混排。

## 4 延迟测量方法

延迟拆成五段，分别对应预算表的行。测量全部基于单调时钟或 PTP，
不引入 wall clock。

- 采集段：`cap_interval_us` 中位间隔即一帧等待；`full_events` 非零说明
  排队已发生，预算需加一帧。
- 转换段：`rga time_ema_us` 直接取数。
- 编码段：`encode_cost_us` EMA 为均值预算，`latency_p99` 为抖动上界。
  发送侧预算取两者组合：均值相加用于典型预算，p99 相加用于最坏预算。
- 网络与抖动缓冲段：局域网按 1~5ms 估计；`playout_delay_ms` 配置值即缓冲预算，
  用 `pb_underrun` 是否为零验证配置是否充分。欠载非零则加档重试。
- 解码与呈现段：本次新增解码送入取出打点（EMA 与分位），呈现到 DRM
  提交时刻由 E2E-C 覆盖。E2E-C 减去前四段即含解码呈现的实测余量，
  可与解码打点互相校验。

时钟规则：单板回环（发送与接收同机）直接用单调时钟，E2E-C 可信；
双板对比必须确认 PTP 已同步且 `skew_reject` 为零，否则本轮 E2E-C 只记录
不采信，预算表填单板值加网络包络。

上限与预算的区分：`rkvenc load max` 达 90% 才认定为硬件编码上限，
否则 fps 受限于软件管道。预算表填的是当前配置下的实测耗时，
上限测试回答的是还有多少余量（多路并发或升分辨率时用）。

## 5 测试分层与矩阵

- 环境基线：每次必跑，记录板型、内核、MPP 与 librga 版本、V4L2 时序、
  CPU governor。版本不同则上限不可比。
- 硬件单点上限：`mpi_enc_test` 编码压测（1080p/720p）、
  `mpp_enc_frame_latency` 编码探针（1080p CBR8M；4K CBR40M，level 自动 51/52）、
  `mpp_dec_frame_latency` 解码压测（预置 H264，1080p 与 4K 各一版）、
  `rga_blit_latency` 转换压测（1080p 自拷贝/缩至 720p；4K 自拷贝、
  4K→1080p 下缩放、BGR3→NV12 生产路径）。输出 fps 与单帧耗时，判定触顶形态。
- 管线负载画像：发送端跑码率矩阵（1080p：4M/8M/20M/40M；4K：20M/40M）与 GOP 矩阵，
  每档稳定后采样窗口，记录编码 cost、p99、丢帧、rkvenc 负载；
  接收端跑预置流解码矩阵（1080p 与 4K 各一版），记录解码 cost 与产出比。
  4K 管线画像需 `live_ingest` 交叉编译产物到位后跑文件源
  （`--source file --mp4 <4K_MP4>`，HDMI 4K 信号暂无），本轮先跑单点探针。
- 端到端预算：单板回环跑 TX+RX，采集预算表；双板跑 TX→网络→RX，
  在 PTP 就绪时采 E2E-C。输出预算表与余量结论。

通过标准初值（1080p60，CBR，GOP 60，收敛三轮后调整）：

- 采集：video_fps 达目标 98% 以上，full_events 零增长。
- 编码：encode_fps 达目标 95% 以上，p99 不超过帧间隔两倍，
  rkvenc max 低于 90%（预算测试不要求打满）。
- 收流：各级产出比 99% 以上，pb_underrun 零增长。
- 解码：frames_out 与输入 AU 比 99% 以上，put_fail 零增长。
- 端到端：E2E-C avg 落在预算表内，skew_reject 为零。

4K 通过标准（CBR40M，GOP 60，阈值见 `thresholds.json` 中 `*_4k` 段）：

- 编码：60 帧窗口 encode_fps 达目标 95% 以上；p99 不超过两帧间隔
  （30fps 档 66666us，60fps 档 33333us）；rkvenc max 低于 90%。
- 解码：put_fail 为零；无输出 AU（no_output）≤12（SPS/PPS/SEI 非帧 AU +
  解码器 priming，不按 MTL 管线产出比判定）；p99 不超过两帧间隔
  （同上按 fps 取档）。4K 解码首帧 cost 偏高为 priming 现象，计入 p99 但不单独判定。
- RGA：200 次循环 fail 为零；p99 初值自拷贝/下缩放 12000us、
  BGR 转换 15000us 以内（三轮收敛后按实测收紧）。

## 6 执行流程

全程在开发板上执行，主机只做 ssh 触发与结果回收。Python 脚本只依赖标准库。

板上建持久目录 `~/code/mtl`（结果与合成流长期保留，不用 `/tmp`）：

```bash
ssh board 'mkdir -p ~/code/mtl/perf ~/code/mtl/streams'
ssh board 'cd <repo> && python3 tests/perf/common/env_probe.py --out ~/code/mtl/perf/env.json'
ssh board 'cd <repo> && python3 tests/perf/tx_capture_encode_perf.py --duration 30 --out ~/code/mtl/perf/tx'
ssh board 'cd <repo> && python3 tests/perf/rx_decode_perf.py --mode offline --input ~/code/mtl/streams/clock_1920x1080.h264 --out ~/code/mtl/perf/rx'
ssh board 'cd <repo> && python3 tests/perf/e2e_latency_budget.py --duration 60 --out ~/code/mtl/perf/e2e'
# 4K 单点（采集跳过，见 §8）：预置流按需生成，编码硬产出 H264，解码走探针判定，RGA 三组
ssh board 'cd <repo> && bash tests/perf/gen_test_stream.sh --out ~/code/mtl/streams --4k'
ssh board '~/code/mtl/bin/mpp_enc_latency --input ~/code/mtl/streams/clock_3840x2160.nv12 --w 3840 --h 2160 --frames 60 --fps 30 --bps 40000000 --out ~/code/mtl/streams/clock_3840x2160.h264 --csv ~/code/mtl/perf/enc_4k_cost.csv'
ssh board 'cd <repo> && python3 tests/perf/rx_decode_perf.py --mode offline --input ~/code/mtl/streams/clock_3840x2160.h264 --dec-binary ~/code/mtl/bin/mpp_dec_latency --width 3840 --height 2160 --fps 30 --out ~/code/mtl/perf/rx4k'
ssh board '~/code/mtl/bin/rga_blit_latency --w 3840 --h 2160 --loop 200 --csv ~/code/mtl/perf/rga_4k_cost.csv'
ssh board '~/code/mtl/bin/rga_blit_latency --w 3840 --h 2160 --dw 1920 --dh 1080 --loop 200 --csv ~/code/mtl/perf/rga_4k_scale.csv'
ssh board '~/code/mtl/bin/rga_blit_latency --w 3840 --h 2160 --srcfmt bgr3 --loop 200 --csv ~/code/mtl/perf/rga_4k_bgr.csv'
```

每轮输出 JSON 原始结果与 Markdown 报告到 `--out` 目录（板上 `~/code/mtl/perf`），
结论汇总后归档到 `docs/archive/perf/`（按日期命名，不改写本方案）。压测与运行中业务会争用硬件，
对比采集指标前需停业务或记录负载基线。

每步期望结果：`env.json` 含板型/内核/MPP/时序且无空字段；TX 脚本 `verdict` 为
`PASS`（`encode_fps` 达目标 95% 以上、p99 不超两帧间隔）；RX 离线 `fps` 正常、
MTL 模式产出比 99% 以上且 `pb_underrun` 零增长；E2E 的 `E2E-C avg` 落在包络内且
`skew_reject` 为零。任一步不通过时，先按 issue 归档排查环境（源抖动/格式变化），
不要直接判程序问题；源端预检与格式协商的详细步骤见两份 issue 文档。

## 7 交付文件映射

- `debug/`：板端最小复现程序，gcc 单文件直编，不经 meson。
  采集耗时、编码单帧、解码单帧、RGA 单次各一个，外加本目录说明。
  4K 由参数指定：编码 `--w/--h/--fps/--bps/--level`（level 留空自动 51/52），
  RGA `--srcfmt bgr3` 覆盖生产路径，解码输入为 4K H264。
- `tests/perf/`：板端编排脚本，common 放环境探测、负载采样、报告生成与阈值
  （`thresholds.json` 含 `tx_encode_4k` / `rx_decode_4k` / `rga_4k` 三段）；
  发送、接收、端到端各一个驱动脚本（接收侧 `--dec-binary` 切探针取分位）；
  `gen_test_stream.sh --4k` 按需合成 4K 时钟流（60 帧 @30fps）。
- 本目录：本方案文档。后续结论报告按轮次新增归档到 `docs/archive/perf/`，不改写本方案。

## 8 风险与后续

- HDMI 源格式变化（BGR3/NV12/DV 时序）会改变 RGA 路径，首轮先记录协商结果。
  当前采集链路存在源抖动导致的 STREAMON 后停流（源端稳定后重采通过，round1 归档
  已随旧口径文档清理），
  本轮 4K 验证跳过采集性能测试，只跑编码/解码/RGA 三项。
- 生产编码器 `h264:level` 固定 42 为 1080p 遗留：4K（>2073600 像素）需按
  fps 切 51/52，否则 MPP 可能拒配或码控失真。探针已按此规则自动选择，
  生产侧 `app/rk3588_live_tx/codec/mpp_encoder.c` 需跟进同规则修改后复测 4K 管线。
- 4K 文件体积大（NV12 60 帧约 746MB）且板上 libx264 软编码慢：H264 以
  `mpp_enc_latency --out` 硬编码产出为准，`gen_test_stream.sh` 只负责 NV12 合成。
- PTP 未就绪时双板 E2E 不可采信，先保证单板回环结论。
- 音频链路不在本次预算内，视频结论先行。
- 多路并发与 4 路合成压力为后续扩展，本次 4K 矩阵只覆盖单路 4K30 主测 + 4K60 摸高。
