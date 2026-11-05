# dvs_media — DVS 节点媒体引擎

节点侧媒体引擎（纯 C），由 `rk3588_playout_rx` 的 RX 侧演进而来，并已接入 `rk3588_live_tx` 的 TX 链路：output角色接收 MTL ST 2110 流（视频+音频），解码合成后 DRM 显示、ALSA 出声；input角色经 V4L2 捕获→RGA→MPP 编码→MTL ST22 发送。与 file_playout 的区别在于**受控运行**：支持 IPC `configure / prepare / commit / abort / clear / query_state`，布局可热更新，并对外上报节点能力（DRM/MPP/MTL 探测）。

复用 `rk3588_common` 与 `rk3588_playout_rx` 的 `rx/`、`shared/`（含 `mp4_demux` 探针与 `extradata_handler` 解码辅助）源码，以及 `rk3588_live_tx` 的捕获/编码/发送链路（`hal`/`io/v4l2_capturer`/`codec`编码侧/`pipeline/tx_pipeline`/`mtl_tx`，meson in-tree 编译，不复制），依赖 MTL、Rockchip MPP（必需）、libdrm、ALSA、json-c、FFmpeg（libavformat/avcodec/avutil）。缺 RGA 时 TX 后端不链接，TX 只报 `configured+pending`；需在编译服务器上构建：

```bash
cd app/dvs_media && meson setup build && ninja -C build
./build/dvs_media --version   # 包版本与 IPC 版本（须与 dvs_agent 同包）
```

## 运行

```bash
dvs_media --config <static.json>              # 独立模式：静态配置直接运行
dvs_media --config <cfg> --ipc-sock <path>    # 受控模式：空闲启动，由 IPC 驱动布局变更
```

## 结构

| 目录/文件 | 说明 |
|---|---|
| `dvs_media_main.c` | 入口：能力探测、配置加载、引擎创建，选择独立/受控模式 |
| `engine/` | 引擎核心：`engine_ctx`（引擎上下文，含 input/output 互斥与 TX 编排）、`session_mgr`（会话管理）、`session_admit`（会话准入）、`layout_executor`（布局执行）、`audio_arbiter`（音频仲裁）、`capability`（能力探测上报，含 V4L2 输入口）、`ipc_engine_bridge`（IPC→引擎桥接）、`tx_session`（TX 参数校验+状态机）、`tx_port_map`（捕获端口映射）+`tx_backend_live_ingest`（TX 数据面，有 MPP/RGA 时链接） |
| `ipc/` | `ipc_server`（Unix socket IPC 服务，含 `hello` 握手）+ `state_machine`（configure/prepare/commit 状态机） |

## 节点包

与 `dvs_agent` 同包发布（见 `../../deploy/dvs-node/`，版本见 `../dvs_common/include/dvs_node_bundle.h`）。配置下发统一走单一 `configure` 谓词；`hello` 上报协议版本、包版本与状态快照，供代理核对同包配对。
| `tests/` | `test_ipc` / `test_state_machine` / `test_session_admit` / `test_tx_session` / `test_tx_port_map` |
