# dvs_agent 本机前端（node-admin）功能与 dvs / rk3588 app 功能映射分析

> 范围：`app/dvs_agent/web/src/pages/node-admin/node-admin.html` + `node-admin.js` 前端功能，
> 映射到 `app/dvs_media`（dvs 节点媒体引擎）、`app/dvs_agent` 自身后端、以及 `app/rk3588_live_tx` / `app/rk3588_playout_rx`（rk3588 数据面 app）。
> 目标：明确前端每个操作最终由哪一层执行，避免把“仅落库/原型”误认为“已生效”。

## 1. 分层约定

| 层 | 代码位置 | 是否接触 MTL/硬件 |
|---|---|---|
| 本机前端 | `app/dvs_agent/web/src/pages/node-admin/` | 否，纯浏览器 |
| 节点代理控制面 | `app/dvs_agent/main.cpp`、`node_store.cpp`、`media_ipc.cpp`、`maintenance.cpp`、`registration.cpp` | 否 |
| 节点媒体引擎 | `app/dvs_media`（IPC + MTL RX + MPP/DRM/ALSA） | 是 |
| rk3588 数据面参考实现 | `app/rk3588_live_tx`、`app/rk3588_playout_rx` | 是 |

说明：`dvs_media` 当前只复用 file_playout 的 RX 管线（`mtl_rx_mp4.c`、`mtl_rx_audio.c`、`rx_compositor.c` 等），不复用 TX；因此前端“输入盒/采集口”相关功能在 rk3588 侧只能对应到 `rk3588_live_tx` 的采集链，尚未接入控制面。`dvs_media` 不提供采集或发送能力。

## 2. 前端区域 → 后端映射总表

| 前端区域/功能 | dvs_agent 后端 | dvs_media（dvs app） | rk3588 app | 当前状态 |
|---|---|---|---|---|
| 登录 `/node/login`、改密 `/node/password` | `main.cpp` 登录路由、`node_store` PBKDF2 密码校验、登录限流 5 次/60 秒、随机 session token 与过期 | 无 | 无 | 已实现登录状态机；严格鉴权默认关闭，可通过 `DVS_REQUIRE_AUTH=1` 开启 |
| 节点角色（输出盒/输入盒） | `node_store` role/pendingRole；scope `role` 状态机；`/api/v1/node/config/role` | IPC `configure` scope `role` → `dvs_engine_set_role`（input 清空 RX 合成器，output 保持空闲等待布局）；实际激活点在重启提升时由维护重启路径触发媒体 `clear` | `rk3588_live_tx` 有 tx/rx/both 运行模式，但未被代理驱动 | 状态机与 pendingRole 已实现；保存路径有意不立即转发媒体，重启提升时才激活，避免会话中途被清空；前端显示 pendingRole 待重启 |
| 输入口选择 | scope `input`；保存时把输入配置转发为 `whitelist`；输入口按节点 capabilities 校验 | `session_admit` 白名单（`dvs_session_whitelist_set`）；`/configure` hook `apply_whitelist`；`dvs_layout_prepare_from_json` | `rk3588_live_tx`：`v4l2_capturer` → `rga_converter` → `mpp_encoder` → MTL TX | 前端保存 → 代理 → IPC → 白名单链路已接通；会话准入与 LRU 淘汰已生效；采集/TX 仍未被控制面编排 |
| 输出设置（端口/分辨率/刷新率/强推/热插拔） | scope `output`；`/api/v1/node/config/output`；端口与范围按 capabilities 校验；保存后保持 pending，后台线程按引擎 `configure` 结果翻转 applied/failed（带 revision 守卫，旧保存的结果不覆盖新保存） | `dvs_engine_set_output` 更新配置并重建合成器；`file_playout_apply_drm_mode`、`file_playout_mode_dims`；`rx_compositor` DRM plane | `rk3588_playout_rx`：`shared/drm_renderer` 多 plane、`shared/display_layout`；`rk3588_live_tx` DRM 渲染 | 配置落库并 IPC 转发；引擎配置与合成器重建已执行；HTTP 立即返回 pending，前端轮询到 applied/failed 为准 |
| 网络配置（DHCP/静态） | scope `network`；校验、快照、回滚与落库；`apply_system_network` 默认只做意图持久化，系统网络重配需显式开启 | 无 | 无 | 已实现校验、快照与回滚；默认意图落库，`applied` 表示意图已持久化，不代表系统网络已切换；真实系统应用为延期项 |
| 注册方式/设备编号/立即注册 | scope `registration`；注册线程与心跳；`/api/v1/node/register` | 无（媒体不参与注册） | 无 | 已实现：配置平台 URL 时进行 HTTP 注册与周期心跳；空 URL 为本地自治模式 |
| SIP 接入 | scope `sip`；参数校验与 TCP 可达性探测（连通性探测，不做完整 SIP 注册会话） | 无 | 无 | V1.0 只做连通性探测；启用且不可达时明确失败，不影响媒体；完整 SIP 会话为延期项 |
| 播放任务投影（当前播放/任务列表/记录） | `/api/v1/node/media/status` 代理 `dvs_media`；心跳上报媒体摘要 | `query_state`：`rxSessions/decoding/layers/ptp`；`apply_layout` / `rx_compositor` | `rk3588_playout_rx`：`playout_buffer`、`av_sync`、`rx_compositor` 多 lane | 代理可读状态；任务操作被前端禁用（提示去平台） |
| 本机信息 | `/api/v1/node/info`、`/node/capability`、`/node/capacity` | `capability.c`：DRM connector/mode、max_decode_sessions、max_layers、PTP、MTL 版本 | `rk3588_playout_rx` 共享 `display_layout`（lane 上限 8）、MPP 解码；`live_ingest` DRM | 能力探测已实现；节点启动与请求时合并引擎能力；引擎不可达时保留种子，引擎报告零端口时如实传播零端口 |
| 维护操作（重启/恢复出厂/升级） | `/api/v1/node/reboot`、`/factory-reset`、`/upgrade`、`/upgrade/upload` | 无 | 无（系统级动作） | 重启提升 pendingRole，输入角色时触发媒体 `clear`；实际重启与升级切换默认关闭以保护开发机，响应体如实标注是否受控；恢复出厂保留网络与注册并清空媒体 |
| 硬件能力基线（`hardware-capabilities.js`） | `/api/v1/node/capability` 返回节点 capabilities | `dvs_capability_probe` / PTP 与 MTL 版本刷新 | `rk3588_playout_rx` `shared/display_layout.h`（8 lane 上限、NV12 直通 4 路） | 端口列表只来自节点上报；无上报或零端口时前端明确显示未上报并禁用端口操作；profile 只作为规格说明，不再作为端口来源 |

## 3. 关键链路说明

### 3.1 输入口 → 白名单 → 会话准入

1. 前端 `zone-input` 保存输入口配置。
2. 代理 `set_desired("input", value)`，更新 `configStates.input` 与 revision。
3. 代理 IPC 转发 `configure { whitelist: value }` 给 `dvs_media`。
4. `dvs_media` `configure` hook 调用 `apply_whitelist`，最终调用 `dvs_session_mgr_set_whitelist`。
5. `dvs_engine_apply_layout` 在创建 compositor 前调用 `dvs_session_admit`，拒绝未白名单源或 LRU 淘汰。

rk3588 侧对应功能：`rk3588_live_tx` 的 V4L2/MPP/RGA 采集与 MTL TX 发流；`rk3588_playout_rx` 的 MP4 demux/TX 音视频发送。它们提供“信号源”本身，但当前没有被代理动态控制。

### 3.2 输出设置 → DRM mode

1. 前端 `zone-output` 保存分辨率/刷新率/输出口/模式。
2. 代理 scope `output` 落库并 IPC 转发 `configure { scope: "output", value }`。
3. `dvs_media` `configure` hook 调用 `dvs_engine_set_output`，更新连接器、分辨率、刷新率与 DRM mode。
4. 当已有 compositor 且存在流时，引擎重建 compositor；`file_playout_apply_drm_mode` 与 `file_playout_mode_dims` 参与 mode 计算。

rk3588 侧对应功能：`rk3588_playout_rx` 的 DRM 多 plane 渲染与 `display_layout` 布局；`rk3588_live_tx` 的 DRM 渲染。当前 dvs_media 已复用这些代码。

### 3.3 播放任务投影

1. 前端 `zone-playback` 只投影当前播放，提示完整任务列表在平台。
2. 代理 `/api/v1/node/media/status` 调用 `media_ipc.query_state()`。
3. `dvs_media` 返回 `query_state` envelope，字段含 `rxSessions/decoding/layers/ptp` 与 lanes。
4. rk3588 侧对应功能：`rk3588_playout_rx` 的 `playout_buffer`、`av_sync`、`rx_compositor` 统计；`rk3588_live_tx` 的 RX bridge/解码/DRM。

### 3.4 能力上报

1. 前端 `hardware-capabilities.js` 的 `resolve(node)` 只使用节点 `capabilities` 的端口；无上报或空数组时显示能力未上报，不生成伪端口。
2. 代理 `/api/v1/node/capability` 返回节点 capabilities，并在启动与请求时合并引擎探测结果；引擎不可达时保留已有值，引擎明确报告零端口时如实传播零端口。
3. `dvs_media` `capability.c` 负责探测 DRM connector/mode、V4L2 输入设备、MPP 解码路数、plane 数；PTP 与 MTL 版本在每次 capability 请求时按引擎实时状态刷新。

rk3588 侧对应功能：`rk3588_playout_rx` 的 lane 上限与 NV12 直通路数；`rk3588_live_tx` 的 MPP 解码/DRM。当前探测多为默认值或环境变量覆盖，前端仍以硬编码 profile 作为无上报数据时的基线。

## 4. 缺口与结论（修正后）

- 节点角色保存有意不立即转发媒体，重启提升时才激活；输入角色在重启路径触发引擎清空 RX 合成器。这是延迟激活语义，不是缺口。
- 输出配置 HTTP 立即返回 pending，后台按引擎结果翻转 applied/failed；前端以轮询到的最终状态为准，不把提交当生效。
- 输入口本轮只做到白名单意图：端口按能力校验，白名单逐项校验，引擎拒绝时配置状态置 failed；采集、编码与 MTL 发送仍未被控制面编排，rk3588 采集链仅作为参考实现，TX 编排延期。
- 网络配置默认只做意图持久化，`applied` 表示意图已落库，不代表系统网络已切换；真实系统应用延期，不在本轮引入 netplan/systemd/iproute2 执行。
- 维护操作默认不执行高副作用系统动作以保护开发机，响应体如实标注是否受控；生产通过显式环境开启。
- SIP 本轮只做 TCP 可达性探测，不做完整会话；启用且不可达时明确失败，不影响媒体。
- 能力探测输入口来自 `/dev/video*` 枚举，输出口来自 DRM connector；前端不再用 profile 伪造端口，无上报或零端口时明确显示并禁用端口操作。
