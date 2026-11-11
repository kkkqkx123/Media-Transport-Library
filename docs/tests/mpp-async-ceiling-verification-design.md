# MPP 编解码异步吞吐上限验证方案

日期：2026-11-11。参考对象：`~/code/zlmediakit` 的 `tests/perf` 分层压测与
`src/HdmiIn/encode/HdmiInAsyncEncoder.{h,cpp}`（送帧线程 + 取包线程 + 有界环，
采集线程只管 `TrySubmit`，VPU 在两帧之间不空转）。

本方案回答两个问题：

1. 生产管线是否需要补充类似的异步编码 / 异步解码？
2. 现有基准测试如何补充，才能测出 MPP 的真实负载与吞吐上限（而非同步往返延迟）？

验证结论只进 `docs/archive/perf/` 按轮归档，不改写本方案。

## 1 现状分析（静态结论，板上实测见 §5）

### 1.1 发送侧编码：已是异步形态，无需新增模块

生产编码器（`app/rk3588_live_tx/codec/mpp_encoder.c`）与 ZL 异步编码器同构，
只是 feeder 的身份不同：

| ZL `HdmiInAsyncEncoder` | 本项目 `mpp_encoder.c` + `tx_encode.c` |
|---|---|
| 采集线程 `TrySubmit`（环满丢帧，永不阻塞） | 采集回调 `tx_encode_on_frame` → `ring_cap2enc`（满则 `ring_drops`，永不阻塞） |
| 有界环（深度 4~16，`Occupancy/Peak/FullDrop` 可观测） | 编码器内环（`ENC_RING_DEPTH` 8，`full_drops` 可观测） |
| feeder 线程 `PutFrameBlocking` | `tx_encode` 线程 `enc_send`（`encode_put_frame`，MPP 内部队列吸纳突发） |
| collector 线程 `CollectOneBlocking` | `collector_thread`（`encode_get_packet` 轮询 + 空闲 500us 睡眠） |
| `Stop` 时排空 MPP 残留包 | `enc_stop` join collector；`DrainPackets` 语义由环内余帧覆盖 |

关键证据是输入输出已解耦：`enc_send` 只管 put（MPP 忙时返回 `-EAGAIN` 计
`send_fails`），`enc_recv` 只从环里 pop（1ms 有界等待计 `recv_timeouts`），
MPP 硬件在 `tx_encode` 线程做 RGA 转换与 MTL 发布的同时持续产出。
round2 实测 `enc_fps 57.7/60`、`p99 6.3ms` 与单点 `mean 3.97ms` 的关系也自洽：
管线吞吐已脱离同步逐帧往返，进入硬件流水区间。

因此发送侧**不新增异步编码模块**。与 ZL 的差距只在可观测性与整定，
不在架构：`ENC_RING_DEPTH` 写死 8（ZL 环深 4~16 可配）、采集到编码输入的
`ring_cap2enc` 深度只有 2~3 且无满水位遥测。后续如需整定，只调参、不加模块。

补充（11-11 实测）：单会话 MPP 编码是严格串行服务（1/service 即上限，
1080p sync 247fps、4K sync 71fps），排队加深不提速。async 探针在板载 MPP
构建（nyanmisaka fork，刚改过 async input task 计数）上呈随机多秒输出
间隙：猝发提交偶发卡住，串行提交 2000 帧零间隙、持续负载下 `rkvenc 53%`
读数正常。因此编码上限以 sync 口径为准，async 只作管线形态诊断（槽超时
自愈把停顿转成可数的 drop，不死锁；`drops=0` 的 run 分位才可采信）。
生产串行 send→recv 不受影响（round2 TX 57.7fps/30s 为证）。

### 1.2 接收侧解码：生产保持同步，先补异步压测再定

生产解码器（`app/rk3588_playout_rx/shared/mpp_decoder.c`）是同步形态：
`decode_thread` 每包执行 put 整 AU → 立即 drain 取帧（`rx_feed_packet`），
`mpp_dec_put_au` 内部 put 遇阻还带 12 次重试 + 回压排空。探针
`debug/mpp_dec_frame_latency.c` 同样是单 AU 提交等待（`wait_ms` 50ms 兜底）。
两者测的都是提交到取出的往返延迟，不是解码器吞吐上限。

ZL 只有异步编码、没有异步解码（解码侧靠多路并发与呈现链路覆盖），
所以"解码同理"不是照抄一个模块，而是照抄测量思想：先用异步流水探针
测出单会话解码吞吐上限，再看生产同步链路是否触及该上限。
只有当异步上限明显高于生产实际吞吐、且生产侧出现 `put_fail` /
`drain_pull` 堆积时，才考虑生产解码异步化。在此之前**不动生产解码代码**。

定案（11-11，正式决策见 `docs/plan/rk3588/RK3588_DECODE_ASYNC_PLAN.md`）：
上限复现（+27~32%，跨启动稳定）。但并发对照（1080p x4 交错复测两轮）
证明**多会话下 sync 全面反超 async**（aggregate 高约 20%，stretch
0.58 vs 0.40）——并发 sync 已打满 HW，生产吞吐侧**不替换**。
单流保持 sync；shallow-async（depth 2~4）仅作为网络抖动 A/B 胜出后
的候选项（文件输入下 `put_fail` 全零，抖动项外推不出，必须实测）。

### 1.3 现有探针的缺口：同步口径量的是延迟，不是上限

`mpp_enc_frame_latency`（提交→轮询→输出，逐帧串行）与 ZL 文档指出的
`mpi_enc_test` simple 模式问题完全一致：送一帧等一帧，每帧都同步等待硬件
完成，fps 反映的是同步往返延迟而非硬件吞吐。round1/round2 的编码单点
（1080p 3.97ms、4K30 14.2ms）与解码单点（1080p 3.35ms、4K 6.5ms）都是该口径。
4K60（budget 33ms）下同步口径余量看起来很薄，但可能是测量方法的人为瓶颈。
要回答"MPP 负载与吞吐"，必须补异步流水口径：双线程全速 put/get，
输入输出完全流水，用 `/proc/mpp_service/load` 的 rkvenc/rkvdec 负载判定
饱和（`max ≥ 90%` 为 `HW_CEILING`，未饱和为 `PIPELINE_THROUGHPUT`，
语义与 ZL 的 `mpp_ceiling_async.py` 一致）。

异步口径的时延数字是提交到输出的管道停留（含排队），天然大于同步口径的
服务耗时，不可直接对比，更不可套用"两帧间隔"阈值。判定只看吞吐与负载，
时延分位仅记录。

## 2 补充方案（只改探针与脚本，不动生产代码）

- 探针原地扩展（同一模块内加模式分支，不新增替代模块）：
  `debug/mpp_enc_frame_latency.c` 与 `debug/mpp_dec_frame_latency.c` 各加
  `--mode sync|async`（默认 sync，现有脚本与正则零改动）。
  sync 行为与输出格式逐字节不变；async 走 feeder/collector 双线程。
- 编排脚本新增 `tests/perf/mpp_async_ceiling.py`（与 `mpp_ceiling.py` 并列，
  不是替代）：负责板端编译探针、跑 sync/async 对比矩阵、后台采样硬件负载、
  输出 JSON + Markdown，与 `tx_capture_encode_perf.py` 共用
  `common/hw_load_sample.py` 与 `common/report.py`。
- 阈值文件加 `async_ceiling` 段（吞吐比、解码 allowance、饱和线），见 §4。

编码 async 口径设计：8 缓冲轮转，输入帧预读进内存消除文件抖动，
feeder  memcpy + `encode_put_frame`（MPP 忙时 2ms 退避重试），collector
`encode_get_packet` 100us 空闲轮询；提交戳按输出顺序配对（CBR 基线无 B 帧，
单会话保序），前若干样本作 priming 剔除；统计 wall fps 与提交到输出分位。

解码 async 口径设计：整文件预读 + AU 偏移表一次切分，feeder 每 AU 一个
packet 提交（`mpp_packet_set_pts` 写入 AU 序号，`put` 遇阻毫秒退避重试），
collector `decode_get_frame` 轮询，用帧 PTS 回配提交戳（PTS 无效时退化为
顺序配对，首批样本剔除覆盖）；feeder 末尾发 EOS，collector 排空后退出；
`no_output = fed - framed` 沿用 12 个 allowance（含 SPS/PPS/SEI 非帧 AU 与
priming，与 `rx_decode_perf.py` 同口径）。

## 3 测量矩阵（单板，文件源，无需 HDMI）

| 组 | 输入 | 编码/解码 | 帧数 | 目标 fps |
|---|---|---|---|---|
| enc-1080p | `clock_1920x1080.nv12` | CBR8M，level 42 | 120 | 60 |
| enc-4k30 | `clock_3840x2160.nv12` | CBR40M，level 51 | 60 | 30 |
| dec-1080p | `clock_1920x1080.h264` | split_parse=1（与生产一致） | 全部 AU | 60 |
| dec-4k | `clock_3840x2160.h264` | 同上 | 全部 AU | 30 |

每组跑 sync 一次 + async 一次（含探针内 warmup 剔除），driver 另做 2 次
async 重复取中位数。负载采样间隔 200ms（`load_interval` 由采样函数写入）。
4K NV12 约 746MB，预读窗口取 32 帧循环（1080p 约 100MB，4K 约 400MB），
不整文件常驻。

## 4 判定口径（`thresholds.json` 的 `async_ceiling` 段）

- 编码：`async_fps / 目标fps ≥ enc_fps_ratio (0.95)` 为 PASS；
  `rkvenc max ≥ saturation_pct (90%)` 记 `HW_CEILING`（该 fps 即单会话硬件上限），
  否则记 `PIPELINE_THROUGHPUT`（软件管道先触顶）。
- 解码：`put_fail == 0` 且 `no_output ≤ no_output_allowed (12)` 且
  `async_fps / 目标fps ≥ dec_fps_ratio (0.95)` 为 PASS；
  `rkvdec max ≥ 90%` 同理分类。
- 任一 FAIL 即 driver exit 1；WARN 保留给"通过但余量薄"（async  fps 仅略高于目标）。

## 5 执行流程（板端，持久目录 `~/code/mtl`）

板上只有 `src/{debug,perf}` 快照（`tests/perf` 对应板上 `src/perf`），
先同步变更文件，再编译探针，最后跑 driver（一轮约 5 分钟）：

```bash
# 同步（主机执行）：探针 + driver + 阈值
scp debug/mpp_enc_frame_latency.c debug/mpp_dec_frame_latency.c \
  board:~/code/mtl/src/debug/
scp tests/perf/mpp_async_ceiling.py board:~/code/mtl/src/perf/
scp tests/perf/common/thresholds.json board:~/code/mtl/src/perf/common/

# 编译（板端执行，沿用探针头注释的 gcc 直编口径）
gcc -O2 ~/code/mtl/src/debug/mpp_enc_frame_latency.c \
  -o ~/code/mtl/bin/mpp_enc_latency -lrockchip_mpp -I/usr/include/rockchip
gcc -O2 ~/code/mtl/src/debug/mpp_dec_frame_latency.c \
  -o ~/code/mtl/bin/mpp_dec_latency -lrockchip_mpp -I/usr/include/rockchip

# 对比矩阵（板端执行）
python3 ~/code/mtl/src/perf/mpp_async_ceiling.py \
  --matrix "1920x1080,3840x2160" --repeat 2 --out ~/code/mtl/perf/async_ceiling
```

期望结果：每组输出 sync/async 两行（fps、mean、分位、负载、分类），
`async fps` 应显著高于 `sync fps`（消除同步往返等待）；
1080p 两组负载不饱和（`PIPELINE_THROUGHPUT`），4K 两组看负载定分类。
任一组 FAIL 时先查输入文件 md5 与 `env_probe.py` 基线，再怀疑程序。

## 6 输入源保真度：dma-buf 直投（`--src dmaheap`）

round1 的 file 模式探针是 CPU memcpy 进 MPP 内部分配缓冲，生产却是
RGA 池 dma-buf 经 `mpp_buffer_import(EXT_DMA)` 零拷贝直投——两者差一个
约 2.7GB/s 的冷缓冲 DRAM 开销（见 round1 报告 §3），file 模式数字不能
直接代表生产。因此编码探针加 `--src file|dmaheap`（默认 file，旧行为不变）：

- `dmaheap`：从生产同款 dma32 堆（`system-uncached-dma32` 优先，回退
  `system-dma32`，与 `dma32_allocator.c` 同序）分配轮转 NV12 缓冲，mmap
  一次性填入文件帧并 `DMA_BUF_IOCTL_SYNC END`，feeder 逐提交
  `mpp_buffer_import(EXT_DMA)`（与生产 `enc_send` 同契约，失败即整轮 FAIL，
  禁止回退——回退会静默测量错误路径）；import 包装保留到输出到达才释放
  （比生产 put 后即放更严格，杜绝复用时序嫌疑），槽位忙闲门控复用 §2 的
  release-after-recv 规则。sync 模式同步支持 dmaheap（串行 import 基线，
  隔离 import 本身开销）。dmaheap 限定 NV12（生产编码输入格式）。
- V4L2 活源（`/dev/video0` BGR3 真实采集节拍 + 真实 fd 交接）不另建探针：
  该路径的覆盖已由 `tx_capture_encode_perf.py`（HDMI 真源跑 `live_ingest`）
  承担，含 RGA 转换与呈现全链路；探针只负责可复现的上限数字。采集抖动
  会污染 wall fps，活源只适合测“有无丢帧/反压”，不适合测上限。
- 判定沿用 §4（编码 verdict 只看 sync；async 仅诊断，见 §1 补充），
  `src` 写入输出行尾（`src=file|dmaheap`，不影响既有正则）。
  预期关系：`sync-file ≈ sync-dmaheap`（import 开销可忽略），
  `async-file ≈ async-dmaheap`（两者同受 CPU/复用开销——dmaheap 无 CPU
  拷贝，若仍慢则证明瓶颈在复用时序而非拷贝），生产 `enc_cost` 应贴近
  sync 口径（零拷贝 + 热缓冲）。若 dmaheap-async 反而失速或挂起，按
  import 生命周期（先查 `dmesg` rkvenc/mpp 报错，再查槽位忙闲打印）排查。

## 7 交付文件映射

- `debug/mpp_enc_frame_latency.c`、`debug/mpp_dec_frame_latency.c`：
  各加 async 分支（本方案 §2）。
- `tests/perf/mpp_async_ceiling.py`：对比编排（本方案 §3~§5）。
- `tests/perf/common/thresholds.json`：加 `async_ceiling` 段（本方案 §4）。
- `tests/perf/README.md`：加新脚本用法。
- 结论报告：`docs/archive/perf/<日期>-mpp-async-ceiling-round<N>.md`，
  含 sync/async 对比表、负载分类、以及"是否补异步编解码"的最终判定。
