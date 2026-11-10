# dvs_media 功能清单

> 分析对象：`app/dvs_media/`（节点侧媒体引擎，纯 C）。更新时间：2026-11-10，基于 `feat/rk3588-apps` 分支现状。

## 1. 定位

`dvs_media` 是 DVS 节点侧的**受控媒体引擎**，由 `rk3588_playout_rx` 的 RX 侧演进，并接入 `rk3588_live_tx` 的 TX 链路：

- **output 角色（RX）**：接收 MTL ST 2110 流（视频 + 音频），MPP 解码 → 布局合成（`rx_compositor`）→ DRM 显示 + ALSA 出声。
- **input 角色（TX）**：V4L2 捕获 → RGA 转换 → MPP 编码 → MTL ST 2110-22（ST22/H.264/H.265）发送，音频可选 ST 2110-30 发送。

与 file_playout 的核心区别是**受控运行**：进程空闲启动，由 `dvs_agent` 经 Unix socket JSON 行协议 IPC 驱动 `hello / configure / set_sources / prepare / commit / abort / clear / query_state / capability / tx_start / tx_stop / event`，支持布局两阶段提交、热更新、能力上报。与 `dvs_agent` 同节点包发布（`deploy/dvs-node/`，版本契约见 `dvs_common/include/dvs_node_bundle.h`，当前 bundle `1`、IPC `1`）。

## 2. 运行模式

```bash
dvs_media --config <static.json>              # 独立模式：静态配置直接运行
dvs_media --config <cfg> --ipc-sock <path>    # 受控模式：空闲启动，IPC 驱动
dvs_media --version                            # 输出 bundle + IPC 版本后退出
```

`dvs_media_main.c` 流程：

1. `--version` 直接打印版本后退出；
2. 启动即 `dvs_capability_probe` 一次性探测节点能力（best-effort，永不 abort，失败只记日志）并打印 JSON；
3. 加载基础配置（网络/DRM/MTL + 可选初始布局），`dvs_session_mgr_create` 创建引擎；
4. 受控模式：有初始布局则 `dvs_engine_start`，否则仅 `dvs_engine_init_mtl` 预热 MTL（加快首次 commit），随后 `dvs_ipc_attach_engine` + `dvs_ipc_server_run` 阻塞服务；
5. 独立模式：要求配置中有 `pip_streams`，应用布局后运行至 SIGINT/SIGTERM。

## 3. 目录结构

| 目录/文件 | 职责 |
|---|---|
| `dvs_media_main.c` | 入口：能力探测、配置加载、引擎创建，选择独立/受控模式 |
| `engine/engine_ctx.c` | 引擎上下文（`dvs_engine`）：持有 MTL 实例与当前 compositor；input/output 角色互斥；布局应用（几何变更走快路径复用 compositor，其余走影子交换重建）、帧锚定提交、commit 报告；TX 编排入口（TX 启动前先停 RX 合成器） |
| `engine/session_mgr.c` | 会话管理薄封装：创建/销毁引擎、应用布局、请求停止、白名单设置 |
| `engine/session_admit.c` | 会话准入：`(mcast_ip, udp_port)` → lane 映射，白名单 + 8 槽 LRU 表；`dvs_session_reserve_batch` 提供 prepare 阶段的全有全无批量准入；abort 释放本次预留；无 MTL/MPP 依赖，x86 可单测 |
| `engine/layout_executor.c` | 两阶段布局执行：prepare 把布局暂存到 shadow（含帧锚、底图、画面调节、音频路由、applyAtNs），commit 时统一应用到引擎；未暂存的 commit 保持当前画面 |
| `engine/audio_arbiter.c` | 音频仲裁与落子：`audioOwner` → lane 音频订阅 + ALSA Master 音量；接受 `lane<N>`、纯十进制索引、`sip:port`、精确 `sip`，空串为静音；解析失败或无音频口时强制静音并记 `applied_ok=0` |
| `engine/audio_owner.c` | 音频 owner 字符串纯分类（mute/lane/index/endpoint/sip/invalid）与 lane 索引提取，无硬件依赖，x86 可单测 |
| `engine/base_image.c` | 底图（墙纸）：节点侧解码本地文件 → 裁剪子矩形 → 拉伸为 XRGB8888 画布 → 推送给合成器背景层；FFmpeg 软解 PNG/JPEG，支持 RGB24/RGBA/BGR24/GRAY8/YUV420P（BT.601），其余报 unsupported；状态机 `none/loading/ready/failed/unsupported`，compositor 重建后重推缓存像素 |
| `engine/capability.c` | 能力探测与上报：DRM 连接器/模式、V4L2 输入口（端口→设备路径→物理身份→信号）、MPP 解码会话上限、DRM 层数、TX 静态上限、PTP 状态/偏移、MTL 版本与 backend；探测永不 abort，允许部分能力继续服务 IPC |
| `engine/stream_validate.c` | 流参数纯校验（身份尺寸、源裁剪、SSM 源地址），prepare 阶段 fail-fast，与 commit 侧 authoritative 校验保持一致 |
| `engine/tx_session.c` | TX 参数校验 + 生命周期状态机：`idle → configured → running / failed`；参数范围（640–3840×480–2160、24–60fps、500–20000kbps、GOP 0 或 250–4000ms）；**诚实规则**：出货二进制恒装 backend，成功即 `running`；无 backend（`configured + backend_pending`）只存在于 x86 单测接缝，绝不声称发流 |
| `engine/tx_port_map.c` | 捕获端口映射：物理身份优先（QUERYCAP card+bus_info+input 索引）绑定 HDMIk；TX 启动时重扫解析，目标缺失/无信号则拒绝并附可用表，绝不静默改绑；另含 codec 名→编码格式、kbps→bps、GOP 默认/换算等纯函数 |
| `engine/tx_backend.h` + `tx_backend_live_ingest.c` | TX 数据面：链接 `rk3588_live_tx` 的 donor 链（V4L2 capture → RGA → MPP encode → MTL TX + 可选 ST2110-30 音频采集发送）。MPP/RGA 均为构建必需，常链接；`tx_session` 的无 backend 分支仅为 x86 单测接缝 |
| `engine/ipc_engine_bridge.c` | IPC → 引擎桥接：把引擎函数装进 `dvs_ipc_engine_hooks_t` 钩子表；解析 `configure` 的 scope（whitelist/output/role/st21_pacing）与 `tx_start` 参数；prepare 做白名单+形状+资源预留三重校验；真实二进制专属，不进无 MTL 单测 |
| `ipc/ipc_server.c` | Unix socket JSON 行协议服务：长连接（最多 8 客户端）、messageId 回显与请求幂等重放缓存、`hello` 握手、异步 prepare + `media.prepare.result` 事件、commit/clear 串行执行器、commit 广播；钩子表未安装时可纯状态机运行（无 MTL/MPP 单测） |
| `ipc/state_machine.c` | 操作状态机：`IDLE → PREPARING → PREPARED`（commit 后回 IDLE，revision 递增）；错误码 `INVALID_ARG / STALE_REVISION / INVALID_STATE / INTERNAL` |
| `tests/` | 8 个 gtest 目标（见 §11），核心逻辑模块刻意不依赖 MTL/MPP，x86 可直接单测 |

## 4. 对外接口（IPC 动词）

行协议：每行一个 JSON，顶层 `type` + `requestId`；业务字段兼容 `payload` 嵌套与顶层两种写法（`payload` 为对象时优先取 `payload`）。响应为统一 envelope（`ok/status/code/msg/data/requestId`）。

| 动词 | 行为 |
|---|---|
| `hello` | 握手：上报 `ipcVersion`、包版本、当前状态快照（供代理核对同包配对） |
| `configure` | 统一配置入口，载荷决定语义：`whitelist`（字符串/数组/对象元组三种写法，逐项校验）分支走白名单设置；`scope/output` 设置 DRM 输出（连接器名、宽高、刷新率、`force/hotplug` 模式，越界拒绝）；`scope/role` 切换 `input/output`；`scope/st21_pacing` 设置 TX pacing（见 §10）；其余载荷转布局 staging |
| `set_sources` | `configure` 的布局别名：同一份 prepare 校验与暂存路径 |
| `prepare` | 异步两阶段第一步：`operationId` 必填，`revision` 缺省为 `revision+1`；先做布局校验与资源预留（见 §5），再经状态机进入 `PREPARING`，立即回 `202 ACCEPTED`，后台 worker 完成后发 `media.prepare.result` 事件（`ready/blocked` + lanes 通道摘要）；同 `operationId+revision` 重放直接回 `202` 不重复派 worker |
| `commit` | 串行执行器上的重动词：先应用暂存布局，成功后再推进状态机；响应合并 commit 报告（`frameIndex/appliedAtTaiNs/late/revision`）；已提交操作的重放直接回 `200 committed` 不重触引擎；引擎失败透出短码（`DECODE_NOT_READY` → 422，其余 500），状态机不推进以便重试/abort；成功广播 `media.layout_applied` + `media.state_changed` |
| `abort` | 丢弃暂存（释放本次 prepare 预留的 admission），操作回到 `IDLE` 并广播状态 |
| `clear` | 重动词：经串行执行器把媒体清到 backdrop-only 空布局（0 lane 合成器保持扫出，MTL 不下电），清空暂存并广播状态 |
| `query_state` | 合并视图：`state/revision/operationId/opState` + `lanes`（每 lane 含 `lane/sip/udp_port/ready/decoding/lostFrames/lastPtsNs`，无 admission 时为空数组）+ `audio`（见 §7）+ `display`（见 §8）+ `rxSessions/decoding/layers` 计数 + `ptp`（live 刷新）+ `tx`（见 §9）；缺省 hook 时退化为空集合，不缓存 stale 值 |
| `capability` | 上报前先 live 刷新 PTP 与 MTL 版本/backend（无 hook 的单测保持探测值），载荷见 §6 |
| `tx_start` | input 角色专属：要求 `port + encode{codec,width,height,fps,bitrateKbps,gopMs} + target{sip,udpPort,audioUdpPort}` 完整；响应直接带 TX 会话快照；`tx_stop` 幂等清参回 `idle` |
| `tx_stop` | 幂等停止 TX（数据面 + 状态机），总回 `200` |
| `event` | 预留回显动词，总回 `200` |

未知 `type`、坏 JSON 缺 `type` 均回 `400 INVALID_ARGUMENT`。

## 5. 两阶段提交与状态机

- `prepare`：校验并**暂存**布局（shadow），不触碰运行中的媒体；白名单检查→形状检查（身份/尺寸/裁剪/源地址）→`dvs_session_reserve_batch` 全有全无预留（超 8 槽或命中拒绝即整个 prepare 400 失败，不污染 LRU）；超 lane 上限（8）的布局直接 400 拒绝，不静默截断；`windows` 旧形状不再静默接受，直接 400 要求 agent 先转译为 `streams`；成功后进入 `PREPARED` 并记录 `prepared_revision`。
- `commit`：把暂存布局经 `dvs_engine_apply_layout_frame` 一次性应用；revision 推进，`dvs_state_commit` 拒绝过期 revision（`STALE_REVISION` → 409）与非法状态转移（`INVALID_STATE` → 409）；失败短码见 §9 末尾。
- `abort` 丢弃暂存并释放本次预留，`clear` 把运行媒体与暂存一起清到 backdrop 空布局。`query_state` 汇报 revision、operation_id、op_state 与快照。
- 幂等：`prepare` 同 `operationId+revision` 在非 IDLE 重放回 `202`；`commit` 在已提交 revision 重放回 `200`；每个连接保留 32 条 `requestId` 应答缓存，掉线重发不重放副作用。

## 6. 能力上报（`capability` / `query_state` 共享结构）

- DRM 输出口：`/dev/dri/card0` 连接器枚举，未连接且无模式的跳过；命名 `HDMI-A-<type_id>`，每口上报最大宽高/刷新率与模式数；`max_layers` 取 plane 总数。
- V4L2 输入口：与 TX 后端共用同一套扫描表（QUERYCAP 物理身份优先，设备路径兜底），按扫描顺序绑定 `HDMIk`；每口上报 `id/label/kind` + 当前设备路径 + 稳定物理身份 + 信号有无 + 3840×2160@60 上限；无设备时为空数组，不报错。
- 解码上限：静态上限 = min（合成 lane 上限 8，NV12 路径 4，DRM plane 数，MPP 上限默认 4）；`DVS_MAX_DECODE_SESSIONS` 环境变量优先覆盖；可用数 = 静态上限 − 已准入数（钳制 ≥0）。
- TX 上限：SoC 公开极限（`h264,h265`、3840×2160@60、20000kbps），捕获口复用 `inputPorts`，不另行探测。
- PTP：`synced/unsynced/unknown` + 偏移 µs；monotonic 时钟模式恒报 `unknown`；`capability` 快照取探测值，`query_state` 走 live hook（MTL TAI 与本地 TAI 差换算，MTL 未就绪则 unknown）。
- MTL：版本号 live 刷新；backend 按端口名前缀映射（`kernel:`→kernel、`native_af_xdp/dpdk_af_xdp/dpdk_af_packet`→af_xdp、其余→dpdk），双端口前缀不一致时报 `unknown`。
- 探测永不 abort，允许部分能力继续服务 IPC；`dvs_capability_to_json` / `dvs_capability_state_json` 为纯序列化，`max_decode_sessions` 字段报静态上限、`availableDecodeSessions` 报可用数。

## 7. 音频路由

- Owner 写法：`lane<N>`、纯十进制 lane 索引、`sip:port`（按视频 `sip:udp_port` 精确匹配）、精确 `sip`（首个同 sip lane）、空串为静音；其余一律 invalid，不回退到其他 lane。
- 订阅要求目标 lane 携带非零 `audioUdpPort`，否则视为无音频口；失败路径强制静音（关订阅 + Master 音量置 0），query 记 `applied_ok=0` 以示未生效。
- 音量 0–100 钳制后映射到 ALSA Master 播放音量；无 Master 控件的设备只做软静音，不报错。
- **诚实查询**：`query_state.audio` 永远返回实际生效状态（`owner/sip/audioPort/volume/muted/subscribed/applied`），pending 请求不回显；`subscribed` 取合成器音频运行态。

## 8. 底图（baseImage）与画面调节（imageAdjust）

- 载荷随 prepare 同行：`baseImage{revision,url,path,width,height,sub{x,y,w,h}}` + `imageAdjust{brightness,contrast,saturation,hue}`；`revision==0` 为无 still。
- 拉取责任在 agent（下载 `url` 到本地文件后把 `path` 交给引擎）；引擎只解 `path`，`url` 不可 fetch 时报 `still path missing, fetch pending` 而不猜测。
- 解码→裁剪（源像素 sub，钳制）→拉伸到当前输出模式画布（缺省 1920×1080）的 XRGB8888；compositor 每次重建后重推缓存像素（渲染器是新的）。
- 状态 `none/loading/ready/failed/unsupported` + `revision/error` 一并进 `query_state.display`；显示后端为 dumb-buffer 背景路径，调节值只记录不应用，`supported=false` 诚实上报为 unsupported，永不静默丢弃。

## 9. TX（input 角色）

- 参数门：`port`（`HDMIk`/`/dev/videoN`/稳定身份三选一）+ `codec(h264/h265)` + 宽高/fps/码率 + `target{sip,udpPort}` 必填，`gopMs` 缺省走编码器默认（跟 fps），`audioUdpPort` 为 0 表示无音频 TX；范围见 `tx_session.h`，先范围校验再按上报 capability  gating。
- 端口解析：启动绑定表按物理身份优先，TX 启动时重扫；目标缺失/无信号直接拒绝并附可用表（`HDMI1(/dev/video0), …`），绝不静默改绑。
- 数据面：`tx_backend_live_ingest` 持有 MTL TX 会话 + TX pipeline；视频链为 V4L2→RGA→MPP→MTL ST22；音频链为 ALSA 采集（48kHz 双声道 PCM16，1ms 包/10ms 帧）→ ST2110-30，音频 bring-up 失败则整个 TX start  loud 失败，不发无声视频。所有进入/退出都在引擎锁下，`tx_stop`/角色切换/clear/stop 全路径对称 teardown。
- **诚实规则**：`tx_session` 的无 backend（`configured + backend_pending`）分支只活在 x86 单测里；出货二进制恒装 live_ingest backend，`tx_start` 成功即 `running`，失败即 `failed`，不存在第三种出货形态；`tx_stop` 幂等清参回 `idle`。
- `query_state.tx` 为 `dvs_tx_status_json` 快照（`state/params/error/backend_pending`）；commit 失败短码：`DECODE_NOT_READY`（影子无首帧，旧画面保持）/ `LAYOUT_APPLY_FAILED` / `MEDIA_SESSION_FAILED` / `LAYOUT_CONFLICT`（input 角色下推布局）。

## 10. 帧锚定提交、快慢路径与角色互斥

- 布局可携带帧锚 `(epochTaiNs, fps, frameIndex)` 与目标 TAI 时刻 `applyAtNs`：`epoch==0` 或 `frameIndex<0` = 立即（当前帧）提交；锚定提交换算到帧起始时刻，所有节点同一瞬间换布局；commit 报告（`dvs_engine_commit_report`）回报实际提交的帧号、提交时刻（TAI ns）、是否迟到（`late`），配合 `dvs_platform` 的跨节点 prepare/commit 屏障实现多节点同时切换。
- **快路径**：纯几何变更（窗口位置/大小调整）复用运行中的 compositor，未来目标在快路径上等待 TAI 时钟到达后换布局。
- **慢路径**：流增减/编码变化走影子交换（build + start + 等首帧 + swap + 释旧）；影子失败（build/start/50×100ms 等首帧超时）保持旧画面并分别报 `MEDIA_SESSION_FAILED` / `DECODE_NOT_READY`；空布局（count 0）切到 0-lane backdrop 合成器保持扫出，MTL 不下电。
- **角色互斥**：`dvs_engine_set_role` 切到 input 即停 RX 合成器并释放 admission，切到 output 即停 TX 会话；同角色重复切换为 no-op，不打断运行链；TX 只能在 input 角色启动（否则 `TX_ROLE_MISMATCH`），布局只能在 output 角色合成（否则 `LAYOUT_CONFLICT`）；输出模式变更有 compositor 时经帧路径重建即时生效，否则等下次布局。
- `dvs_engine_media_idle` 无泄漏断言：`pip_stream_count==0` 且 TX 为 `idle` 才算空闲（backdrop 空合成器视为空闲显示）。

## 11. 白名单与流校验

- 白名单项为 `ip`（该 host 任意端口）或 `ip:port`（精确），逗号串/数组/ `{sip,udp_port?}` 对象元组三种写法统一归一后逐项校验（IPv4 点分十进制 + 端口 1–65535），空/非法整体 400 拒绝；`NULL/""` 为关闭（allow all）。
- 准入表 8 槽 LRU：`dvs_session_tick` 每布局周期推时钟，命中刷新、满表驱逐最久未用；`reserve_batch` 先全量过白名单再 admit，任一拒绝则全表不动。
- 流形状校验（prepare fail-fast）：每 lane 必带 `sip/udpPort/width/height/fps` 合法值；源裁剪 `srcX/srcY/srcW/srcH` 零宽高即全帧、越界拒绝；`srcIp` 为空即任意源组播，否则必须为 IPv4 字面量。

## 12. IPC 健壮性与安全

- 串行执行器：commit/clear 同一时刻只跑一个，第二重并发立即 `503 BUSY` 可重试，等待上限 15s 超时报 500；worker 起不来时调用方线程内联执行，语义不变。
- 异步 prepare：worker 模拟首帧/plane 分配等待（默认 50ms，单测可置 0），`media.prepare.result` 事件带通道摘要回发起连接，`media.layout_applied` / `media.state_changed` 广播全连接。
- 安全：socket 文件 `0600` 属主-only；`accept` 后 `SO_PEERCRED` 核对 uid，非同用户直接拒连；超 8 客户端时内联回 `503 BUSY` 不占槽。

## 13. ST 2110-21 pacing profile

TX pacing profile 复用 file_playout 的解析（in-tree，不复制）：配置文件字段 `st21_pacing`（`auto|tsc|tsc_narrow|rl`），默认 `tsc_narrow`（ST 2110-21 Narrow，广播互通首选）；IPC `configure` 新增 `scope: "st21_pacing"`，`value.st21_pacing` 为同取值字符串；MTL 已初始化时拒绝（需 clear 后再配，下次 MTL init 生效）。`tsc_narrow` 不受 ST22 会话 RL→TSC 强制覆盖影响；VRX 余量校验用 `../rk3588_playout_rx/tests/verify_vrx_margin.py`。

## 14. in-tree 源码复用（非拷贝）

`meson.build` 以相对路径直接编译 `rk3588_playout_rx` 的 `rx/`、`shared/`（含 `mp4_demux`、`extradata_handler`）、`rk3588_common`，以及 `rk3588_live_tx` 的 `hal/`、`io/v4l2_capturer`、`codec/` 编码侧、`pipeline/tx_pipeline`（含 `tx_encode/tx_supervise/tx_stats`）、`mtl_tx`。**修改这两个应用的这些目录会直接影响 dvs_media 的构建与行为。**

## 15. 构建与测试

```bash
cd app/dvs_media && meson setup build && ninja -C build
./build/dvs_media --version
```

依赖：MTL、Rockchip MPP（**必需**，缺失则 meson 直接 error）、librga（**必需**，缺失直接构建失败，不再产出 backend-less 二进制）、libdrm、ALSA、json-c、FFmpeg（libavformat/avcodec/avutil）。单元测试为独立 meson 目标（`tests/meson.build`），8 个可执行文件，MTL/MPP 无关模块 x86 可单测：

| 目标 | 覆盖 |
|---|---|
| `test_state_machine` | 状态机转移、revision/幂等、错误码 |
| `test_ipc` | IPC 行协议、重放缓存、prepare/commit/clear/tx/query/capability |
| `test_session_admit` | 白名单、LRU 准入、批量预留全有全无 |
| `test_tx_session` | TX 参数范围、状态机、backend 缺席诚实态 |
| `test_tx_port_map` | 端口表构建/解析、codec/码率/GOP、设备扫描退化 |
| `test_stream_validate` | 身份尺寸、裁剪、源地址校验 |
| `test_audio_owner` | owner 分类与 lane 索引提取 |
| `test_capability_logic` | 静态上限/可用数、backend 名、PTP 换算、JSON 序列化 |

## 16. 设计原则小结

1. **诚实状态**：query 永远反映实际生效状态（applied_*），pending 请求不回显；出货二进制恒装 TX backend。
2. **fail-fast 拒绝**：端口/白名单/参数/形状不满足即拒绝并附原因，绝不静默降级或改绑。
3. **可测性分层**：纯逻辑（准入、校验、状态机、仲裁、端口映射、能力数学）与硬件数据面（live_ingest backend）解耦，前者 x86 单测覆盖。
4. **受控生命周期**：一切媒体变更经 prepare/commit 屏障，支持跨节点帧对齐；错误以短码向上传递。
