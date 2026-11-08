# dvs_media 功能分析

> 分析对象：`app/dvs_media/`（节点媒体引擎，纯 C）。编写时间：2026-11-08，基于 `feat/rk3588-apps` 分支。

## 1. 定位

`dvs_media` 是 DVS 节点侧的**受控媒体引擎**，由 `rk3588_playout_rx` 的 RX 侧演进而来，并接入 `rk3588_live_tx` 的 TX 链路：

- **output 角色（RX）**：接收 MTL ST 2110 流（视频 + 音频），MPP 解码 → 布局合成（`rx_compositor`）→ DRM 显示 + ALSA 出声。
- **input 角色（TX）**：V4L2 捕获 → RGA 转换 → MPP 编码 → MTL ST 2110-22（ST22/H.264/H.265）发送。

与 file_playout 的核心区别是**受控运行**：进程空闲启动，由 `dvs_agent` 经 Unix socket IPC 驱动 `configure / prepare / commit / abort / clear / query_state`，支持布局热更新、两阶段提交、能力上报。与 `dvs_agent` 同节点包发布（`deploy/dvs-node/`，版本契约见 `dvs_common/include/dvs_node_bundle.h`）。

## 2. 运行模式

```bash
dvs_media --config <static.json>              # 独立模式：静态配置直接运行
dvs_media --config <cfg> --ipc-sock <path>    # 受控模式：空闲启动，IPC 驱动
```

`dvs_media_main.c`（入口）流程：

1. `--version` 输出 bundle 版本 + IPC 版本（须与 dvs_agent 同包配对）；
2. `dvs_capability_probe` 一次性探测节点能力（best-effort，永不 abort）；
3. 加载基础配置（网络/DRM/MTL + 可选初始布局），`dvs_session_mgr_create` 创建引擎；
4. 受控模式：有初始布局则 `dvs_engine_start`，否则仅 `dvs_engine_init_mtl` 预热（加快首次 commit），随后 `dvs_ipc_attach_engine` + `dvs_ipc_server_run` 阻塞服务；
5. 独立模式：要求配置中有 `pip_streams`，应用布局后运行至 SIGINT/SIGTERM。

## 3. 目录结构

| 目录/文件 | 职责 |
|---|---|
| `dvs_media_main.c` | 入口：能力探测、配置加载、引擎创建，选择独立/受控模式 |
| `engine/engine_ctx.c` | 引擎上下文（`dvs_engine`）：持有 MTL 实例与当前 compositor；input/output 角色互斥；布局应用（几何变更走快路径复用 compositor，其余重建）、帧锚定提交、commit 报告；TX 编排入口 |
| `engine/session_mgr.c` | 会话管理：从基础配置创建/销毁引擎（`dvs_session_mgr_create/destroy`） |
| `engine/session_admit.c` | 会话准入：(mcast_ip, udp_port) → lane 映射，白名单 + 8 槽 LRU 表；`dvs_session_reserve_batch` 提供 prepare 阶段的全有全无批量准入；无 MTL/MPP 依赖，x86 可单测 |
| `engine/layout_executor.c` | 两阶段布局执行：prepare 将布局暂存到 shadow（含帧锚 `dvs_frame_anchor_t`、底图 `dvs_base_image_desc_t`、画面调节 `dvs_image_adjust_t`），commit 时统一应用到引擎 |
| `engine/audio_arbiter.c` | 音频仲裁：`audioOwner`（窗口标识）→ lane 音频订阅 + 音量。接受 `lane<N>`、十进制索引、`sip:port`、精确 `sip`；空 = 静音。**诚实查询原则**：query 返回实际生效状态，从不回显 pending |
| `engine/audio_owner.c` | 音频 owner 的落地应用（lane 订阅切换、ALSA 音量） |
| `engine/base_image.c` | 底图（墙纸）：节点侧拉取 url 暂存文件 → 解码为 XRGB8888 画布 → 叠加到合成器；状态机 `none/loading/ready/failed/unsupported`，compositor 重建后重推缓存像素 |
| `engine/capability.c` | 能力探测与上报：DRM 连接器/模式、V4L2 输入口（设备路径 + 物理身份 + 信号）、MPP 解码会话上限、DRM 层数、TX 静态上限、PTP 状态/偏移、MTL 版本与 backend（dpdk/af_xdp/kernel） |
| `engine/stream_validate.c` | 流参数校验（布局中每路流的合法性） |
| `engine/tx_session.c` | TX 参数校验 + 生命周期状态机：`idle → configured → running / failed`。参数范围（640–3840×480–2160、24–60fps、500–20000kbps、GOP 0/250–4000ms）；**诚实规则**：无数据面 backend 时只报 `configured + backend_pending`，绝不声称发流 |
| `engine/tx_port_map.c` | 捕获端口映射：物理身份优先（QUERYCAP card+bus_info+input 索引）绑定 HDMIk；TX 启动时重扫解析，目标缺失/无信号则拒绝并附可用表，绝不静默改绑 |
| `engine/tx_backend.h` + `tx_backend_live_ingest.c` | TX 数据面：链接 `rk3588_live_tx` 的 donor 链（V4L2 capture → RGA → MPP encode → MTL TX）。缺 RGA/MPP 的构建不链接，TX 停留在 configured |
| `engine/ipc_engine_bridge.c` | IPC → 引擎桥接：把引擎函数装进 `dvs_ipc_engine_hooks_t` 钩子表 |
| `ipc/ipc_server.c` | Unix socket JSON 行协议服务：长连接、messageId 回显、`hello` 握手（协议/包版本 + 状态快照）、异步 prepare + `media.prepare.result` 事件、commit 广播；配置统一走单一 `configure` 谓词。钩子表未安装时可纯状态机运行（无 MTL/MPP 单测） |
| `ipc/state_machine.c` | 操作状态机：`IDLE → PREPARING → PREPARED`（commit 后回 IDLE，revision 递增）；错误码 INVALID_ARG / STALE_REVISION / INVALID_STATE / INTERNAL |
| `tests/` | 7 个 gtest 目标：`test_ipc`、`test_state_machine`、`test_session_admit`、`test_tx_session`、`test_tx_port_map`、`test_capability_logic`、`test_audio_owner`、`test_stream_validate` |

## 4. 关键机制

### 4.1 两阶段提交与状态机

- `prepare`：校验并**暂存**布局（shadow），不触碰运行中的媒体；成功进入 `PREPARED` 并记录 `prepared_revision`。
- `commit`：将暂存布局一次性应用；revision 推进，`dvs_state_commit` 拒绝过期 revision（STALE_REVISION）与非法状态转移。
- `abort` 丢弃暂存，`clear` 移除全部媒体。query_state 汇报 revision、operation_id、op_state 与快照。

### 4.2 帧锚定提交（跨节点屏障）

布局可携带帧锚 `(epoch_ns, fps, frame_index)` 与目标 TAI 时间 `apply_at_ns`：

- `epoch==0` 或 `frame_index<0` = 立即（当前帧）提交；
- 未来目标在快路径上等待 TAI 时钟到达后换布局；
- commit 报告（`dvs_engine_commit_report`）回报实际提交的帧号、提交时刻（TAI ns）、是否迟到（`late`），配合 `dvs_platform` 的跨节点 prepare/commit 屏障实现多节点同时切换；
- 失败时给出短码：`DECODE_NOT_READY` / `LAYOUT_APPLY_FAILED` / `MEDIA_SESSION_FAILED`。

### 4.3 角色互斥（input/output）

- `dvs_engine_set_role`：切到 output 停 TX 会话、input 停 RX 合成器；TX 只能在 input 角色启动（否则 `TX_ROLE_MISMATCH`），RX 只在 output 角色合成。
- `dvs_engine_media_idle` 是无泄漏断言：无 compositor、无 TX、无残留布局才算空闲，供回环与维护检查使用。

### 4.4 布局应用快慢路径

- **几何变更**（窗口位置/大小调整）：复用运行中的 compositor，不解码器重建；
- **其他变更**（流增减/编码变化）：stop + free + create + start 重建（正确但尚非帧边界原子）；
- 底图像素缓存在引擎，compositor 每次重建后重推（渲染器是新的）。

### 4.5 能力上报

`dvs_capability` 覆盖：DRM 输出口（连接器名、最大分辨率/刷新率、模式数）、V4L2 输入口（端口→当前设备路径→物理身份→信号）、`max_decode_sessions`（MPP，默认上限 4，可被 `DVS_MAX_DECODE_SESSIONS` 环境覆盖；静态上限 = min(合成 lane 上限, DRM plane 数, MPP 上限)）、`max_layers`、TX 上限（SoC 公开极限）、PTP（synced/unsynced/unknown + 偏移 µs，区分外部/内置/monotonic 时钟模式）、MTL 版本与 backend。探测永不 abort，允许部分能力继续服务 IPC。

### 4.6 in-tree 源码复用（非拷贝）

`meson.build` 以相对路径直接编译 `rk3588_playout_rx` 的 `rx/`、`shared/`（含 `mp4_demux`、`extradata_handler`）、`rk3588_common`，以及 `rk3588_live_tx` 的 `hal/`、`io/v4l2_capturer`、`codec/` 编码侧、`pipeline/tx_pipeline`、`mtl_tx`。**修改这两个应用的这些目录会直接影响 dvs_media 的构建与行为。**

## 5. 对外接口摘要（IPC）

| 谓词 | 行为 |
|---|---|
| `hello` | 握手：上报协议版本、包版本、状态快照（供代理核对同包配对） |
| `configure` | 配置下发统一入口（网络/DRM/输出模式/角色/白名单等） |
| `prepare` | 异步暂存布局/底图/调节/音频，完成后发 `media.prepare.result` 事件 |
| `commit` | 应用暂存布局；响应合并 commit 报告（帧号/时刻/late），并广播事件 |
| `abort` / `clear` | 丢弃暂存 / 移除全部媒体 |
| `query_state` | revision、能力、实时计数（rx_sessions/decoding/layers）、lanes 就绪统计、音频/显示/底图状态、TX 快照、PTP |
| TX verbs | input 角色：`tx_start`（参数完整才接受）/ `tx_stop`；无 backend 时报 `NOT_IMPLEMENTED` 或 idle |

`tx_start` 仅在角色为 input 且参数完整时接受；无 RGA 的构建中 TX 后端不链接，会话保持 `configured + pending`。

## 6. 构建与测试

```bash
cd app/dvs_media && meson setup build && ninja -C build
./build/dvs_media --version
```

依赖：MTL、Rockchip MPP（必需）、libdrm、ALSA、json-c、FFmpeg（libavformat/avcodec/avutil）；TX 数据面另需 RGA。单元测试为独立 meson 目标（`tests/meson.build`），核心逻辑模块（admit/tx_session/port_map/state_machine/capability/audio_owner/stream_validate）刻意不依赖 MTL/MPP，x86 可直接单测。

## 7. 设计原则小结

1. **诚实状态**：query 永远反映实际生效状态（applied_*），pending 请求不回显；无 backend 时 TX 只报 configured。
2. **fail-fast 拒绝**：端口/白名单/参数不满足即拒绝并附原因，绝不静默降级或改绑。
3. **可测性分层**：纯逻辑（准入、校验、状态机、仲裁）与硬件数据面（live_ingest backend）解耦，前者 x86 单测覆盖。
4. **受控生命周期**：一切媒体变更经 prepare/commit 屏障，支持跨节点帧对齐；错误以短码向上传递。
