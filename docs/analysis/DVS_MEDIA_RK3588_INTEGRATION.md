# dvs_media 与 rk3588 app 的集成分析

> 分析对象：`app/dvs_media`（DVS 节点媒体引擎，纯 C）
> 对照对象：`app/rk3588_file_playout`、`app/rk3588_live_ingest`、`app/rk3588_common`
> 结论先行：**dvs_media 由 `rk3588_file_playout` 的 RX 侧演进而来，通过 meson in-tree 编译直接复用其源码（非复制）；与 `rk3588_live_ingest` 目前只有接口层面的预留，尚未接入其数据面。**

---

## 1. 集成方式：in-tree 编译复用，不复制源码

`app/dvs_media/meson.build` 是一个**独立的 meson 工程**（不是 `app/meson.build` 的子工程）。它不把 `rk3588_file_playout` / `rk3588_common` 的源文件复制到本地，而是直接用相对路径 `files('../rk3588_file_playout/...')` 把上游源码编进本工程的 `static_library`，再链接到 `dvs_media` 可执行文件。

这种"借用源码 + 本工程额外封装一层控制面"的模式，正是 AGENTS.md 中描述的"由 file_playout 演进而来"的工程落地。

三个静态库构成复用骨架：

| 静态库 | 来源 | 内容 |
|---|---|---|
| `dvs_shared` | `rk3588_common/src/*` + `rk3588_file_playout/shared/*` | 无锁环、slab 池、定时器、DRM 模式、布局、PTP、E2E 时间戳、MPP 解码、DRM 渲染等基础组件 |
| `file_playout_probe` | `rk3588_file_playout/tx/mp4_demux.c` | FFmpeg 解封装（MP4 probe） |
| `dvs_rx` | `rk3588_file_playout/rx/*` + 部分 `shared/*` + `tx/extradata_handler.c` | 接收/解码/合成/播放缓冲/AV 同步/ALSA 出声的完整 RX 数据面 |

最终 `dvs_media` 可执行文件链接 `dvs_shared + dvs_rx + file_playout_probe`，并额外编译本工程 `engine/` 与 `ipc/`。

---

## 2. 具体复用的源文件清单

来自 `app/dvs_media/meson.build:46-119` 的精确映射：

**`rk3588_common/src/`（基础无状态组件，第一批已纳入 `sps_vui`、`sei_timestamp`、`display_layout`、`drm_mode`、`ptp_time`）**
- `ring_buffer.c` `slab_pool.c` `timer.c` `sps_vui.c` `sei_timestamp.c` `display_layout.c` `drm_mode.c` `ptp_time.c`
- 可选依赖 MTL（找到时以 `-DHAVE_MTL=1` 编译，见 `rk3588_common/meson.build`）

**`rk3588_file_playout/shared/`（跨应用共享的 RX/解码/显示辅助）**
- `au_trace.c` `tx_tai_ts.c` `e2e_stamp.c` `rx_e2e_stats.c`
- `mpp_decoder.c`（MPP 解码封装）`drm_renderer.c`（DRM 渲染）

**`rk3588_file_playout/rx/`（RX 数据面核心）**
- `alsa_audio.c` `playout_buffer.c` `av_sync.c` `media_clk.c` `mtl_rx_mp4.c` `mtl_rx_bridge.c` `mtl_rx_audio.c` `h264_prep.c` `rx_pipeline.c` `rx_compositor.c`

**`rk3588_file_playout/tx/`（仅取与 RX 路径共用的部分）**
- `mp4_demux.c`（编入 `file_playout_probe`）`extradata_handler.c`（编入 `dvs_rx`）

**根级配置**
- `rk3588_file_playout/file_playout_config.c`（编入 `dvs_media` 主目标）

---

## 3. 复用的接口/函数层（dvs_media 调用了哪些 file_playout 符号）

dvs_media 自身只写"控制面 + IPC"，所有数据面动作都委托给 file_playout 的公开符号（`engine_ctx.c` 等是直接证据）：

| 类别 | 复用符号 |
|---|---|
| 配置/初始化 | `file_playout_config_load` `file_playout_mtl_init` `file_playout_apply_drm_mode` `file_playout_mode_dims` `file_playout_pip_validate` |
| 合成器 | `rx_compositor_create` `rx_compositor_start` `rx_compositor_stop` `rx_compositor_free` `rx_compositor_try_fast_apply` `rx_compositor_report_stats` `rx_compositor_lane_*` |
| 数据结构 | `file_playout_cfg` `file_playout_pip_stream` `MP4_PIP_MAX_LANES`（`rk3588_common/include/rk3588_common/display_layout.h`，第一批已由 file_playout `shared/` 上移） |
| 编解码/渲染 | `mpp_decoder.c` `drm_renderer.c`（经 `rx_compositor` 间接调用） |
| MTL 公共 API | `mtl_version` `mtl_ptp_read_time` `mtl_stop` `mtl_uninit` |

注意 `engine_ctx.c` 直接 `#include "file_playout_config.h"` 与 `"display_layout.h"`（后者第一批已上移 `rk3588_common`），说明 dvs_media 把 donor 的头文件当作"半公开 API"使用——这是 in-tree 复用的代价：donor 的内部头对 dvs_media 完全可见，二者强耦合。

---

## 4. 与 rk3588_live_ingest 的关系：仅预留接口，未接入数据面

这是一个关键且容易被误读的点：

- `app/dvs_media/meson.build` **没有**包含任何 `rk3588_live_ingest/` 的源文件，也没有对应的 include 路径。
- dvs_media 的 TX 路径（`engine/tx_session.c` + `engine/tx_session.h`）是**纯参数校验 + 状态机**，刻意不依赖 MTL/MPP，以便在 x86 上单测。

`tx_session.h:54` 明确写道：

> the donor chain is `rk3588_live_ingest` capture/encode/TX taken per duty

也就是说，未来在单板 bring-up 时，会从 `rk3588_live_ingest` 按职责"借用" capture → encode → MTL TX 的数据面，通过 `dvs_tx_set_backend()` 注入（`dvs_tx_backend_fn` 钩子）。**当前状态**：backend 为 NULL，TX 启动后只报告 `DVS_TX_CONFIGURED` + `backend_pending`，绝不谎称包在流动（`tx_session.c:112-120` 的诚实规则）。

因此：
- **RX 侧（output role）**：完全复用 file_playout 的 RX 数据面。
- **TX 侧（input role）**：仅占位，live_ingest 的数据面尚未接入。

---

## 5. dvs_media 自己新增的"受控层"（file_playout 没有的部分）

演化方向是**受控运行**：由 `dvs_agent` 通过 IPC 驱动布局变更，而非 file_playout 的"命令行一次性跑完"。新增模块：

| 模块 | 职责 |
|---|---|
| `engine/engine_ctx.c` | 引擎上下文：MTL 生命周期、合成器构建/重建、`role` 互斥（RX/TX 二选一）、统计循环 |
| `engine/session_mgr.c` | 生命周期入口（对 IPC 层封装 `dvs_engine_*`） |
| `engine/session_admit.c` | 白名单 + LRU 准入（source 准入控制） |
| `engine/layout_executor.c` | 两阶段 `prepare`（暂存待定布局，不动运行画面）/ `commit`（换上待定布局）/ `abort` / `clear`；从 JSON 解析 `streams`/`layout` 到 `file_playout_pip_stream` |
| `engine/audio_arbiter.c` | ALSA 音频路由仲裁 |
| `engine/ipc_engine_bridge.c` | 把 IPC server 的 hooks（prepare/commit/clear/abort/counts/ptp_state/lane_stats/tx_start/tx_stop/tx_status）桥接到引擎 |
| `engine/capability.c` | 节点能力探测与上报（DRM 连接器/plane、V4L2 输入口、MTL/MPP/PTP 状态） |
| `ipc/ipc_server.c` | Unix socket IPC 服务 + `hello` 握手 |
| `ipc/state_machine.c` | `configure/prepare/commit/abort/clear/query_state` 六状态机 |

---

## 6. 共享版本契约：`dvs_common`

dvs_media 与 dvs_agent **同包发布**，版本契约在 `app/dvs_common/include/dvs_node_bundle.h`：
- `DVS_NODE_BUNDLE_VERSION`（包版本，当前 `"1"`）
- `DVS_MEDIA_IPC_VERSION`（IPC 协议代，当前 `1`）

`dvs_media_main.c` 在 `--version` 时打印二者，IPC 握手时双方比对，避免版本错配。该头文件用纯 `#define`，C（dvs_media）与 C++（dvs_agent）均可消费。

---

## 7. 编译与依赖

- 独立工程：`cd app/dvs_media && meson setup build && ninja -C build`
- **必须在编译服务器**构建（依赖 MTL / Rockchip MPP / libdrm / ALSA / FFmpeg / json-c）。
- MPP 为**必需**：meson 探测不到会直接 `error()` 退出（`meson.build:21-36`）。
- 依赖声明：`mtl`、`rockchip_mpp`、`libdrm`、`alsa`、`json-c`、`libavformat/avcodec/avutil`、`threads`。

---

## 8. 注意事项与潜在坑

1. **AGENTS.md 描述与代码不一致（重要）**：AGENTS.md 称 file_playout 的 `shared/` "each app carries its own copy"（各自持有一份副本）。但 dvs_media 实际是 **in-tree 直接 `#include`/编译 file_playout 的 `shared/` 与 `rx/` 源码**，并非复制。涉及这两处改动时，影响面会跨 app，需同步验证 file_playout 与 dvs_media。建议修正 AGENTS.md 或统一为复制。

2. **rx_compositor 来源**：dvs_media 复用的是 `rk3588_file_playout/rx/rx_compositor.c`，不要与 `io/drm_renderer.c` 或 file_playout 的 `shared/drm_renderer.c` 混淆（AGENTS.md 已提示存在近似重复模块）。改 `dvs_media` 的渲染/合成行为时确认目标文件是 `rx/rx_compositor.c`。

3. **强耦合 donor 内部头**：`engine_ctx.c` 直接引用 `file_playout_config.h` / `display_layout.h`（后者已上移 `rk3588_common`），任一 donor 重构这些头都会直接破坏 dvs_media。

4. **TX 占位未闭环**：input role 当前无数据面 backend，`dvs_tx_set_backend` 仅在 `tests/test_tx_session.c` 的单元测试中注入 fake backend。生产接入 live_ingest 数据面是后续 TODO。

5. **donor 源码跨 app 引用**：`rk3588_common/src/` 同时被三处引用；`rk3588_file_playout/shared/` 余下部分仍被 file_playout 自己与 dvs_media 引用。新增引用前先确认归属，避免回搬。

---

## 9. 集成关系图（文本）

```
                 rk3588_common (static: ring_buffer/slab/timer)
                          ▲
                          │ in-tree 编译
                          │
   rk3588_file_playout ───┤  rx/*  shared/*  tx/mp4_demux  file_playout_config
                          │  (RX 数据面 + 配置，被借用)
                          │
   rk3588_live_ingest ────┤  (仅 TX 数据面 donor chain，尚未接入；通过
                          │   dvs_tx_set_backend 预留接口)
                          │
                  ┌───────┴───────────────┐
                  │     dvs_media         │
                  │  engine/  (控制面)     │
                  │  ipc/     (IPC/状态机) │
                  │  dvs_media_main.c      │
                  └───────┬───────────────┘
                          │ 同包发布 + IPC 握手
                          ▼
                     dvs_agent / dvs_platform
```
