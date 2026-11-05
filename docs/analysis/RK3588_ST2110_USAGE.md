# RK3588 应用对 ST2110 协议的使用分析

分析对象：本仓库中面向 Rockchip RK3588（ARM，软件 pacing，无 ICE/E810 硬件限速）的 MTL 应用。
执行日期：2026-11-05。

## 0. 一句话结论

RK3588 三个承载实时媒体流的 app（`rk3588_live_tx`、`rk3588_playout_rx`、`dvs_media`）
**只用 ST2110 的两个 profile**：

- **视频 = ST2110-22（压缩 / mezzanine 码流，frame-level + codestream 打包）**
- **音频 = ST2110-30（PCM16 / 48 kHz / 1 ms ptime）**

并且它们**全部走 MTL 的 Session API**（`st22_tx/rx_create`、`st30_tx/rx_create`），
**没有使用 Pipeline API**（`st20p_/st22p_/st30p_`），也**没有使用未压缩的 ST2110-20
（`st20_tx/rx_create`）**。传输后端统一用 **`native_af_xdp`**（软件 pacing），时间同步以
**外部 PTP（linuxptp → CLOCK_TAI）** 为主。

---

## 1. 哪些 app 用了 ST2110

| App | 角色 | 使用的 profile | 当前数据面状态 |
|---|---|---|---|
| `app/rk3588_live_tx` | 低延迟实时采集→编码→收发→解码→显示 | ST2110-22（TX + RX） | 完整 TX/RX |
| `app/rk3588_playout_rx` | MP4 文件分发 | ST2110-22（TX + RX 视频）+ ST2110-30（TX + RX 音频） | 完整 TX/RX |
| `app/dvs_media` | 演进自 file_playout 的节点媒体引擎 | ST2110-22（RX，多 lane PiP）+ ST2110-30（RX 音频） | RX 完整；TX 后端暂为桩（`backend_pending`） |
| `app/dvs_agent` / `app/dvs_platform` | 控制面（HTTP/SQLite 配置与编排） | 不涉及 ST2110 | 无 MTL |

> 注：`app/v4l2_to_ip/v4l2_to_ip.c` 与 `app/sample/*` 也含 ST2110 代码，但前者用
> ST2110-20 未压缩（`ST20_FMT_YUV_422_8BIT` + `st20_tx_create`），后者是上游示例，均不属
> 本次 RK3588 应用范畴，仅作对照、不在本分析主线。

---

## 2. 使用的 MTL API 层

三个 app 一致地选择 **Session 层**（绕过 Pipeline 层的帧转换/生命周期管理），由应用自己
管理码流缓冲区、编码器/解码器对接、背压与调度。

| 操作 | 调用 | 出现位置 |
|---|---|---|
| 创建视频 TX 会话 | `st22_tx_create(st, &ops)` | `live_ingest/mtl_tx.c:119`、`file_playout/tx/mtl_tx_video.c:135`、`file_playout/tests/rx_minimal.c:56` |
| 创建视频 RX 会话 | `st22_rx_create(st, &ops)` | `live_ingest/mtl_rx.c:160`、`file_playout/rx/mtl_rx_mp4.c:142` |
| 创建音频 TX 会话 | `st30_tx_create(st, &ops)` | `file_playout/tx/mtl_tx_audio.c:134` |
| 创建音频 RX 会话 | `st30_rx_create(st, &ops)` | `file_playout/rx/mtl_rx_audio.c:161` |
| 取/还 TX 帧缓冲 | `st22_tx_get_fb_addr` / `st30_tx_get_framebuffer` | 各 `mtl_tx_*` |
| 还 RX 帧缓冲 | `st22_rx_put_framebuff` / `st30_rx_put_framebuff` | 各 `mtl_rx_*`、bridge、解码线程 |

会话句柄类型：`st22_tx_handle` / `st22_rx_handle` / `st30_tx_handle` / `st30_rx_handle`。

---

## 3. 视频：ST2110-22（压缩码流）

### 3.1 会话参数（TX 与 RX 对称）

代码位置：`live_ingest/mtl_tx.c:87-117`、`live_ingest/mtl_rx.c:129-158`、
`file_playout/tx/mtl_tx_video.c:100-133`、`file_playout/rx/mtl_rx_mp4.c:109-141`。

| `ops` 字段 | 取值 | 含义 |
|---|---|---|
| `type` | `ST22_TYPE_FRAME_LEVEL` | frame-level（整帧一次提交），非 slice-level |
| `fmt` | `ST20_FMT_YUV_420_8BIT` | 码流对应的像素格式标注（codestream 模式下仅作元信息；boxes 已禁用） |
| `pack_type` | `ST22_PACK_CODESTREAM` | 直接搬运编码器 Annex-B 码流，不做像素封装 |
| `flags` | `ST22_TX_FLAG_DISABLE_BOXES` / `ST22_RX_FLAG_DISABLE_BOXES` | 不生成/解析 ST2110-22 的 box 容器，纯 codestream |
| `payload_type` | `112` | RTP payload type（视频） |
| `fps` | `st_frame_rate_to_st_fps(...)` | 帧率（CFR） |
| `width`/`height` | 如 1920/1080 | 分辨率（用于会话协商与状态） |
| `framebuff_cnt` | 8–`N` | 应用自管的帧缓冲数量 |
| `framebuff_max_size` | 512 KB ~ 1 MB | 单帧码流上限（live_ingest 512 KB；file_playout 1 MB） |
| `get_next_frame` / `notify_frame_done` | 回调 | TX 拉帧 / 完成通知 |
| `notify_frame_ready` | 回调 | RX 帧就绪（lcore 上下文） |
| `dip_addr` / `udp_port` / `port` | 组播目的 IP、UDP 端口、网卡名 | 会话寻址 |
| `ip_addr`（RX） | 源 IP | 仅 RX 校验来源 |

关键代码片段（TX，`file_playout/tx/mtl_tx_video.c:100-117`）：

```100:117:app/rk3588_playout_rx/tx/mtl_tx_video.c
    struct st22_tx_ops ops = {0};
    ops.name               = "file_playout_video_tx";
    ops.priv               = tx;
    ops.type               = ST22_TYPE_FRAME_LEVEL;
    ops.fps                = st_frame_rate_to_st_fps((double)p->fps);
    ops.width              = (uint32_t)(p->width > 0 ? p->width : 1920);
    ops.height             = (uint32_t)(p->height > 0 ? p->height : 1080);
    ops.fmt                = ST20_FMT_YUV_420_8BIT;
    ops.pack_type          = ST22_PACK_CODESTREAM;
    ops.num_port           = 1;
    ops.flags             |= ST22_TX_FLAG_DISABLE_BOXES;
```

### 3.2 帧缓冲状态机

TX/RX 都实现同一套 `FB_FREE → FB_FILLING → FB_READY → FB_SENDING` 状态机（`enum fb_state`，
见 `live_ingest/mtl_tx.c:16-21`），用 `atomic_compare_exchange_strong` 推进，配合
`pthread_mutex`+`pthread_cond` 在“无空闲缓冲”时阻塞等待。这套机制直接替代 Pipeline 层的
`st20p_tx_get_frame/put_frame`，把 ST2110-22 帧的“填充—提交—回收”生命周期完全交给应用线程。

- TX：`acquire_fb`（FREE→FILLING）→ 应用写入码流 → `publish_fb`（FILLING→READY）；
  MTL 在 `get_next_frame` 回调里取 READY 帧（READY→SENDING），发送完 `notify_frame_done`
  回到 FREE。
- RX：lcore 回调 `notify_frame_ready` 拿到 `frame` 指针与 `meta->frame_total_size`，推入
  应用 ring；消费线程处理完调用 `st22_rx_put_framebuff` 归还。

### 3.3 码流内容

- `file_playout`：TX 把 FFmpeg demux 出的 **H.264 Annex-B**（已拼接 extradata）直接作为
  ST2110-22 codestream 发送；RX 端剥离出 NAL 喂给 **MPP 硬解**。
- `live_ingest`：TX 把 MPP 编码出的 **H.264** 码流前插一段 **SEI 时间戳 NAL**（含全局
  `frame_seq` + 采集端 PTP 时间戳，`sei_timestamp_build`），RX 端用 `sei_timestamp_peek`
  回读，用于跨机端到端时延测量。

---

## 4. 音频：ST2110-30（PCM）

代码位置：`file_playout/tx/mtl_tx_audio.c:101-133`、`file_playout/rx/mtl_rx_audio.c:123-141`。

| `ops` 字段 | 取值 | 含义 |
|---|---|---|
| `type` | `ST30_TYPE_FRAME_LEVEL` | frame-level |
| `fmt` | `ST30_FMT_PCM16` | 16-bit PCM |
| `sampling` | `ST30_SAMPLING_48K` | 48 kHz |
| `ptime` | `ST30_PTIME_1MS` | 1 ms 打包周期（线速 pacing） |
| `channel` | `2` | 双声道 |
| `payload_type` | `98` | RTP payload type（音频） |
| `framebuff_size` | `st30_calculate_framebuff_size(...)` | 由 fmt/ptime/sampling/channel 计算 |
| `framebuff_cnt` | TX 默认 32；RX 默认 16 | 缓冲数 |

音频帧缓冲用 `st30_tx_get_framebuffer` / `st30_rx_put_framebuff`。RX 端用轻量 **SPSC 环形
队列（32 slot）** 解耦 lcore 回调与 ALSA 播放线程（`file_playout/rx/mtl_rx_audio.c:20`），
播放线程写 ALSA 并调用 `av_sync_note_audio_frame` 做 A/V 同步。

音频 TAI 取值优先级（`file_playout/rx/mtl_rx_audio.c:43-51`）：
`meta->timestamp_first_pkt` → `st10_get_tai(meta->tfmt, meta->timestamp, sample_rate)`。

---

## 5. 传输后端、PMD 与 pacing

### 5.1 PMD 选择

RK3588 没有 DPDK poll-mode NIC（无 ICE/E810），因此统一走 **AF_XDP**。
`file_playout` 的 `file_playout_pmd_from_port`（`file_playout_config.c:707-720`）支持
`kernel:` / `native_af_xdp:` / `dpdk_af_xdp:` / `dpdk_af_packet:` / 裸接口名（DPDK user）。
默认且推荐 **`native_af_xdp:eth0`**，需 `MtlManager` + hugepages 就绪。

`live_ingest` 用 `mtl_pmd_from_port(port_name)`（`live_ingest_app.c:115`）按接口名推断。

### 5.2 pacing：纯软件

RK3588 无法做 NIC 硬件限速，pacing 由 MTL 软件任务负责。相关 `MTL_FLAG`
（`live_ingest_app.c:77-82`、`file_playout_config.c:803-835`）：

| Flag | 作用 |
|---|---|
| `MTL_FLAG_BIND_NUMA` | 绑定 NUMA（单 socket 下仍保留） |
| `MTL_FLAG_TX_VIDEO_MIGRATE` / `MTL_FLAG_RX_VIDEO_MIGRATE` | 视频会话可在调度器间迁移 |
| `MTL_FLAG_RX_SEPARATE_VIDEO_LCORE` | 视频 RX 独占 lcore |
| `MTL_FLAG_SHARED_RX_QUEUE` / `MTL_FLAG_SHARED_TX_QUEUE` | 单队列时共享队列 |

> 重要坑（`file_playout_config.c:826-833` 注释）：单队列 AF_XDP 下，**ST22 视频 + ST30 音频
> 同端口必须开启 `SHARED_RX_QUEUE`**，否则音频任务会覆写 RX 入口并在
> `rx_audio_session_tasklet → mt_rxq_burst` 处 SIGSEGV。

### 5.3 PTP / 时间源

`file_playout` 定义三种模式（`file_playout_app.h:13-17`），默认 **external**：

| 模式 | 标志 / 实现 | 适用 |
|---|---|---|
| `FILE_PLAYOUT_PTP_EXTERNAL` | `ptp_get_time_fn = mp4_ptp_mtl_time_fn` → `CLOCK_TAI`（linuxptp + `set_tai_offset`） | 双机/多机共享 PTP 域（默认） |
| `FILE_PLAYOUT_PTP_BUILTIN` | `MTL_FLAG_PTP_ENABLE` | 需 NIC HW timesync（RK3588 不具备） |
| `FILE_PLAYOUT_PTP_MONOTONIC` | 相对 monotonic 锚定 | 无 PTP 调试 |

`live_ingest` 通过 `mtl_ptp_read_time` 作为 `time_now` 回调把采集时间戳统一到 PTP 域。

---

## 6. 时间同步与时间戳细节

ST2110-22/30 的线速 pacing 与跨机 E2E 时延都依赖 TAI 时间轴，三个 app 的处理方式：

- **TX 绝对/相对 TAI 锚定**：`file_playout` 在 `mtl_tx_video.c:111-112` 与 `mtl_tx_audio.c:110-111`
  开启 `ST22_TX_FLAG_USER_TIMESTAMP | ST22_TX_FLAG_USER_PACING`，由 `shared/tx_tai_ts.c`
  计算 `epoch_anchor` / `stream_start_tai`：绝对线速 TAI（≥1e15 ns）直接用；ST30 的
  “stale meta”相对预填则对齐 PTP 网格（`tx_tai_compute_anchor`）。
- **视频 RX 首帧锚定**：`file_playout/rx/mtl_rx_mp4.c:79-80` 用 `meta->timestamp` +
  `media_clk_frame_pts(&clk, tfmt, timestamp)` 把首帧锚定到 `mtl_ptp_read_time()`。
- **视频 SEI 时间戳（live_ingest）**：采集端把 PTP 时间戳写进 SEI NAL；RX `rx_pipeline.c:286`
  用 `sei_timestamp_peek` 取出 `cap_ts` 计算端到端时延，并做零值/回退/越界分类
  （`rx_lat_classify`）。
- **音频 RX TAI**：见 §4 `timestamp_first_pkt` / `st10_get_tai`。
- **A/V 同步**：`file_playout/rx/av_sync.c` + `rx/media_clk.c` 让视频 PTS 与音频帧处在同一
  PTP 时间域后做 playout 调度。

---

## 7. RX 数据流与背压

### 7.1 live_ingest（环路单流）

```
ST22 RX(lcore) → notify_frame_ready
   → rx_pkt 描述符池(RX_POOL_SIZE=16)  → output_ring
   → mtl_rx_bridge 线程：slab 拷贝 codestream
   → decode 线程：slice_reassembler → MPP 解码 → DRM 显示
```

- lcore 回调非阻塞：先抢 `pool[RX_POOL_SIZE]` 描述符，满则按 `ST_FRAME_STATUS_*` 丢弃并计入
  `stat_pool_full`（`mtl_rx.c:78-95`）。
- `mtl_rx_bridge` 线程把 `frame` 拷到 `slab_pool` 后立即 `st22_rx_put_framebuff` 归还 MTL，
  避免占用 RX 帧缓冲（`mtl_rx_bridge.c:74-126`）。
- 背压可见性：池满丢弃（pool-full）、ring 满丢弃（ring_drops）、停机丢弃（stopping_drops），
  均有独立计数与 5s 冷却告警。

### 7.2 file_playout / dvs_media（多 lane PiP + 音频）

- 每个视频源独立一个 **ST22 RX 会话**（`mtl_rx_mp4`，`RX_ST22_FB_CNT=8`、`RX_POOL_SIZE=16`，
  见 `rx_compositor.c:28` 与 `mtl_rx_mp4.c:13`），经 `rx_compositor` 做多窗口合成后 MPP 解码 +
  DRM 显示。
- 音频独立一路 **ST30 RX**（`mtl_rx_audio`），SPSC(32) → ALSA。
- RX 先校验 `meta->status`：`ST_FRAME_STATUS_CORRUPTED` 直接丢弃；非
  `COMPLETE`/`RECONSTRUCTED` 计入 `incomplete`（`mtl_rx_mp4.c:58-68`）。
- `dvs_media` 的 TX 路径（`engine/tx_session.c`）当前为**桩**：仅做参数校验与状态机
  （`DVS_TX_CONFIGURED` / `backend_pending`），真正的 ST22 TX 后端待单板 bring-up 接入。

---

## 8. 各 app 对照速查

| 维度 | live_ingest | file_playout | dvs_media |
|---|---|---|---|
| 视频 profile | ST2110-22 TX+RX | ST2110-22 TX+RX | ST2110-22 RX（多 lane） |
| 音频 profile | 无 | ST2110-30 TX+RX | ST2110-30 RX |
| 码流封装 | codestream + 禁用 box + SEI 时间戳 | codestream + 禁用 box + Annex-B | 同 file_playout |
| API 层 | Session | Session | Session（经 rx_compositor / tx_session） |
| PMD | af_xdp / 推断 | native_af_xdp 默认，多后端可配 | 同 file_playout |
| PTP | 外部 PTP（`mtl_ptp_read_time`） | external / builtin / monotonic | external / monotonic |
| 用户时间戳 | SEI 内嵌（采集端 PTP） | USER_TIMESTAMP+USER_PACING（TX） | 同 file_playout |
| pacing | 软件 | 软件（含 SHARED_QUEUE 约束） | 软件 |
| 背压 | pool(16)+ring+bridge | per-lane pool(16)+ring+compositor | 同 file_playout |

---

## 9. 关键源码索引

| 功能 | 文件 |
|---|---|
| 视频 TX 会话（ST22） | `app/rk3588_live_tx/mtl_tx.c`、`app/rk3588_playout_rx/tx/mtl_tx_video.c` |
| 视频 RX 会话（ST22） | `app/rk3588_live_tx/mtl_rx.c`、`app/rk3588_playout_rx/rx/mtl_rx_mp4.c` |
| 音频 TX 会话（ST30） | `app/rk3588_playout_rx/tx/mtl_tx_audio.c` |
| 音频 RX 会话（ST30） | `app/rk3588_playout_rx/rx/mtl_rx_audio.c` |
| RX→解码桥接 / 背压 | `app/rk3588_live_tx/mtl_rx_bridge.c`、`app/rk3588_playout_rx/rx/rx_compositor.c` |
| MTL 初始化与 PTP/PMD/Flag | `app/rk3588_live_tx/live_ingest_app.c:77-128`、`app/rk3588_playout_rx/file_playout_config.c:705-868` |
| TAI 锚定 / 时间戳换算 | `app/rk3588_playout_rx/shared/tx_tai_ts.c`、`shared/ptp_time.c`、`rx/media_clk.c` |
| SEI 时间戳（live） | `app/rk3588_live_tx/hal/sei_timestamp.*`、`pipeline/rx_pipeline.c:283-294` |
| dvs TX 桩 | `app/dvs_media/engine/tx_session.c` |
