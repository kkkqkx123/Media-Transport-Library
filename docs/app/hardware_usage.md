# RK3588 应用硬件使用分析

本文整理 `app/` 下两个 RK3588 应用对各个硬件模块的实际使用情况：接口、关键文件、调用时机、零拷贝路径与软件回退。所有行号基于 `feat/rk3588-apps` 分支当前代码。

| 应用 | 定位 | 数据链路 |
|---|---|---|
| `app/rk3588_end2end/` | 低延迟实时采集-编码-传输-解码-渲染 | V4L2 采集 → RGA 转换 → MPP 编码 → MTL TX/RX → MPP 解码 → DRM 显示 |
| `app/rk3588_mp4_dist/` | MP4 文件分发（多窗口/PvD） | FFmpeg demux → MTL TX/RX（视频+音频）→ MPP 解码 → DRM 多 plane 显示 + ALSA 音频 |

硬件覆盖总览：

| 硬件 | rk3588_end2end | rk3588_mp4_dist |
|---|---|---|
| V4L2 采集（rk_hdmirx） | ✅ /dev/video0 | — |
| MPP VENC（H.264 编码） | ✅ | —（TX 直发码流，无编码） |
| MPP VDEC（H.264 解码） | ✅ | ✅ |
| RGA 2D 加速 | ✅ BGR3→NV12 CSC | ❌ 未使用（缩放/合成由 DRM plane 完成） |
| DRM/KMS 显示 | ✅ 单 CRTC page-flip | ✅ 单 CRTC 多 overlay plane 合成 |
| ALSA 音频 | ❌ 无音频链路 | ✅ 48k/2ch/S16_LE 播放 |
| DMA-BUF | V4L2 EXPBUF / RGA / MPP 内部 | MPP 内部（无直接 DMA heap 操作） |
| MTL 网络 | native_af_xdp，ST2110-22 | native_af_xdp 或 kernel，ST2110-22 + ST2110-30 |
| PTP 时钟 | `mtl_ptp_read_time` 回调 | linuxptp (ptp4l) + `CLOCK_TAI` |

---

## 1. rk3588_end2end

### 1.1 V4L2 视频采集（rk_hdmirx）

- 文件：`io/v4l2_capturer.c`；设备默认 `/dev/video0`，可由 JSON 配置覆盖（`e2e_config.c:51`、`:123`）。
- 驱动为 rk_hdmirx（HDMI RX 输入），使用多平面 API `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`（`v4l2_capturer.c:91`）。
- 格式协商：优先 `V4L2_PIX_FMT_NV12`（2-plane），失败回退 rk_hdmirx 默认的 `V4L2_PIX_FMT_BGR24`（`:94-104`）；实际协商结果记录在 `actual_fourcc / actual_num_planes / plane_stride`（`:106-108`）。
- 分辨率/帧率：默认 1920x1080@60（`e2e_config.c:38-40`），`VIDIOC_S_PARM` 设置 timeperframe（`:116-121`）。
- 缓冲方式：**MMAP**（REQBUFS + QUERYBUF + mmap，`:124-160`），buf_count = 6（`e2e_app.c:299`）；每个 plane 通过 `VIDIOC_EXPBUF` 导出 dma-buf fd（`:162-171`），下游以 fd 引用帧。
- 调用时机：`tx_pipeline_create`（`tx_pipeline.c:233`）创建，专用 capture 线程（`tx_pipeline.c:226-273`）循环 DQBUF → `capture_on_frame` 回调（打 PTP/单调时间戳后推 `ring_cap2enc`，`tx_pipeline.c:60-92`）→ 立即 QBUF 归还。帧的存活完全依赖 dma-buf fd 引用。

### 1.2 MPP 硬件编解码

**编码器 `codec/mpp_encoder.c`**
- H.264 / AVC（`MPP_VIDEO_CodingAVC`，`:40`），Baseline@L4.2，CBR 40 Mbps，GOP 60，Annex-B 输出（`:72-74`）。
- **零拷贝输入**：`MPP_BUFFER_TYPE_EXT_DMA` + `mpp_buffer_import` 直接导入 RGA 输出的 NV12 dma-buf fd（`:104-110`）；导入失败回退 mmap + memcpy（`:112-120`）。
- 输出码流 packet memcpy 到 slab/MTL fb（`enc_append_packet`，`:182`）。

**解码器 `codec/mpp_decoder.c`**
- H.264（`:64`），`split_parse = 1`（`:73`），关闭 deinterlace（`:61`）。
- info-change 时创建 `MPP_BUFFER_TYPE_ION` buffer group（内核无 ION 时 MPP 内部回退 DMA_HEAP，`:144-154`），24 个输出 buffer。
- 解码输出帧取 `mpp_buffer_get_fd` 的 dma-buf fd 直接送 DRM（`:254-270`）；`FB_DELAY` 环延迟释放，防止 DRM 翻页中 buffer 被复用产生鬼影（`:260-279`）。

**SEI 时间戳 `hal/sei_timestamp.c`**
- 自定义 user_data_unregistered SEI（UUID "MTLT"，25 字节），携带 frame_seq + cap_ts_ns（PTP 或 CLOCK_MONOTONIC）。
- 编码端注入点：`tx_pipeline.c:140/160`；接收端 `sei_timestamp_extract`（`rx_pipeline.c:129`）剥离并做端到端延迟统计。

### 1.3 RGA 硬件 2D 加速

- 文件：`codec/rga_converter.c`（librga C API：`c_RkRgaInit / c_RkRgaGetAllocBufferCache / c_RkRgaGetBufferFd / c_RkRgaBlit`）。
- 用途：BGR3 → NV12 颜色空间转换（BT.709，`color_space_mode=2`，`:81-88`），同尺寸无缩放。
- 输出 buffer：预分配 pool（默认 12 个，`e2e_app.c:301`），每个导出 dma-buf fd（`:37-47`），轮转使用（`:70-71`）。
- 同步方式：`c_RkRgaBlit`（`:92`）为同步阻塞调用，无 fence/异步路径。
- 调用时机：TX encode 线程每帧 `rga_convert`（`tx_pipeline.c:105`）；src 为 V4L2 导出的 dma-buf（零拷贝输入），dst 为 RGA 自有 dma-buf，输出 fd 直接给 MPP 编码器。

### 1.4 DRM/KMS 显示

- 文件：`io/drm_renderer.c`；设备 `/dev/dri/card0`，connector `HDMI-A-1`（`e2e_config.c:52-53`），mode 1920x1080@60；按名称部分匹配 connector（`:53-72`），从 connector 的 encoder 得 crtc_id（`:102-104`）。
- 使用 legacy CRTC 接口（非 atomic plane API）；FB 通过 `DRM_IOCTL_MODE_ADDFB2` 创建，`DRM_FORMAT_NV12`，双 plane 共用同一 GEM handle，UV offset = hor_stride * ver_stride（`:159-173`）。
- 翻页方式：首帧 `drmModeSetCrtc`，之后 `drmModePageFlip` + `DRM_MODE_PAGE_FLIP_EVENT` 异步翻页，`poll` + `drmHandleEvent` 等待 flip 完成（`:179-209`、`:39-50`）；翻页失败回退 SetCrtc；FB 槽位延迟 buf_count 帧回收（`:204-208`）。
- 输入 buffer：解码器 dma-buf fd 经 `drmPrimeFDToHandle` 导入（`:157`）→ **零拷贝显示**。
- 调用时机：RX decode 线程 `rx_render_frame`（`rx_pipeline.c:60`）。

### 1.5 内存 / DMA-BUF

- `hal/dma_allocator.c`（DRM dumb buffer + `drmPrimeHandleToFD` 导出 dma-buf）**已编译进 HAL 但运行路径未使用**（无调用点，见 `meson.build:19`）。
- 实际 dma-buf 来源：
  - V4L2 `VIDIOC_EXPBUF`（采集帧）；
  - RGA `c_RkRgaGetAllocBufferCache`（转换输出）；
  - MPP ION/DMA_HEAP buffer group（解码输出）。
- 无直接操作 `/dev/dma_heap/*`（cma/system）节点的代码。
- `hal/slab_pool.c`：普通 malloc 的 slab 池（非 dma-buf），原子 CAS 分配 + cond 等待；TX 512KB × ring×3（`tx_pipeline.c:212`），RX 512KB × 10（`e2e_app.c:141-167`），用于码流包暂存。MTL fb 由 MTL 库分配（`st22_tx_get_fb_addr`，`mtl_tx.c:119`）。

### 1.6 MTL 网络传输

- 初始化：`mtl_setup_single`（`e2e_app.c:116-138`），`MTL_PMD_NATIVE_AF_XDP` + `MTL_PROTO_STATIC`，单端口；默认 TX 口 `native_af_xdp:enP3p49s0`，RX 口 `native_af_xdp:eth0`（`e2e_config.c:45-50`，r8168 单队列）；flags：BIND_NUMA、TX/RX_VIDEO_MIGRATE、RX_SEPARATE_VIDEO_LCORE（`:96-99`）。
- 协议：**ST 2110-22（ST22 JPEG-XS frame-level codestream）**，非 ST2110-20/30/40。
  - TX：`st22_tx_create`（`mtl_tx.c:87-115`），payload_type 112，`ST20_FMT_YUV_420_8BIT` + `ST22_PACK_CODESTREAM`，DISABLE_BOXES，8 fb × 512KB；
  - RX：`st22_rx_create`（`mtl_rx.c:90-118`）同参数；TX/RX 各 1 会话。
- Pacing：由 MTL 库内部 pacing（app 按 fps 注册，`st_frame_rate_to_st_fps`）；app 侧 `hal/timer.c:39-48` 的 `e2e_timer_wait_frame` 用 `clock_nanosleep(CLOCK_MONOTONIC)` 做帧步进（ref/测试路径）。
- 时钟：`mtl_ptp_read_time` 作为 time_now 回调注入 tx/rx pipeline（`e2e_app.c:205,311`），失败回退 CLOCK_MONOTONIC（`tx_pipeline.c:67-69`）。
- RX 桥：`mtl_rx_bridge.c:71-77` 从 MTL fb **memcpy** 到 slab 再推 dec_ring（一次 CPU 拷贝），MTL fb 尽早归还。

### 1.7 定时器 / CPU 布局

- `hal/timer.c`：CLOCK_MONOTONIC + `clock_nanosleep` 帧间隔等待；Phase 5 预留 `mtl_ptp_read_time`。无 timerfd、无 GPIO、无硬件定时器。
- 忙等待使用 ARM `yield` 指令循环（`mpp_encoder.c:136-139`、`mpp_decoder.c:41-45`）。
- CPU 亲和性：capture=1、encode=2、bridge=5、decode=4（`e2e_config.c:55-58`，`pthread_setaffinity_np`）。
- 音频：无 ALSA/ST30 引用，本应用仅视频链路。

### 1.8 端到端数据链路（zero-copy 标注）

```
TX: rk_hdmirx V4L2 (/dev/video0, BGR3, MMAP+EXPBUF)
    --dma-buf fd, 零拷贝--> RGA 硬件 BT.709 CSC
    --NV12 dma-buf, 零拷贝--> MPP H.264 编码 (EXT_DMA fd 导入; 失败回退 mmap+copy)
    --码流 memcpy 进 MTL ST22 fb (含 25B SEI 时间戳)--> native_af_xdp (enP3p49s0, ST2110-22)

RX: native_af_xdp (eth0) ST22 收帧
    --bridge memcpy 到 slab (一次 CPU 拷贝)--> MPP H.264 解码 (ION/DMA_HEAP 输出)
    --dma-buf fd, 零拷贝--> DRM card0/HDMI-A-1 NV12 ADDFB2 + 异步 page-flip
```

软件回退路径：V4L2 NV12 失败→BGR3；编码 fd 导入失败→mmap+copy；DRM page-flip 失败→SetCrtc；PTP 读取失败→CLOCK_MONOTONIC。

---

## 2. rk3588_mp4_dist

### 2.1 MP4 解封装（纯软件，FFmpeg libavformat）

- `tx/mp4_demux.c`：`avformat_open_input` / `avformat_find_stream_info`（L27-34）；`av_find_best_stream` 选出 H.264 视频流（宽高/fps/time_base/extradata，L43-66）与 AAC 音频流（采样率/声道，L70-85）。
- AVCC→Annex-B：`h264_mp4toannexb` bitstream filter（L94-113），extradata SPS/PPS 注入配合 `tx/extradata_handler.c`。
- 读包入口：`mp4_demux_read_packet`（L218-254），由 `tx/tx_scheduler.c` 的 lane 线程（`tx_read_one_video` L180）与音频线程（经 `mp4_playlist_audio_demux`）逐帧调用。
- 播放列表：`tx/mp4_playlist.c` — `mp4_playlist_create`（L65，每个 stream 一个 demux ctx）、`playlist_advance_video/audio`（L132/146）实现循环播放与 seek 重定位。

### 2.2 MPP 硬件解码

- 文件：`shared/mpp_decoder.c`（经 `shared/codec_plugin.h` 的 codec_plugin 抽象注册，供 rx_compositor / rx_pipeline 使用）。
- `mpp_init(MPP_CTX_DEC, MPP_VIDEO_CodingAVC)`（L372）— 仅 H.264；关闭 deinterlace（L369）、`MPP_DEC_SET_DISABLE_DPB_CHECK`（L377）、`split_parse`（L385，可由 env `MP4_DIST_MPP_FORCE_SPLIT` / `MP4_DIST_NO_SPLIT_PARSE` 控制，L336-344）。
- DMABUF 输出：`dec_get_frame`（L197）→ `mpp_frame_get_buffer`（L224）→ `mpp_buffer_get_fd`（L231）直接取 dma-buf fd + hor/ver stride（L250-251）交 DRM import；`held[FB_DELAY]` 环形持有 MppFrame 延迟归还。
- 喂数：`dec_put_packet`（L167）、`mpp_dec_put_whole/put_au/put_chunk/pull`（L454-611）；EOS（L496-518）；软 reset（L520，绕开 MPP 1.3.x `api->reset()` SIGSEGV）。
- 解码 buffer 由 MPP 内部 buffer group 管理（`mpp_buffer_group_put` L430），未显式挂 DMA heap，fd 可直接 drmPrime import。
- 调用者：`rx/rx_compositor.c` L386（每 lane 一个 VDEC，PvD 多路时每路一个）及 `rx/rx_pipeline.c`。

### 2.3 RGA —— 未使用

全目录无 `rga / RGA / im2d / imresize` 引用。`rx_compositor` 名字有误导：它不做像素级合成，只是多 lane 管线编排器（`rx/rx_compositor.c` L330-449：每 lane = mtl_rx_mp4 + bridge + MPP decoder + h264_prep + playout buffer）。缩放/定位完全交给 DRM plane 硬件缩放（`drmModeSetPlane` 的 dst rect）；解码输出 NV12，DRM 直接消费 NV12，无需颜色转换。

### 2.4 DRM/KMS 多窗口显示

- 文件：`shared/drm_renderer.c`（辅助：`shared/drm_mode.c`、`shared/display_layout.c`）；默认 `/dev/dri/card0`、connector `HDMI-A-1`（`mp4_dist_config.c` L40-41）。
- `drm_renderer_create`（L296-408）：open + `DRM_CLIENT_CAP_UNIVERSAL_PLANES`（L315）、find_conn、encoder→crtc；模式选择 `pick_connector_mode`（L235，"auto" 时最接近 1080p60）。
- 合成方式：
  - 全屏：CRTC page-flip（`drmModePageFlip` + `page_flip_handler` L56，`drm_renderer_display_fullscreen` L480）；NV12 fb 由 `import_nv12_fb`（L453）用 `drmModeAddFB2` 导入解码 DMABUF。
  - viewport / PvD 多窗口：`find_nv12_overlay_plane`（L108-159）找支持 NV12 的 overlay plane（优先 plane_id≥79 的 RK AFBC/高端 plane）；`drm_renderer_display_layers`（L507-556）每 layer 一个 `drmModeSetPlane`（L226-233）多 plane 硬件合成；背景用 dumb buffer（`bg_create` L161-201）+ `drmModeSetCrtc`（L218）。单 CRTC 多 plane，不做多 CRTC。
- 调用者：`rx/rx_compositor.c` L428（多窗口 layout 来自 `display_layout_from_pip_streams` L414）、`rx/rx_pipeline.c` `rx_render_frame` L126（单路全屏）。

### 2.5 ALSA 音频播放与 AV 同步

- `rx/alsa_audio.c`：`snd_pcm_open(PLAYBACK)`（L43）；`snd_pcm_set_params(S16_LE, RW_INTERLEAVED, 2ch, 48000 Hz, soft_resample=1, latency=500ms)`（L51-57）；写入 `snd_pcm_writei`（L77-102），-EPIPE 时 `snd_pcm_prepare` 恢复；句柄互斥锁串行化。
- 主时钟：`alsa_update_playback_clock`（L24-34）用 `snd_pcm_delay` 换算已播放秒数，`alsa_audio_clock_get`（L104）供 av_sync 读取。
- AV 同步：`rx/av_sync.c` — `av_sync_decide_for_pts`（L63-112）以音频/PTP 时钟为基准对视频 PTS 做 display/drop/sleep 决策；音频侧 `rx/mtl_rx_audio.c` L86-91 收到 ST30 帧即写 ALSA 并 `av_sync_note_audio_frame(tai_ns)`；视频侧 `rx/playout_buffer.c` `pop_at`（L46）按目标时间弹帧。

### 2.6 内存 / DMA-BUF

- 无直接 DMA heap 节点操作；dma-buf 唯一来源是 MPP 内部分配（见 §2.2）。
- `shared/slab_pool.c`：纯 malloc 池，用于 RX 码流包缓存（`rx_compositor.c` L345 `lane->coded_pool`，bridge 从 MTL fb 拷出 → slab → dec_ring）。
- `shared/ring_buffer.c`：SPSC 有锁 ring（`ring_push/pop_wait` L40/69），lane 的 `mtl_ring` / `dec_ring`（L343-344）。
- MTL 侧 fb：`st22_rx_get_fb_addr` / st30 fb（`mtl_rx_mp4` / `mtl_rx_audio`）。
- ST22 fb → slab → MPP packet 之间有一次 CPU memcpy（非零拷贝）；仅"解码输出 → DRM"是零拷贝。

### 2.7 MTL 网络传输与 PTP

- 视频：`tx/mtl_tx_video.c` L100-132 `st22_tx_create`，`ST22_TYPE_FRAME_LEVEL`，`ST20_FMT_YUV_420_8BIT` + `ST22_PACK_CODESTREAM`，payload 112，UDP 20000，fb 8；`ST22_TX_FLAG_USER_TIMESTAMP | USER_PACING`（L110-112）配合 `shared/tx_tai_ts.c` 的 TAI 时间轴 pacing。**ST 2110-22。**
- 音频：`tx/mtl_tx_audio.c` L101- `st30_tx_create`（PCM16 / 48 kHz / 1ms ptime / 2ch，payload 98，UDP 20002）。**ST 2110-30。**
- RX：`rx/mtl_rx_mp4.c` L117-146 `st22_rx_create`（同参数，fb_cnt 可配）；`rx/mtl_rx_audio.c` L131-157 `st30_rx_create`（fb 16 × 1ms）。
- 端口后端：`native_af_xdp:`（RK 板上，需 MtlManager + hugepage）或 `kernel:`（PC 联调，见 `config/tx_dual_host.json.example` 等）。
- PTP：外部 linuxptp 方案 — `shared/ptp_time.c` L29 `mp4_ptp_read_tai_ns` 读 `CLOCK_TAI`（fallback realtime + adjtimex TAI offset），L52 `mp4_ptp_mtl_time_fn` 挂到 MTL `ptp_get_time_fn`；配置见 `config/ptp/ptp4l-master-unicast.conf`、`ptp4l-slave-unicast.conf` + `scripts/start_ptp_dual_host.sh`（unicast ptp4l 双主机）。RX 侧 `rx/media_clk.c` 用 ST22 MEDIA_CLK 首帧锚定媒体时钟。

### 2.8 播放调度

- `tx/tx_scheduler.c`：多 lane 结构（`tx_scheduler` L33、`tx_video_lane` L24，支持 PvD 多路 `MP4_PIP_MAX_LANES`）。`tx_scheduler_create`（L429）建 lane + `mtl_tx_audio`（L463-478，含 AAC→PCM 解码 `aac_decode_init`）；`tx_scheduler_run`（L487-535）启动每 lane 视频线程 + 音频线程。
- 每 lane 节奏（`tx_publish_video_frame` L136）：demux 读 AU → acquire st22 fb → 拷入 → publish；实际 pacing 由 MTL `USER_PACING` + TAI 时间戳完成。
- 换源/循环：`tx/mp4_playlist.c` 在 EOF 时 reopen/seek。
- RX 调度：`rx_compositor_start`（L498）起 lane 线程 + render 线程；`playout_buffer.c` 按 `av_sync_playout_target_ns` 精确弹帧（早/晚 → sleep 或 drop）。

### 2.9 端到端数据链路（zero-copy 标注）

| 环节 | 硬件/机制 | zero-copy? | 软件回退 |
|---|---|---|---|
| Demux/BSF | CPU（FFmpeg） | — | 本身即软件 |
| ST22 TX | CPU + NIC（MTL/AF_XDP） | fb 内 memcpy | `kernel:` socket 后端 |
| ST22 RX | CPU + NIC | MTL fb → slab 拷贝一次 | — |
| 解码 | MPP VDEC（RK VPU） | 输出 DMABUF fd | 无软解 fallback（失败仅 dump 重试） |
| 缩放/定位 | DRM plane 硬件缩放 | NV12 DMABUF 直通 `drmModeAddFB2` + `drmModeSetPlane` | 找不到 NV12 plane 则退回全屏 CRTC page-flip |
| 合成 | KMS 多 plane（单 CRTC） | 是 | 无 RGA |
| 音频 | ALSA CPU 写入 | memcpy writei | — |
| 时钟 | CLOCK_TAI（linuxptp） | — | fallback realtime + TAI offset |

---

## 3. 两应用的共享与差异

### 3.1 重复模块（各目录内副本，非编译期共享）

| mp4_dist | end2end 对应 | 说明 |
|---|---|---|
| `shared/mpp_decoder.c` | `codec/mpp_decoder.c` | 同经 `codec_plugin.h` 抽象；end2end 另有 mpp_encoder、rga_converter |
| `shared/drm_renderer.c` | `io/drm_renderer.c` | end2end 为单窗口 legacy page-flip；mp4_dist 增加多 plane 合成 |
| `shared/ring_buffer.c` | `pipeline/ring_buffer.c` | 同构 SPSC ring |
| `shared/slab_pool.c` | `hal/slab_pool.c` | 同构 malloc slab 池；end2end 另有闲置的 `hal/dma_allocator.c` |
| `mtl_rx_bridge.c` / `pipeline_common.h` / `rx_pipeline.c` | `pipeline/` 同名文件 | 同构 RX 管线 |

### 3.2 关键差异

- **TX 侧硬件**：end2end 有完整的 V4L2 采集 + RGA CSC + MPP 编码硬件链路；mp4_dist 的 TX 是纯 CPU（demux 直发已有码流，无编码器、无 RGA）。
- **合成**：end2end 单窗口 page-flip；mp4_dist 用 DRM 多 overlay plane 硬件合成多窗口，RGA 完全未用。
- **音频**：仅 mp4_dist 有 ALSA + ST2110-30 + AV sync。
- **PTP**：end2end 用 MTL 内部 PTP（`mtl_ptp_read_time`）；mp4_dist 用外部 linuxptp + `CLOCK_TAI`。
- **协议**：两者视频均为 ST2110-22（ST22 frame-level），mp4_dist 额外有 ST2110-30 音频。



