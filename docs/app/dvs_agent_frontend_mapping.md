# dvs_agent 本机前端（node-admin）功能与 dvs / rk3588 app 功能映射分析

> 范围：`app/dvs_agent/web/src/pages/node-admin/node-admin.html` + `node-admin.js` 前端功能，
> 映射到 `app/dvs_media`（dvs 节点媒体引擎）、`app/dvs_agent` 自身后端、以及 `app/rk3588_live_ingest` / `app/rk3588_file_playout`（rk3588 数据面 app）。
> 目标：明确前端每个操作最终由哪一层执行，避免把“仅落库/原型”误认为“已生效”。

## 1. 分层约定

| 层 | 代码位置 | 是否接触 MTL/硬件 |
|---|---|---|
| 本机前端 | `app/dvs_agent/web/src/pages/node-admin/` | 否，纯浏览器 |
| 节点代理控制面 | `app/dvs_agent/main.cpp`、`node_store.cpp`、`media_ipc.cpp`、`maintenance.cpp` | 否 |
| 节点媒体引擎 | `app/dvs_media`（IPC + MTL RX + MPP/DRM/ALSA） | 是 |
| rk3588 数据面参考实现 | `app/rk3588_live_ingest`、`app/rk3588_file_playout` | 是 |

说明：`dvs_media` 当前**只复用 file_playout 的 RX 管线**（`mtl_rx_mp4.c`、`mtl_rx_audio.c`、`rx_compositor.c` 等），不复用 TX；因此前端“输入盒/采集口”相关功能在 rk3588 侧只能对应到 `rk3588_live_ingest` 的采集链，尚未接入控制面。

## 2. 前端区域 → 后端映射总表

| 前端区域/功能 | dvs_agent 后端 | dvs_media（dvs app） | rk3588 app | 当前状态 |
|---|---|---|---|---|
| 登录 `/node/login`、改密 `/node/password` | `main.cpp` 登录路由、`node_store` SHA-256 密码校验 | 无 | 无 | 原型级：固定 session `sess-1`，严格鉴权默认关闭 |
| 节点角色（输出盒/输入盒） | `node_store` role/pendingRole；scope `role` 状态机；`/api/v1/node/config/role` | IPC `configure` scope `role`（仅转发，`session_mgr` 无角色执行器） | `rk3588_live_ingest` 有 tx/rx/both 运行模式，但未被代理驱动 | 状态机有，执行器无 |
| 输入口选择 | scope `input`；保存时把输入配置转发为 `whitelist` | `session_admit` 白名单（`dvs_session_whitelist_set`）；`/configure` hook `dvs_layout_prepare_from_json` | `rk3588_live_ingest`：`v4l2_capturer` → `rga_converter` → `mpp_encoder` → MTL TX | 前端保存 → 代理 → IPC 链路存在；白名单语义部分接通 |
| 输出设置（端口/分辨率/刷新率/强推/热插拔） | scope `output`；`/api/v1/node/config/output` | `file_playout_apply_drm_mode`、`file_playout_mode_dims`、`rx_compositor` DRM plane；`capability` 端口能力 | `rk3588_file_playout`：`shared/drm_renderer` 多 plane、`shared/display_layout`；`rk3588_live_ingest` DRM 渲染 | 配置落库 + IPC 转发；真实 DRM mode 切换未执行 |
| 网络配置（DHCP/静态） | scope `network`；仅落库 | 无 | 无 | stub：无 NetworkManager/系统调用 |
| 注册方式/设备编号/立即注册 | scope `registration`；注册线程占位 | 无（媒体不参与注册） | 无 | stub：无真实平台注册/心跳外发 |
| SIP 接入 | scope `sip`；落库 | 无 | 无 | 可选，V1.0 未实现 |
| 播放任务投影（当前播放/任务列表/记录） | `/api/v1/node/media/status` 代理 `dvs_media`；心跳上报媒体摘要 | `query_state`：`rx_sessions/decoding/layers/ptp`；`apply_layout` / `rx_compositor` | `rk3588_file_playout`：`playout_buffer`、`av_sync`、`rx_compositor` 多 lane | 代理可读状态；任务操作被前端禁用（提示去平台） |
| 本机信息 | `/api/v1/node/info`、`/node/capability`、`/node/capacity` | `capability.c`：DRM connector/mode、max_decode_sessions、max_layers、PTP、MTL 版本 | `rk3588_file_playout` 共享 `display_layout`（lane 上限 8）、MPP 解码；`live_ingest` DRM | 能力探测多为默认值/占位 |
| 维护操作（重启/恢复出厂/升级） | `/api/v1/node/reboot`、`/factory-reset`、`/upgrade` | 无 | 无（系统级动作） | stub：返回固定 ok，不执行实际动作 |
| 硬件能力基线（`hardware-capabilities.js`） | `/api/v1/node/capability` 返回节点 capabilities | `dvs_capability_probe` / `dvs_capability_refresh_mtl` | `rk3588_file_playout` `shared/display_layout.h`（8 lane 上限、NV12 直通 4 路） | 前端 profile 为硬编码基线，应改为节点上报 |

## 3. 关键链路说明

### 3.1 输入口 → 白名单 → 会话准入

1. 前端 `zone-input` 保存输入口配置。
2. 代理 `set_desired("input", value)`，更新 `configStates.input` 与 revision。
3. 代理 IPC 转发 `configure { whitelist: value }` 给 `dvs_media`。
4. `dvs_media` `configure` hook 调用 `dvs_layout_prepare_from_json`；白名单最终应调用 `dvs_session_mgr_set_whitelist` → `dvs_session_whitelist_set`。
5. `dvs_engine_apply_layout` 在创建 compositor 前调用 `dvs_session_admit`，拒绝未白名单源或 LRU 淘汰。

rk3588 侧对应功能：`rk3588_live_ingest` 的 V4L2/MPP/RGA 采集与 MTL TX 发流；`rk3588_file_playout` 的 MP4 demux/TX 音视频发送。它们提供“信号源”本身，但当前没有被代理动态控制。

### 3.2 输出设置 → DRM mode

1. 前端 `zone-output` 保存分辨率/刷新率/输出口/模式。
2. 代理 scope `output` 落库并 IPC 转发 `configure { scope: "output", value }`。
3. `dvs_media` 复用 `file_playout_apply_drm_mode` / `file_playout_mode_dims` 计算 DRM mode。
4. 实际 plane 创建在 `rx_compositor_create`（`shared/drm_renderer` 多 plane）中完成。

rk3588 侧对应功能：`rk3588_file_playout` 的 DRM 多 plane 渲染与 `display_layout` 布局；`rk3588_live_ingest` 的 DRM 渲染。当前 dvs_media 已复用这些代码，但前端保存不会触发重新 apply。

### 3.3 播放任务投影

1. 前端 `zone-playback` 只投影当前播放，提示完整任务列表在平台。
2. 代理 `/api/v1/node/media/status` 调用 `media_ipc.query_state()`。
3. `dvs_media` 返回 `query_state` envelope，字段含 `rxSessions/decoding/layers/ptp`。
4. rk3588 侧对应功能：`rk3588_file_playout` 的 `playout_buffer`、`av_sync`、`rx_compositor` 统计；`rk3588_live_ingest` 的 RX bridge/解码/DRM。

### 3.4 能力上报

1. 前端 `hardware-capabilities.js` 的 `resolve(node)` 从节点 `capabilities` 读取端口与 safeModes。
2. 代理 `/api/v1/node/capability` 返回节点种子中的 capabilities。
3. `dvs_media` `capability.c` 负责探测 DRM connector/mode、MPP 解码路数、plane 数、PTP、MTL 版本。

rk3588 侧对应功能：`rk3588_file_playout` 的 lane 上限与 NV12 直通路数；`rk3588_live_ingest` 的 MPP 解码/DRM。当前探测多为默认值，前端仍以硬编码 profile 为主。

## 4. 缺口与结论

- dvs_agent 前端的“配置状态”展示的是 desired/applied 状态机，但 `set_desired` 之后**没有执行器**把配置应用到 OS 或媒体引擎（网络、输出 mode、角色切换均未执行）。
- 维护操作（reboot/factory-reset/upgrade）为假回执，未调用系统或升级脚本。
- 注册/心跳线程为空转，前端“立即注册”不会真正连平台。
- 输入口/输出设置的保存会 IPC 转发到 dvs_media，但 dvs_media 的 `configure` hook 目前只调用布局 prepare，白名单/输出 mode 的真实应用仍不完整。
- rk3588 app 提供了采集、编解码、MTL 收发、DRM/ALSA 的底层能力，但尚未被代理动态编排；当前只能作为 `dvs_media` 复用的 RX 管线与参考实现。
