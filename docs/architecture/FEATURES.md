# Media Transport Library 功能清单

基于 `README.md`、`doc/design.md`、`CHANGELOG.md`、`include/*.h` 与 `lib/src` 实现整理。版本 `26.09.0.DEV`。

## ST 2110 协议覆盖

| 标准 | 能力 | 主要实现 |
|------|------|----------|
| ST 2110-10 | 系统定时、RTP、组播 | PTP、epoch、媒体时钟 |
| ST 2110-20 | 未压缩视频 | `st20_*` / `st20p_*`，RFC 4175 |
| ST 2110-21 | 流量整形 | RL / TSC / TSN / PTP / BE / TSC_NARROW |
| ST 2110-22 | 压缩视频（JPEG-XS 等） | `st22_*` / `st22p_*` + 编解码插件 |
| ST 2110-30 | PCM 音频 | `st30_*` / `st30p_*` |
| ST 2110-40 | 辅助数据 RFC 8331 | `st40_*` / `st40p_*` |
| ST 2110-41 | Fast Metadata | ST40 会话 opt-in：`ST40_TX_FLAG_FAST_METADATA` / `ST40_RX_FLAG_FAST_METADATA` |
| ST 2022-7 | 双路径冗余 | `num_port = 2`，软件去重 |
| ST 2022-6 | SDI over IP | RTP passthrough，应用自行封装 |

TX 视频固定 gapped 调度。`ST21_PACING_LINEAR` 未实现。

## 视频格式与帧率

分辨率：720p、1080i、1080p、4K、8K 及其他 ST 2110-20 列出的格式。

帧率（`enum st_fps`）：120、119.88、100、60、59.94、50、30、29.97、25、24、23.98。隔行时 FPS 表示场/秒。

像素格式：ST 2110-20 全部列出的格式，含 YUV 4:2:2 10-bit。Pipeline 内置 SIMD 大小端转换（AVX2 / AVX512 / AVX512-VBMI2）。

## 数据路径后端

1. **DPDK PMD**（生产路径）  
   Kernel bypass、poll mode。日常验证在 Intel E810 / E830。Narrow pacing 仅在这两款配合 RL 时保证。

2. **Native AF_XDP + eBPF**  
   `MTL_PMD_NATIVE_AF_XDP`。MtlManager 加载 `mtl_dp_filter` 做 UDP 端口过滤。需要 CAP_NET_RAW。

3. **Kernel socket**（实验性）  
   `MTL_PMD_KERNEL_SOCKET`。无 DPDK 网卡时的回退。

4. **DPDK AF_XDP / AF_PACKET**（实验性）

无 root 运行、SR-IOV 虚拟化、每进程最多 8 个 NIC、多进程（靠 Manager 分配 lcore）均已实现。

## Pacing

`enum st21_tx_pacing_way`：

| 方式 | 说明 |
|------|------|
| `AUTO` | 按网卡能力自动选择 |
| `RL` | NIC 限速，部分硬件卸载。E810/E830 + 补丁 ICE 驱动 |
| `TSC` | 纯软件，宽 pacing |
| `TSC_NARROW` | TSC + 单 bulk transmitter |
| `TSN` | E830 PF + PTP，包带 launch time，NIC PHC 到点才发 |
| `PTP` | 按 PTP 时钟 pacing |
| `BE` | best effort |

RL 路径会在帧起点前发 padding，让 shaper 在 scheduled time 放出第一包。默认 TX 队列深度：具备 reclaim 的 RL/TSN 口为 2048 descriptor，其余 512。

## 应用 API 模式

### Pipeline（推荐）

`st20p_tx_get_frame` / `st20p_tx_put_frame` 及 RX、ST22/30/40 对应接口。库管理帧缓冲、色彩转换、编解码插件。可开 `ST20P_*_FLAG_BLOCK_GET` 变成阻塞（1 秒超时），`st20p_tx_wake_block` 可提前唤醒。

### Session / Frame

`st20_tx_create` + `get_next_frame` / `notify_frame_done`。Slice 模式用 `query_frame_lines_ready` / `notify_slice_ready` 做行级超低延迟。

### RTP passthrough

应用自己组/解 RTP，库只做 L2。用于 ST 2022-6。

生命周期：`mtl_init` 后可在 `mtl_start` 前或后创建会话；释放须在 `mtl_stop` 前完成。

## 可靠性与定时

**ST 2022-7**：TX 复制到第二块网卡；RX 同一 tasklet 收双路径，RTP 头定位后丢弃重复。应用可看 `pkts_recv[MTL_SESSION_PORT_MAX]`。

**RTCP 重传**（非标准、仅 MTL 互操作）：`ST**_TX_FLAG_ENABLE_RTCP` / `ST**_RX_FLAG_ENABLE_RTCP`。RX 发现缺包发 NACK，TX 从 ring 重发。

**不完整帧**：默认丢弃。`ST**_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` 可交给应用。

**VSync**：`ST**_TX_FLAG_ENABLE_VSYNC`，按 PTP epoch 回调 `ST_EVENT_VSYNC`。

**用户时间戳 / pacing**：

- `ST20_TX_FLAG_USER_TIMESTAMP`：应用指定 RTP timestamp
- `ST20_TX_FLAG_USER_PACING`：按 TAI 等到对应 epoch 再发
- `ST20_TX_FLAG_EXACT_USER_PACING`：第一包精确落在给定时刻
- `ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH`：RTP timestamp 对齐 N × T_FRAME（ST 2110-20 §7.6.3）

**运行时改地址**：`st20_tx_update_destination`、`st20_rx_update_source` 及 22/30/40/pipeline 对应函数，转发场景不必重建会话。

**隔行**：每场当一帧。应用用 `second_field` 标记。高度填整帧 1080，缓冲只有 540 行。

## 加速与卸载

- **零拷贝 TX**：多段 mbuf，NIC DMA 直接读应用帧。
- **RX DMA**：payload ≥ 1024 字节走 DMA（`mt_dma.c`，最多 16 session）。
- **SIMD CSC**：`st_avx2.c`、`st_avx512.c`、`st_avx512_vbmi.c`，API 在 `st_convert_api.h`。
- **GPU Direct**：`gpu_direct/` 封装 Level Zero，分配 device/shared buffer，ST 2110-20 可用 GPU 缓冲。
- **外部帧**：应用自管缓冲；DPDK 路径需 `mtl_dma_map` 提供 IOVA。`ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE` 提供两阶段释放。
- **硬件 RX timestamp**：`MTL_FLAG_ENABLE_HW_TIMESTAMP`，供 timing parser。
- **RX timing parser**：`ST20_RX_FLAG_TIMING_PARSER_STAT` / `_META`，判定 narrow/wide/fail。

## 控制面网络功能

- 内置 PTP（`MTL_FLAG_PTP_ENABLE`），或 `ptp_get_time_fn` 接 ptp4l
- `MTL_FLAG_PHC2SYS_ENABLE` 把 PHC 同步到系统时钟
- 静态 IP 或 DHCP
- IGMPv3；`MTL_FLAG_NO_MULTICAST` 关闭加入（SDN 直送）
- `MTL_FLAG_VIRTIO_USER` 作 exception path
- TAP（Windows，`enable_tap`）

## 插件系统

`mtl_init` 时读 `kahawai.json`（可用 `KAHAWAI_CFG_PATH` 覆盖），加载动态库。插件导出：

- `st_plugin_get_meta`
- `st_plugin_create`
- `st_plugin_free`

可注册 JPEG-XS 编解码、色彩转换加速。树内实现：`plugins/st22_avcodec`。

## 生态集成

| 组件 | 位置 | 覆盖 |
|------|------|------|
| FFmpeg 4.4 / 6.1 / 7.0 | `ecosystem/ffmpeg_plugin/` | st20p/st22p/st30p TX+RX，冗余、PTP AVOption |
| GStreamer | `ecosystem/gstreamer_plugin/` | st20p/st30p/st40p TX+RX，DMA、零拷贝、PTP |
| OBS | `ecosystem/obs_mtl/` | 直播推流 |
| MTL with MXL | `ecosystem/MTL_with_MXL/` | 与 Media eXchange Layer 桥接 |
| Python | `python/swig` | SWIG，含 timing parser 示例 |
| Rust | `rust/` crate `imtl` | video-tx / video-rx 示例 |

## 示例与工具应用

- Pipeline 样例：`TxSt20PipelineSample` 等（`app/sample/`）
- Legacy / slice / RTP：`app/sample/legacy/`、`low_level/`
- 转发：`app/sample/fwd/`
- GPU Direct：`app/sample/gpu_direct/`
- V4L2 采集上 IP：`app/v4l2_to_ip/`
- 色彩转换工具：`app/tools/convert_app.c`
- 端到端压测：`tests/tools/RxTxApp`（JSON 配置在 `config/`）

## 可观测性

- DPDK `RTE_LOG`，级别 `enum mtl_log_level`，可 `mtl_set_log_printer` / `mtl_openlog_stream`
- 端口统计：`mtl_get_port_stats`，含乱序、重复、帧级计数
- USDT（`mt_usdt_provider.d`）：生产环境用 eBPF 拉探针，默认 meson 选项 `enable_usdt`
- pcap dump：`mt_pcap.c`
- `MTL_FLAG_TASKLET_TIME_MEASURE` 测量 tasklet 耗时

## 测试分层

| 层级 | 位置 | 条件 |
|------|------|------|
| Unit | `tests/unit/` | 无 NIC、无 root，`./build.sh unit` |
| Integration | `tests/integration_tests/` | 真实 VF，`KahawaiTest` |
| Acceptance | `tests/acceptance/` | pytest，RxTxApp / FFmpeg / GStreamer |
| Fuzz | `tests/fuzz/` | libFuzzer，打 RX 解析 |

## 平台支持（CI）

容器构建 + 单测：Ubuntu 22.04 / 24.04 / 26.04、Rocky Linux 9。

带 E810/E830/E835 的裸机集成与验收：Ubuntu 24.04。Windows 有独立 build/run/VM 文档。AWS 有 `doc/aws.md`。

## 明确不在当前树内的能力

- 用户态 UDP 栈与 `ld_preload` UDP shim 已删除（`MTL_TRANSPORT_UDP` 仅保留枚举）
- RDMA、Intel Media SDK 已移除
- TX `ST21_PACING_LINEAR` 未实现
- 主分支代码用于评估，生产请用 tagged release
