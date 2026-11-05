# dvs_platform 平台前端功能与 dvs / rk3588 app 功能映射分析

> 范围：`app/dvs_platform/web/src/pages/platform/*.html` 平台后台功能，
> 映射到 `app/dvs_platform` 自身控制面、`app/dvs_agent` 节点代理、`app/dvs_media` 节点媒体引擎、以及 `app/rk3588_playout_rx` / `app/rk3588_live_tx` 数据面 app。
> 目标：区分“平台文档/编排”与“节点媒体执行”边界，明确哪些前端操作最终落到 MTL/MPP/DRM。

## 1. 分层约定

| 层 | 代码位置 | 职责 |
|---|---|---|
| 平台前端 | `app/dvs_platform/web/src/pages/platform/` | 分组/墙/设置/端口绑定/调度/设备/任务 |
| 平台服务 | `app/dvs_platform/main.cpp`、`platform_store.cpp`、`layout.cpp`、`ops.cpp` | 文档 CRUD、节点网关、布局拆分、两阶段提交、升级、墓碑 |
| 节点代理 | `app/dvs_agent` | 注册/心跳、本机 HTTP、配置状态机、IPC 驱动媒体引擎 |
| 节点媒体引擎 | `app/dvs_media` | MTL RX、白名单准入、布局 apply、DRM/ALSA、PTP |
| rk3588 数据面 | `rk3588_playout_rx`、`rk3588_live_tx` | MTL ST22/ST30 收发、MPP、DRM、ALSA、PTP |

## 2. 前端页面 → 后端映射总表

| 前端页面/功能 | dvs_platform 后端 | dvs_agent / dvs_media | rk3588 app | 当前状态 |
|---|---|---|---|---|
| 分组管理 `group-list` | `platform_store` docs kind `groups` CRUD | 无 | 无 | 已实现（文档存储） |
| 拼墙列表 `wall-list` | docs kind `walls` CRUD；墓碑/软删除 | 无 | 无 | 已实现（文档存储） |
| 基本设置 `wall-basic` | docs kind `walls` 字段更新；`config-push` geometry/role | 代理 `/api/v1/node/config/sync` 应用 geometry/role；输出 mode 未执行 | `file_playout` DRM mode/分辨率；`display_layout` | 平台落库 + 部分推送；媒体 mode 未执行 |
| 端口绑定 `port-bind` | 节点 doc `geometry`；绑定关系文档 | 代理节点 doc；`dvs_media` 能力/geometry | `file_playout` DRM connector/plane；`live_ingest` DRM | 平台记录绑定；节点 geometry 驱动布局拆分 |
| 图像调节 `wall-image` | docs kind `walls` settings.image | 无执行（未推送） | `file_playout` 无图像调节控制；MPP/DRM 无对应 | 仅文档/前端模拟 |
| 传输配置 `wall-transfer` | docs kind `whitelist` | 代理 scope `input` → IPC `configure whitelist` → `session_admit` | `file_playout` 无动态白名单；`live_ingest` TX 源 | 白名单链路部分接通 |
| 高清底图 `wall-backdrop` | docs kind `walls` backdrop | 无下发 | 无底图 plane 支持 | 前端/文档层，媒体未实现 |
| 画面调度 `screen-schedule` | `ops/prepare`、`ops/commit`、`layout.cpp` fragment_for_node | 代理 `/api/v1/node/media/layout` → `dvs_media` prepare/commit | `rx_compositor_try_fast_apply`、`display_layout`、DRM plane | 核心链路已搭，apply_at/影子布局部分实现 |
| 设备列表 `device-list` | docs kind `node` CRUD、rename、retire tombstone、upgrade | 代理 retire/upgrade；媒体无 | 无 | 平台编排 + 代理假回执 |
| 播放任务板 | docs kind `playback-tasks` | 代理媒体状态投影 | `file_playout` playout/av_sync | 文档层 + 状态投影 |
| 节点网关（注册/心跳） | `/api/v1/nodes/register`、`/heartbeat`、tombstone GC | 代理注册/心跳线程（占位） | 无 | 平台端已实现；代理端未真实外发 |

## 3. 详细映射

### 3.1 分组/墙/绑定文档（平台自有）

- 前端：`group-list.html`、`wall-list.html`。
- 后端：`platform_store.cpp` 文档 CRUD，kind `groups`/`walls`/`scenes`/`bindings`/`whitelist`/`playback-tasks`。
- 无 dvs_media/rk3588 对应；这些是平台控制面文档，不触达 MTL。

### 3.2 基本设置 / 端口绑定 / 几何

- 前端：`wall-basic.html`、`port-bind.html`。
- 平台：更新 wall doc；节点 doc 存 `agentBase`、`geometry`（x/y/w/h）。
- `config-push` 只推送 `geometry` 与 `role` 到代理；代理 `config/sync` 只应用 `geometry`/`role`，其他键忽略。
- `layout.cpp` `fragment_for_node` 按节点 geometry 把全局窗口拆成节点局部窗口（win/src 坐标）。
- dvs_media：布局 commit 时用拆分后的 windows 创建/更新 compositor lane。
- rk3588：`rk3588_playout_rx` 的 `display_layout_from_pip_streams` 与 `rx_compositor` lane/DRM plane；`rk3588_live_tx` 的 DRM 渲染。平台拆分结果最终应映射到这些 plane 坐标。

### 3.3 传输配置（白名单）

- 前端：`wall-transfer.html` 白名单表（单播/组播）。
- 平台：kind `whitelist` 文档；prepare 时随 wall/windows 下发？当前 prepare 主要下发 windows；白名单未在 prepare 链路中显式下发。
- 代理：scope `input` 保存时 IPC `configure { whitelist }`。
- dvs_media：`session_admit` 白名单 + LRU（`DVS_MAX_SESSIONS=8`）。
- rk3588：`rk3588_playout_rx` 无动态白名单控制；`rk3588_live_tx` MTL TX 源地址固定由配置提供。白名单执行最终发生在 dvs_media 的 RX 准入层。

### 3.4 画面调度（核心媒体链路）

- 前端：`screen-schedule.html` 信号源面板、画布、场景、清屏、未保存改动。
- 平台：`/api/v1/ops/prepare` 创建 Operation，计算 apply_at；后台线程按成员节点调用 `/api/v1/node/media/layout`，下发 fragment + geometry + applyAtNs。
- 代理：`/api/v1/node/media/layout` 转为 IPC `prepare`，并异步向平台回执 commit（ready/blocked）。
- dvs_media：IPC `prepare` 开始异步准备；`commit` 应用 pending layout；`rx_compositor_try_fast_apply` 尝试快速应用；失败 rollback。
- rk3588：`rk3588_playout_rx` `rx_compositor_create` 创建 lane 与 MTL RX session；`display_layout` 解析 lane 坐标；`mtl_rx_mp4` 接收 ST22；`mtl_rx_bridge`/`rx_pipeline` 解码送 DRM。`rk3588_live_tx` 提供 TX 侧码流来源（V4L2→MPP→MTL TX）。

### 3.5 设备管理

- 前端：`device-list.html` 筛选、别名、退役、批量升级。
- 平台：节点 doc rename/retire；`tombstone_add` 防止退役节点复活；串行 `/api/v1/nodes/upgrade`。
- 代理：retire 更新 lifecycle；upgrade 转发到 `/api/v1/node/upgrade`。
- dvs_media：无对应（媒体引擎不管理设备生命周期）。
- rk3588：无对应（系统/部署层）。

### 3.6 播放任务

- 前端：screen-schedule 底部任务板 + `playback-tasks` API。
- 平台：kind `playback-tasks` 文档；八态任务状态机。
- 代理：`/api/v1/node/media/status` 投影 `currentPlayback`。
- dvs_media：`query_state` 返回 rx sessions/decoding/layers/ptp。
- rk3588：`rk3588_playout_rx` 的 playout buffer/av_sync 提供播放连续性；`rk3588_live_tx` 提供实时源。

### 3.7 能力与 PTP

- 前端本机页 `hardware-capabilities.js` safeModes/customRange；平台设备列表展示节点信息。
- 平台：节点 doc capabilities/geometry。
- 代理：`/api/v1/node/capability`、`/node/capacity`。
- dvs_media：`capability.c` 探测 DRM connector/mode、max_decode_sessions、max_layers、PTP、MTL 版本。
- rk3588：`rk3588_playout_rx` lane 上限 8、NV12 直通 4 路；`shared/ptp_time.c` 读取 PTP；`rk3588_live_tx` PTP。当前 dvs_media PTP 状态在 hook 中固定为 `synced` 且 offset 0，前端“跨节点同帧：需控制服务与统一时钟联调”。

## 4. 缺口与结论

- 平台前端的图像调节、底图、底图拆分等能力在 dvs_media/rk3588 侧尚无执行器，当前仅平台文档/前端模拟。
- 白名单前端已能保存，但 prepare 链路未显式下发白名单；白名单主要通过代理 scope `input` 单独下发。
- 设备退役/升级在平台端有编排，但代理端 maintenance 为 stub，未执行真实动作。
- 画面调度链路已形成平台 prepare → 节点 layout → dvs_media prepare/commit → rx_compositor 的主路径，但 apply_at 等待、影子布局、rollback 仍部分实现。
- rk3588 app 提供 MTL 收发、MPP、DRM、ALSA、PTP 的底层能力；平台前端的最终媒体效果依赖 dvs_media 复用这些能力，平台本身不直接触达 MTL。
