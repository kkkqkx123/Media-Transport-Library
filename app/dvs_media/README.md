# dvs_media — DVS 节点媒体引擎

节点侧媒体引擎（纯 C），由 `rk3588_file_playout` 的 RX 侧演进而来：接收 MTL ST 2110 流（视频+音频），解码合成后 DRM 显示、ALSA 出声。与 file_playout 的区别在于**受控运行**：支持 IPC `configure / prepare / commit / abort / clear / query_state`，布局可热更新，并对外上报节点能力（DRM/MPP/MTL 探测）。

复用 `rk3588_common` 与 `rk3588_file_playout` 的 `rx/`、`shared/`、`tx/mp4_demux` 源码（meson in-tree 编译，不复制），依赖 MTL、Rockchip MPP（必需）、libdrm、ALSA、json-c、FFmpeg（libavformat/avcodec/avutil）。需在编译服务器上构建：

```bash
cd app/dvs_media && meson setup build && ninja -C build
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
| `engine/` | 引擎核心：`engine_ctx`（引擎上下文）、`session_mgr`（会话管理）、`session_admit`（会话准入）、`layout_executor`（布局执行）、`audio_arbiter`（音频仲裁）、`capability`（能力探测上报）、`ipc_engine_bridge`（IPC→引擎桥接） |
| `ipc/` | `ipc_server`（Unix socket IPC 服务）+ `state_machine`（configure/prepare/commit 状态机） |
| `tests/` | `test_ipc` / `test_state_machine` / `test_session_admit` |
