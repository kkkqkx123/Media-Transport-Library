# RK3588 三个 app 与 DVS app 的关系及 MTL 使用分析

> 仓库：`kkkqkx123/Media-Transport-Library`（通过 gh-proxy.com 拉取，`--depth=1`）
> 分析范围：`app/` 目录下的 `rk3588_common`、`rk3588_live_tx`、`rk3588_playout_rx` 与 `dvs_*`（dvs_common / dvs_agent / dvs_media / dvs_platform）
> 约定：下文"**3 个 rk3588 app**"指 `rk3588_common`、`rk3588_live_tx`、`rk3588_playout_rx`；"**dvs app / DVS 媒体应用**"特指承担数据面、真正调用 MTL 的 `dvs_media`（其余 `dvs_common/agent/platform` 为控制面基础设施，文中一并说明）。

---

## 0. 结论速览

| 维度 | 结论 |
|---|---|
| 三者与 dvs 的关系 | `dvs_media` 由 `rk3588_playout_rx` 的 **RX 侧演进而来**，并通过 meson **in-tree 编译**（不复制）复用 `rk3588_common`、`rk3588_playout_rx` 的 `rx/`、`shared/`，以及 `rk3588_live_tx` 的 TX 数据面（捕获/编码/发送）。`rk3588_live_tx` 是 TX-only 编码发送端，`rk3588_playout_rx` 是 RX-only 解码接收端。 |
| MTL 角色 | MTL 只服务于**数据面**：`rk3588_live_tx`（TX-only）、`rk3588_playout_rx`（RX-only）、`dvs_media`（RX + input 角色 TX）。控制面 `dvs_agent/平台` 完全不接触 MTL。 |
| 抽象层次 | `rk3588_common` = 无状态基础组件库；`live_ingest` / `file_playout` = 端到端参考实现（demo）；`dvs_media` = 受控的生产级媒体引擎（替换 file_playout RX）。 |
| 复用方式 | `dvs_media/meson.build` 直接把 `rk3588_playout_rx/*.c` 编进自己的静态库/可执行文件（in-tree，零拷贝、零分叉），保证算法单一来源。 |

---

## 1. 模块定位与依赖关系

### 1.1 三个 rk3588 app

| 模块 | 类型 | 职责 | 与 MTL 的关系 | 构建方式 |
|---|---|---|---|---|
| `rk3588_common` | 纯 C 静态库 | 跨应用复用的无状态基础组件：`ring_buffer`（无锁环形缓冲）、`slab_pool`（预分配内存池）、`timer`（单调定时）、`pipeline_common.h`（dmabuf_frame / codec_packet 等跨应用数据结构）、`codec_plugin.h`（编解码插件接口） | 可选依赖：找到 MTL 时 `-DHAVE_MTL=1` 编译，找不到也能独立构建（`rk3588_common/README.md:3`） | 作为 `rk3588_common_dep` 被引用 |
| `rk3588_live_tx` | 独立 meson 工程（不入 `app/meson.build`） | HDMI 实时采集 → RGA 色彩转换 → MPP 硬编码（H.264）→ **MTL TX（ST2110-22）**；文件帧源插件可选；端到端编码发送端 | **TX-only**：只发，不收 | `cd app/rk3588_live_tx && meson setup build && ninja` |
| `rk3588_playout_rx` | 独立 meson 工程（RK3588 交叉编译） | MTL ST2110-22（视频）/ ST2110-30（音频）接收；RX 侧 MPP 硬解 + DRM 显示 + ALSA 出声 | **RX-only**：只收，不发 | `cd app/rk3588_playout_rx && meson setup build && ninja` |

### 1.2 dvs 套件四个目录

| 模块 | 类型 | 职责 | 是否用 MTL |
|---|---|---|---|
| `dvs_common` | C++17 源文件集合（HTTP server/client、SHA-256、内置 SQLite） | 控制面共享基础设施，被 `dvs_agent` 与 `dvs_platform` in-tree 编入各自二进制 | 否 |
| `dvs_agent` | C++17 可执行文件 | 运行在每个 RK3588 节点上的**节点代理**：注册/心跳、经 IPC 驱动本机 `dvs_media`、维护、本地 HTTP API + Web 前端 | 否 |
| `dvs_media` | 纯 C 可执行文件 | **节点媒体引擎**：RX 角色接收 MTL ST2110 流并解码合成显示/出声；input 角色复用 live_tx TX 数据面采集/编码/发送；受控运行（IPC 驱动 configure/prepare/commit） | **是（RX + input TX）** |
| `dvs_platform` | C++17 可执行文件 | 平台侧控制服务：节点注册/心跳网关、分组/墙/场景/绑定文档、布局像素拆分、跨节点两阶段提交屏障、批量升级 | 否 |

### 1.3 依赖与复用拓扑

```mermaid
graph TD
    subgraph 控制面[控制面 / 无 MTL]
        PL[dvs_platform<br/>:8081 中心服务]
        AG[dvs_agent<br/>每节点 :8080]
        CM[dvs_common<br/>HTTP/SHA/SQLite]
        AG -->|in-tree 编入| CM
        PL -->|in-tree 编入| CM
    end

    subgraph 数据面[数据面 / 用 MTL]
        DM[dvs_media<br/>节点媒体引擎]
        RK[rk3588_common<br/>基础库]
        LI[rk3588_live_tx<br/>双向演示]
        FP[rk3588_playout_rx<br/>MP4 TX/RX 演示]
        DM -->|in-tree 复用源码| RK
        DM -->|in-tree 复用源码| FP
        LI -->|in-tree 复用源码| RK
        FP -->|in-tree 复用源码| RK
    end

    AG -->|Unix socket IPC| DM
    AG -->|HTTP 注册/心跳/升级| PL
```

关键事实（`dvs_media/meson.build:14-94`）：

- 复用 `rk3588_common/src/{ring_buffer,slab_pool,timer}.c`（`-DHAVE_MTL=1`）。
- 复用 `rk3588_playout_rx/shared/*`（au_trace、drm_mode、display_layout、ptp_time、tx_tai_ts、e2e_stamp、rx_e2e_stats、mpp_decoder、drm_renderer、mp4_demux、extradata_handler）。
- 复用 `rk3588_playout_rx/rx/*`（alsa_audio、playout_buffer、av_sync、media_clk、mtl_rx_mp4、mtl_rx_bridge、mtl_rx_audio、h264_prep、rx_pipeline、rx_compositor）。
- 复用 `rk3588_live_tx` TX 数据面（hal DMA、io/v4l2_capturer、codec/rga_converter & mpp_encoder、pipeline/tx_pipeline、mtl_tx）作为 input 角色。
- 自身仅新增控制/引擎层：`engine/`（engine_ctx、session_mgr、session_admit、layout_executor、audio_arbiter、capability、ipc_engine_bridge）、`ipc/`（ipc_server、state_machine）、`dvs_media_main.c`。

> 即：`dvs_media = file_playout RX 管线 + 受控引擎外壳 + IPC 控制面`。这正是 `dvs_media/README.md:5` 所述"由 `rk3588_playout_rx` 的 RX 侧演进而来"。

---

## 2. 三者如何使用 MTL

所有 MTL 调用都收敛到 `st_api.h` / `mtl_api.h` 的 ST22/ST30 会话 API。三者的使用形态对比：

| 形态 | `rk3588_live_tx` | `rk3588_playout_rx` | `dvs_media` |
|---|---|---|---|
| 方向 | TX-only | RX-only | RX + input TX |
| 视频 | `st22_tx_create` / `st22_rx_create`（ST22 帧级 codestream，YUV420-8bit，PT=112） | `st22_tx_create`（视频）/ `st22_rx_create`（mtl_rx_mp4.c） | 复用 file_playout 的 `mtl_rx_mp4.c`（`st22_rx_create`） |
| 音频 | 文件帧源可选 ST30 TX（`mtl_tx_audio.c`）；HDMI 帧源无音频 | `st30_rx_create`（`mtl_rx_audio.c`，ST2110-30 PCM） | 复用 file_playout 的 `mtl_rx_audio.c`（`st30_rx_create`）及 live_tx 的 `mtl_tx_audio.c` |
| 时钟 | PTP（内置或外部） | 外部 PTP → `CLOCK_TAI`，`ptp_get_time_fn`；USER_TIMESTAMP + TAI 时间轴 | 复用 `ptp_time.c`，引擎用 `mtl_ptp_read_time` 锚定（`engine_ctx.c:176`、`ipc_engine_bridge.c:38`） |
| 帧缓冲 | app 侧 `fb_addr[i] = st22_tx_get_fb_addr(...)`，生产者/消费者环形状态机（`mtl_tx.c:118-123`） | 由 `file_playout_mtl_init` 统一创建会话 | 由 `file_playout_mtl_init` 统一创建会话（`engine_ctx.c:106-121`） |
| 背压 | RX 描述符池 + ring，池耗尽 drop 并告警（`mtl_rx.c:70-104`） | RX 描述符池 + ring（`mtl_rx_mp4.c:30-94`） | 同 file_playout RX |

### 2.1 `rk3588_live_tx` 的 MTL 用法（TX-only 示例）

**TX（`mtl_tx.c:87-119`）**：构造 `struct st22_tx_ops`，设为 `ST22_TYPE_FRAME_LEVEL`、`ST22_PACK_CODESTREAM`、`ST20_FMT_YUV_420_8BIT`、`ops.payload_type = 112`、关闭 boxes（`ST22_TX_FLAG_DISABLE_BOXES`）；注册 `get_next_frame` / `notify_frame_done` 回调；`framebuff_cnt` 帧缓冲由 `st22_tx_get_fb_addr` 取回，应用把编码码流直接写入帧缓冲后 `publish_fb`。这是**帧级零拷贝直写**模式。

**RX**：当前 `rk3588_live_tx` 已移除 RX 会话与解码/显示链路，不再调用 `st22_rx_create`；回环验证由 `rk3588_playout_rx` 进程完成。

### 2.2 `rk3588_playout_rx` 的 MTL 用法

- **RX 视频**（`rx/mtl_rx_mp4.c`）：`st22_rx_create`，`notify_frame_ready` 中做 `ST_FRAME_STATUS_COMPLETE/RECONSTRUCTED` 校验，按 `meta->timestamp` 经 `media_clk_frame_pts` 锚定到 TAI，再推 ring。
- **RX 音频**（`rx/mtl_rx_audio.c`）：`st30_rx_create`，帧放入 SPSC ring，独立播放线程 drain 并 ALSA 写入。
- **MTL 初始化**：`file_playout_mtl_init(...)` 统一创建 MTL 会话句柄 `struct mtl_session *st`（RX-only）。
- 发送由 `rk3588_live_tx` 提供，本应用不再包含 TX 会话。

### 2.3 `dvs_media` 的 MTL 用法（受控 RX + input TX）

- 引擎**只在需要时才初始化 MTL**（`engine_ctx.c` 的 `dvs_engine_init_mtl`，以及布局应用时懒初始化双端口）。
- RX 能力来自复用：`mtl_rx_mp4.c`、`mtl_rx_audio.c`、`mtl_rx_bridge.c`，会话创建逻辑与 file_playout 完全一致。
- input 角色 TX 能力来自 `rk3588_live_tx` 数据面（`tx_backend_live_ingest.c`），通过 `mtl_tx_create/start/stop/free` 与 `tx_pipeline` 调用。
- 受控模式下引擎**空闲启动**（`--ipc-sock`），由 IPC 的 `configure/prepare/commit` 才真正 `apply_layout` 创建/调整 RX 会话（`dvs_media_main.c:71-86`、`engine_ctx.c:182-311`）。
- 时间同步通过 `mtl_ptp_read_time(st)` 注入合成器与音频同步（`engine_ctx.c:175-178`）。

```c
// engine_ctx.c:106-117  — dvs_media 的 MTL 初始化入口（与 file_playout 同源）
int dvs_engine_init_mtl(dvs_engine* e) {
    if (e->st == NULL) {
        e->st = file_playout_mtl_init(&e->cfg, e->cfg.rx_port, e->cfg.rx_lip, 0, 1,
                                  e->cfg.mtl_log_level, e->cfg.mtl_lcores_rx);
        ...
    }
}
```

---

## 3. 关键关系总结

1. **演进关系（代码血缘）**：`dvs_media` ⇒ `rk3588_playout_rx`（RX 侧）+ `rk3588_live_tx`（TX 数据面）；三者共享 `rk3588_common`。`rk3588_live_tx` 与 `rk3588_playout_rx` 是独立的 TX-only / RX-only 参考实现。
2. **职责分层**：`rk3588_common`（地基）→ `rk3588_*`（参考实现/demo）→ `dvs_media`（生产受控引擎）；`dvs_common/agent/platform`（控制面）与数据面正交，仅在每节点由 `dvs_agent` 通过 IPC 驱动 `dvs_media`。
3. **MTL 收敛**：三者的 MTL 使用高度同构（ST22/ST30 + PTP/TAI + 帧缓冲/ring + 背压），差异仅在"是否双向"和"是否受控"。`dvs_media` 把 file_playout 的 RX 直接复用，几乎没有 MTL 层的新代码，体现出"算法单一来源"的复用纪律。
4. **构建隔离**：所有 rk3588/dvs 数据面 app 都是**独立 meson 工程**，不接入顶层 `app/meson.build`，需各自 `meson setup && ninja`（便于在编译服务器上按依赖分阶段构建）。

---

## 4. 待核实/开放点

- `dvs_media` 与 `rk3588_playout_rx` 的 RX 是**源码级 in-tree 复用**而非独立库链接，长期维护需关注两处同步（改 file_playout RX 会直接波及 dvs_media，反之亦然）。
- `dvs_media` 已复用 `rk3588_live_tx` 的 TX 数据面作为 input 角色（`tx_backend_live_ingest.c`）；生产节点同一时刻只运行 input 或 output 一种角色，MTL 在进程内初始化一次并同时持有 RX 与 TX 端口。
