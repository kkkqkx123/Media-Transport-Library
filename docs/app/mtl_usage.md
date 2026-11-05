# app 中 rk3588 应用与 dvs 对 MTL 的使用方式

本文整理 `app/` 下 RK3588 应用与 DVS 系列组件如何使用 Media Transport Library (MTL)。
重点不是 MTL 内部实现，而是**应用层怎样初始化 MTL、创建 ST 2110 会话、管理
framebuffer、接入 PTP 时间与 pacing**，以及各组件之间的差异。

## 0. 总结

| 组件 | 是否直接使用 MTL | 使用形态 | ST 会话类型 | PTP/时间 |
|------|------------------|----------|-------------|----------|
| `rk3588_live_tx` | 是 | 单进程 TX 或 RX，`both` 模式 fork 两个进程 | ST22 TX/RX（帧级，H.264 码流） | `mtl_ptp_read_time` 作为 TX/RX 流水线时钟 |
| `rk3588_playout_rx` | 是 | TX 与 RX 可分离进程/双机 | ST22 TX/RX（视频）+ ST30 TX/RX（音频） | 外部 PTP/monotonic/built-in 三种模式；TX 使用 TAI 时间戳 + 用户 pacing |
| `dvs_media` | 是（复用 file_playout RX 管线；已新增 TX 会话） | RX/显示节点 + TX 输入节点（角色互斥）；MTL 由引擎懒初始化 | ST22 RX（经 `rx_compositor` 多路）+ ST30 RX（音频）；TX 经 `dvs_tx` 会话（后端当前为桩） | `mtl_ptp_read_time` 用于布局定时与能力上报 |
| `dvs_agent` | 否 | 仅通过 UDS 查询 `dvs_media` 状态 | 无 | 心跳中转发 `query_state` 返回的 PTP 状态 |
| `dvs_platform` | 否 | 平台编排服务，不接触媒体传输 | 无 | 无 |

共同特征：

- 每个进程**一个 `mtl_handle`**，通过 `mtl_init` + `mtl_start` 创建，退出时
  `mtl_stop` + `mtl_uninit`。
- 媒体会话全部使用 **frame-level** ST22/ST30 会话，应用通过
  `get_next_frame` / `notify_frame_done` 或 `notify_frame_ready` 回调接入 MTL，
  不直接调用 packet API。
- framebuffer 由 MTL 分配，应用只保存地址并维护状态机/池化描述符；
  收包回调必须非阻塞，把帧移交给 ring buffer/队列。
- RX 路径统一遵循：回调中 `rx_pool_acquire` → `ring_push`；失败时
  `st*_rx_put_framebuff` 归还 framebuffer；停止时 drain ring 并归还。
- ARM 平台使用软件 pacing；PMD 类型从端口名前缀推断
  (`kernel:` / `native_af_xdp:` / `dpdk_af_xdp:` / `dpdk_af_packet:` / 默认 DPDK)。

## 1. 通用 MTL 初始化模式

### 1.1 live_ingest

文件：`app/rk3588_live_tx/live_tx_app.c`

- `mtl_setup_single()` 构造 `mtl_init_params`：
  - `num_ports = 1`，端口名写入 `port[MTL_PORT_P]`
  - `net_proto = MTL_PROTO_STATIC`，IP 写入 `sip_addr[MTL_PORT_P]`
  - TX/RX 队列数按模式设置（TX: `tx_q=1, rx_q=0`；RX: `tx_q=0, rx_q=1`）
  - 常用 flags：`MTL_FLAG_BIND_NUMA`、`MTL_FLAG_TX_VIDEO_MIGRATE`、
    `MTL_FLAG_RX_VIDEO_MIGRATE`、`MTL_FLAG_RX_SEPARATE_VIDEO_LCORE`
  - 单队列时追加 `MTL_FLAG_SHARED_TX_QUEUE` / `MTL_FLAG_SHARED_RX_QUEUE`
- `mtl_pmd_from_port()` 根据端口名前缀选择 PMD。
- `mtl_init()` 后必须 `mtl_start()`；失败时 `mtl_uninit()`。

### 1.2 file_playout

文件：`app/rk3588_playout_rx/file_playout_config.c` (`file_playout_mtl_init()`)

- 与 live_ingest 相同的单端口/static IP/PMD 模式。
- 增加 PTP 模式分支：
  - `FILE_PLAYOUT_PTP_BUILTIN`：`MTL_FLAG_PTP_ENABLE`（依赖 NIC 硬件 timesync）
  - `FILE_PLAYOUT_PTP_MONOTONIC`：仅相对锚点，不启用 MTL PTP
  - 默认外部模式：注册 `mtl_init_params.ptp_get_time_fn = mp4_ptp_mtl_time_fn`，
    `priv = &g_ptp_ctx`，由 linuxptp/CLOCK_TAI 提供时间
- 根据 `tx_q/rx_q` 追加 `MTL_FLAG_TX_VIDEO_MIGRATE` /
  `MTL_FLAG_RX_VIDEO_MIGRATE` / `MTL_FLAG_RX_SEPARATE_VIDEO_LCORE`
- 单队列时追加 shared RX/TX queue flags，避免 ST22+ST30 在同一端口互相覆盖
  xdp 队列条目。
- 支持 `lcores` 参数把 EAL lcore 钉到指定 CPU。

### 1.3 dvs_media

文件：`app/dvs_media/engine/engine_ctx.c`

- `dvs_engine_init_mtl()` / `dvs_engine_apply_layout_at()` 懒初始化 MTL：
  `file_playout_mtl_init(&cfg, rx_port, rx_lip, 0, 1, log_level, mtl_lcores_rx)`，
  即 **RX-only**（`tx_q=0, rx_q=1`），shared RX queue。
- `dvs_engine_stop()` 调用 `mtl_stop()` + `mtl_uninit()`。
- `dvs_engine` 持有 `mtl_handle st`，传递给 `rx_compositor` 与 PTP 读取。

## 2. rk3588_live_tx 的 MTL 使用

### 2.1 TX (`app/rk3588_live_tx/mtl_tx.c`)

- `mtl_tx_create()` 调用 `st22_tx_create(p->st, &ops)`。
- 会话参数：
  - `type = ST22_TYPE_FRAME_LEVEL`
  - `pack_type = ST22_PACK_CODESTREAM`
  - `fmt = ST20_FMT_YUV_420_8BIT`
  - `fps = st_frame_rate_to_st_fps(...)`
  - `ST22_TX_FLAG_DISABLE_BOXES`
  - `framebuff_cnt` 由配置决定（常见 8），`framebuff_max_size` 由配置决定（RX 端固定 512KiB）
  - `get_next_frame = tx_get_next_frame`
  - `notify_frame_done = tx_notify_frame_done`
- MTL 分配的 framebuffer 地址通过 `st22_tx_get_fb_addr()` 取出，TX 流水线直接写入。
- 应用维护 `FB_FREE/FILLING/READY/SENDING` 状态机：
  - `mtl_tx_acquire_fb()` 原子地把空闲 fb 标记为 `FILLING`，返回地址；
  - 编码完成后 `mtl_tx_publish_fb()` 标记 `READY`；
  - MTL 调用 `tx_get_next_frame()` 时把 `READY` 转为 `SENDING`，并填入
    `codestream_size`；
  - `tx_notify_frame_done()` 把 fb 标记回 `FREE` 并唤醒等待线程。
- `mtl_tx_start/stop` 只切换 `running` 原子标志，不直接操作 MTL。

### 2.2 RX / both 模式

当前 `rk3588_live_tx` 为 TX-only：已移除 `mtl_rx.c`、`mtl_rx_bridge.c`、`rx_pipeline` 与 `both` 自环模式，不再在同一应用内接收或解码。回环验证由单独的 `rk3588_playout_rx` 进程经网络完成；TX 与 RX 各自调用一次 `mtl_init`。

## 3. rk3588_playout_rx 的 MTL 使用

### 3.1/3.2 TX 视频 / TX 音频

当前 `rk3588_playout_rx` 为 RX-only：已移除 `tx/mtl_tx_video.c` 与 `tx/mtl_tx_audio.c`，不再在本应用内发送。发送由 `rk3588_live_tx` 提供；MTL TX API 的使用参见第 2 节。

### 3.3 RX 视频 (`rx/mtl_rx_mp4.c`)

- `st22_rx_create()`，`notify_frame_ready = rx_notify_frame_ready`。
- 回调中：
  - stopping 时归还 framebuffer；
  - `meta->status` 为 CORRUPTED/INCOMPLETE 时计数并归还；
  - acquire 16 槽描述符池；
  - `media_clk_frame_pts()` 把 `meta->tfmt/timestamp` 转为 TAI/ns；
  - `ring_push()` 到 RX ring；失败时归还；
  - `au_trace_record(AU_TRACE_RX_MTL, ...)`。
- `mtl_rx_mp4_stop()` drain 描述符池与 ring，全部 `st22_rx_put_framebuff()`。

### 3.4 RX 音频 (`rx/mtl_rx_audio.c`)

- `st30_rx_create()`，`notify_frame_ready = rx_notify_frame_ready`。
- 回调把帧放入 SPSC ring（`q_head/q_tail`），不阻塞 MTL lcore。
- 独立 `audio_play_thread` drain 队列，调用 ALSA 写入，并 `av_sync_note_audio_frame()`。
- `st30_rx_put_framebuff()` 在播放完成后归还 framebuffer。

### 3.5 多路合成 (`rx/rx_compositor`)

- `rx_compositor_create()` 接收 `mtl_handle mt`，内部为每一路流创建
  `mtl_rx_mp4` / `mtl_rx_audio` 会话。
- 使用 `mtl_ptp_read_time` 作为 `time_now`（外部/内置 PTP 模式）。
- `rx_compositor_try_fast_apply()` 在布局仅几何变化时复用已有 compositor，
  避免重建解码器。
- `dvs_media` 直接复用这套 compositor。

## 4. dvs 的 MTL 使用

### 4.1 dvs_media

- 本质是 RX/显示节点，复用 `rk3588_playout_rx` 的 RX 管线静态库
  (`dvs_rx` + `dvs_shared`)。
- MTL 生命周期：
  - `--config` 静态模式：`dvs_engine_start()` → `dvs_engine_apply_layout()`，
    内部懒初始化 MTL 并创建 compositor。
  - `--ipc-sock` 受控模式：若配置无初始流，调用 `dvs_engine_init_mtl()` 只起 MTL，
    不创建 compositor；布局通过 IPC `configure/prepare/commit` 应用。
- `dvs_engine_apply_layout_at()`：
  - 首次/无 MTL 时初始化 MTL；
  - `count <= 0` 清屏：停 compositor，保留 MTL；
  - 有 compositor 时先尝试 fast apply，失败则重建 compositor；
  - 重建时 `rx_compositor_start()` 启动所有 RX 会话。
- PTP：`engine_now_ns()` 在非 monotonic 模式使用 `mtl_ptp_read_time(e->st)`；
  `engine_wait_until()` 可按目标 TAI 时间延迟布局切换。
- TX 会话（演进中）：`engine/tx_session.c` 提供 `dvs_tx_start()`，仅当节点角色为 `input`
  时启动，经 `dvs_tx`（内部 backend 函数）建立 ST22 TX；当前数据面 backend 为桩
  （`backend_pending`，见 `engine/tx_session.c` 注释），真正的 ST22 TX 后端待单板
  bring-up 接入。因此实际媒体流仍以 RX/显示为主，但 MTL 已不再是严格意义上的 RX-only。
- 能力上报：`dvs_capability_refresh_mtl()` 调用 `mtl_version()` 与
  `mtl_ptp_read_time()`，填充 MTL 版本与 PTP 状态。
- IPC `ptp_state` hook 也读取 `mtl_ptp_read_time()`。
- `query_state` 返回的 rxSessions/decoding/layers/lanes 来自 compositor；
  当前 `session_mgr`/`session_admit` 等控制面已实现，但 `session_mgr.c` 注释中
  的 ST22 会话生命周期仍由 compositor 间接管理。

### 4.2 dvs_agent

- 不直接链接/初始化 MTL。
- 通过 `MediaIpcClient` 查询 `dvs_media` 的 `query_state`，获取 rxSessions、
  decoding、layers、ptp、lanes。
- `Registration::build_heartbeat()` 把这些状态转发到平台心跳。
- 因此 agent 对 MTL 的"使用"是**状态消费**，不是传输控制。

### 4.3 dvs_platform

- 不使用 MTL；负责节点注册/心跳网关、分组/墙/场景/绑定、布局像素拆分、
  跨节点 prepare/commit 屏障与回退。
- 平台下发的布局最终由 agent 转给 `dvs_media`，再由 `dvs_media` 通过 MTL
  建立/调整 RX 会话。

## 5. MTL API 使用清单（应用层）

| API | 使用位置 | 用途 |
|-----|----------|------|
| `mtl_init` / `mtl_start` / `mtl_stop` / `mtl_uninit` | live_ingest、file_playout、dvs_media | 实例生命周期 |
| `mtl_set_log_level` | live_ingest、file_playout | 日志级别 |
| `mtl_ptp_read_time` | live_ingest TX/RX、file_playout、dvs_media | 媒体时钟/布局定时/能力 |
| `mtl_version` | dvs_media capability | 版本上报 |
| `st22_tx_create/free/get_fb_addr` | live_ingest、file_playout | 视频 TX 会话 |
| `st22_rx_create/free/put_framebuff` | live_ingest、file_playout、dvs_media | 视频 RX 会话 |
| `st30_tx_create/free/get_framebuffer` | file_playout | 音频 TX 会话 |
| `st30_rx_create/free/put_framebuff` | file_playout | 音频 RX 会话 |
| `st_frame_rate_to_st_fps` | 所有 ST 会话创建处 | 帧率转换 |
| `st30_calculate_framebuff_size` | file_playout audio | 音频帧大小 |
| `st10_get_tai` / `st10_timestamp_fmt` / `media_clk_*` | file_playout RX | TAI 时间转换 |

## 6. 设计要点与注意事项

1. **回调非阻塞**：`notify_frame_ready` 在 MTL lcore 上执行，只能做 acquire/push，
   不能 malloc、加锁、sleep 或打 INFO 日志。
2. **Framebuffer 生命周期**：RX 回调中失败路径必须 `put_framebuff`；停止时必须
   drain ring 并归还；否则 MTL 会话卡死或内存泄漏。
3. **池化描述符**：应用额外维护 `rx_pkt` 描述符池，与 MTL framebuffer 分离，
   避免 ring 中直接持有 MTL 指针导致生命周期混乱。
4. **单队列共享**：同一端口上 ST22+ST30 并存时必须启用 shared RX/TX queue，
   否则第二个会话覆盖队列条目导致崩溃。
5. **进程隔离**：`both`/TX+RX 同机时使用多进程，每个进程独立 `mtl_init`；
   不要在同一进程内重复初始化 DPDK EAL。
6. **TAI 与 pacing**：file_playout TX 在 `use_tai_timestamp` 时启用用户时间戳与
   用户 pacing；RX 用 `media_clk` 把 MTL timestamp 转为 TAI/ns 供 playout/AV sync。
7. **dvs 的 TX 仍在演进**：`dvs_media` 以 RX/显示链路为主，但已引入 `dvs_tx` 会话
   （`engine/tx_session.c`）；该 TX 在节点角色为 `input` 时可启动，当前数据面 backend 为桩
   （`backend_pending`），尚未建立真正的 ST22 TX，发送端实际仍由 `rk3588_playout_rx`
   或其它 TX 节点承担。

## 7. 当前缺口（与 dvs 相关）

- `dvs_media` 的 `session_mgr`/`engine` 已能创建/销毁 compositor，但
  `capability.c` 的 MTL 版本/PTP 状态仍以 probe 时的默认值为主，只有
  `dvs_capability_refresh_mtl()` 在 MTL 初始化后更新。
- `dvs_agent` 的注册/心跳网络通道与维护动作仍为 stub；MTL 状态只能通过
  `query_state` 间接触达。
- `dvs_platform` 不直接感知 MTL，所有传输状态经节点 agent 汇总。
