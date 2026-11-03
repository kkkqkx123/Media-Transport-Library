# dvs_media / dvs_agent 功能现状分析与需求差距

> 基线:`feat/rk3588-apps`,HEAD `ecdf6c02`。对照 `docs/req/`(01 需求整理、03 需求到实现映射、04 端到端技术方案、06 数据与接口建议)与 `docs/plan/DVS_FRONTEND_INTEGRATION_PLAN.md`(S1/S2 阶段定义)。
> 结论先行:**dvs_media 与 dvs_agent 目前都是"骨架 + stub"状态**——IPC 消息集、配置状态机、HTTP 接口面等**结构层**已经按规划(S1/S2)搭好,但**功能层**(会话管理、布局执行、音频仲裁、能力探测、注册心跳、维护动作)绝大多数是空实现或假回执。

## 1. dvs_media(节点媒体引擎)现状

目录:`app/dvs_media/`,独立 meson 工程,链接 mp4_dist 的 RX 管线静态库(`dvs_rx`)与共享基础设施(`dvs_shared`)。

### 1.1 已实现

| 模块 | 状态 | 说明 |
| --- | --- | --- |
| `dvs_media_main.c` | ✅ 骨架完成 | 两种模式:`--config <json>` 加载 mp4_dist 配置后即退出(仅 smoke);`--ipc-sock <path>` 进入 IPC 服务;两者都未启动任何 MTL 会话 |
| `ipc/ipc_server.c` | ✅ 结构完成 | Unix domain socket + JSON 行协议,顺序 accept/单请求/单响应;支持 8 条消息:`configure / prepare / commit / abort / clear / set_sources / query_state / event`,响应为统一 envelope(`ok/status/code/msg/data/requestId`) |
| `ipc/state_machine.c` | ⚠️ 最小实现 | 仅维护一个全局 `(revision, state_name)`;commit 时 revision+1;**无 idle→prepared→committed 迁移校验、无旧 revision 拒绝、无持久化** |
| `engine/session_mgr.c` | ❌ stub | 空函数;MTL `st22p_rx_*`/`st30p_rx_*` 会话生命周期未实现 |
| `engine/layout_executor.c` | ❌ stub | 空函数;prepare 影子布局 / commit 帧边界原子切 plane 未实现 |
| `engine/audio_arbiter.c` | ❌ stub | 空函数;audioOwner/volume → ALSA 未实现 |
| `engine/capability.c` | ❌ stub | 空函数;能力探测(DRM mode/MPP 路数/DRM plane 数/PTP)未实现 |
| 复用层(meson 引入) | ✅ 可用 | 编译期复用 `mp4_dist/rx` 全套 RX 管线(组播收流、MPP 解码、playout buffer、av_sync、rx_compositor、alsa_audio)及 `mp4_dist_config.c`,但与 IPC/状态机**之间没有任何调用关系**——复用代码是"挂在链接线上",不是"接进引擎" |

### 1.2 功能判定

- IPC `prepare/commit` 只改状态机字符串,**不创建会话、不预解码、不切布局**;`query_state` 返回的 `rxSessions/decoding/layers/ptp` 全部是硬编码 0/`unknown`。
- req 04 §3 的八条 IPC 消息**名称齐、语义空**;S1 退出标准(单节点 4 路窗口 IPC 增删、帧边界切换、abort 回滚、kill 恢复)一条都未达成。
- 真正能跑的媒体能力仍停留在它复用的 `rk3588_mp4_dist`(按 MP4 分发的静态管线),dvs_media 自身是"壳"。

## 2. dvs_agent(节点代理)现状

目录:`app/dvs_agent/`,C++17,依赖 json-c + SQLite(amalgamation),内置 HttpServer + 静态前端(`web/src`)。

### 2.1 已实现

| 模块 | 状态 | 说明 |
| --- | --- | --- |
| `http_server.cpp` | ✅ 完成 | 线程 accept、HTTP 解析、静态文件服务;单线程处理,无 TLS |
| `main.cpp` 路由 | ✅ 接口面齐 | 登录(`/api/v1/node/login`,cookie session 固定 `sess-1`)、`/api/v1/node/info`、role 查询、**六 scope 配置状态机 REST**(`/api/v1/node/config/{role,input,output,network,registration,sip}` GET/POST/`retry`)、`/api/v1/nodes` 列表/心跳/升级/退役、媒体状态代理(`/api/v1/node/media/status|sources|logs`)、capability、维护(reboot/factory-reset/upgrade)、audit-events、静态前端兜底 |
| `node_store.cpp` | ✅ 基本完成 | SQLite kv 存储;节点种子 JSON(id/role/sn/mac/capabilities/configStates/pendingRole/sip/lifecycle 等,字段与 req 06 模型对齐);**六 scope 配置状态机真实可用**:`set_desired`(desired/desiredValue/operationId,revision 自增)、`mark_applied`、`mark_failed`、`retry`、`all_states`、`config_revision`;密码 SHA-256 存储、verify/set |
| `media_ipc.cpp` | ⚠️ 最小实现 | UDS JSON 行客户端,仅 `query_state()`;连接失败静默返回空串 |
| `maintenance.cpp` | ❌ stub | reboot/factory_reset/upgrade 全部返回固定 ok JSON,**不执行任何实际动作** |
| 心跳线程 | ❌ 占位 | 3 秒循环,`platform_url` 存在时注释写着 "skip actual HTTP client"——**没有真实注册/心跳外发** |
| `web/src` | ✅ 原型前端 | node-device-admin 静态页面,走上面的本地 API |

### 2.2 功能判定

- req 05(节点本机页)的**接口形状**已具备:角色/输入/输出/网络/注册/SIP 六 scope 的 desired/applied 状态机 + revision + operationId 与 req 06 §2 的协议字段基本对齐。
- 但配置**只落库,不生效**:`set_desired` 之后没有任何组件把它推给 dvs_media 或操作系统(网络配置无 NetworkManager 调用、输出模式无 DRM 调用、角色切换无重启作业)。
- req 09 的注册/心跳/回滚、req 10 的升级/退役均为假回执;认证为固定 session 的原型级实现(未登录校验被注释)。

## 3. 对照 docs/req 的覆盖差距

| REQ | 需求 | dvs_media | dvs_agent | 差距评级 |
| --- | --- | --- | --- | --- |
| 0008 角色与信号 | 角色切换作业、输入口、输出参数、能力回读 | — | role scope 状态机 + pendingRole 有 | 🔴 能力探测(capability.c)空;切换作业无执行器 |
| 0009 网络/注册/运维 | 网络配置回滚、注册心跳、重启/恢复/升级 | — | network/registration scope 有;维护 API 有 | 🔴 全部为 stub,无真实系统调用 |
| 0003 传输配置下发 | 白名单/组播/底图 → 媒体引擎 | `set_sources` 消息存在但空 | — | 🔴 无白名单→会话映射 |
| 0004 绑定与能力上报 | capabilities JSON 上报 | capability.c 空 | 节点种子含 capabilities 字段 | 🔴 无运行时探测与上报 |
| 0005 场景切换 | prepare/commit 同帧、幂等、回滚 | IPC 消息存在;状态机无迁移校验/无 revision 幂等 | — | 🔴 核心 S4 未开始 |
| 0006 开窗调度 | 布局片段 → plane 执行 | layout_executor.c 空 | — | 🔴 复用的 rx_compositor 未接入 IPC |
| 0007 清屏 | 清窗保底图、幂等 | `clear` 消息存在但空 | — | 🔴 |
| 0011 实时可靠性 | PTP/VSYNC 状态、离线保画、恢复同步 | query_state 的 ptp 字段硬编码 | — | 🔴 无统计/事件上报通道 |
| 0010 设备管理 | 退役/升级/心跳 | — | retire/upgrade/heartbeat API 存在 | 🟡 数据模型有,动作为假 |
| 0001/0002 分组拼墙 | 平台侧 CRUD | — | — | ⚪ 平台服务不在本仓库范围 |

**总体**:当前完成度对应集成计划里的 **S1/S2 的"脚手架验收"层**,需求 11 条中真正落地的只有 0008/0009/0010 的"节点本机页状态机数据模型"这一薄层;媒体面(0003/0005/0006/0007/0011)在 dvs_media 内为全 stub。

## 4. IPC 方式分析与网络通信建议

### 4.1 当前 IPC:UDS + JSON 行,单请求单响应

- **匹配的部分**:agent 与 media 同机部署(req 04 §3 明确进程隔离理由),UDS 低延迟、免认证复杂度、权限天然受控,**本机 IPC 选型正确,不需要换掉**。JSON 行 + envelope 也与前端/平台协议一致,便于调试。
- **与目标功能的差距**(不是"要不要换协议",而是"当前实现撑不住目标语义"):

| 问题 | 现状 | 目标(req 04/06) | 影响 |
| --- | --- | --- | --- |
| 短连接、单请求 | 每条消息 accept→read→write→close | 需要 `event` **反向推送**(stream_lost、decode_error、clock.unsynced、playback.started 等) | 短连接只能 agent 轮询 query_state,事件时效性和量都不行 |
| 无 messageId 关联 | 响应 `requestId` 恒为 `"ipc"` | req 06 §2.1 要求 `messageId` 回带做幂等 | 无法做重复消息去重、无法并发多请求 |
| 单线程串行、无状态解析 | accept 循环里同步处理,prepare/commit 应立即返回 | prepare 要等解码首帧(秒级),会阻塞其它消息 | 需要"请求排队 + prepare 异步 + result 事件回推" |
| 无 revision 校验 | 任何 commit 都 revision+1 | 旧 revision 拒绝、operationId/expiresAt 校验 | 同帧语义无从谈起 |
| agent 侧客户端同样裸 | 连接失败返回空串,HTTP 层返回空对象 | 需要明确的 MEDIA_SESSION_FAILED/引擎不可用错误码 | 本机页会把引擎故障显示为空状态 |

### 4.2 是否需要补充网络通信

**需要,但分层补,本机 IPC 不动。**

1. **agent ↔ 平台(必须补,当前完全缺失)**:req 06 §2 明确要求 WSS 或 gRPC over TLS、节点主动连接(穿 NAT)、`node.register / node.heartbeat / media.prepare|commit 回执 / media.* 事件`。当前 agent 只有被动的本机 HTTP;心跳线程是空转。这是下一块必须实现的网络面:
   - 传输建议 **WebSocket(WSS)客户端**:C++ 侧可引入轻量 ws 库或先用 libcurl + 自实现,消息即现有 JSON envelope,与 platform 原型(real-api.js)对齐成本最低。
   - 优先级:register/heartbeat(含 media 摘要、ptp offsetUs)> prepare/commit 回执 > 事件流。
2. **agent ↔ media(增强而非替换)**:保留 UDS,把 ipc_server 从"每请求一连接"升级为**长连接会话**:
   - 请求/响应都带 `messageId`;`prepare` 异步执行、完成后以 `media.prepare.result` 事件推送;
   - 增加 agent 订阅事件(event 消息改为注册订阅者,或双向:media 主动 `event:{...}` 推送);
   - 补 revision/operationId/expiresAt 校验与影子布局持久化。
3. **不需要补的**:节点间媒体面网络(ST2110 组播、PTP)由 MTL 承担,不经过 agent;SIP 按需求是可选独立栈,V1.0 可不做。

### 4.3 建议的实施顺序

1. ipc_server 长连接化 + messageId + 异步 prepare + 事件推送(S1 收尾);
2. agent MediaIpcClient 配套升级(订阅事件、超时与错误码上抛到 HTTP envelope);
3. agent 补平台 WS 客户端:register/heartbeat 真实外发 + 平台下行消息映射到六 scope 状态机与 media IPC(S2/S3 交界);
4. capability.c 真实探测(DRM mode/plane 数、MPP 路数、PTP offset),随 register 上报;
5. maintenance 由 stub 换成真实动作(systemctl reboot、A/B 升级脚本),失败保留旧版本。

## 5. 一句话结论

架构与协议形状(进程隔离、八条 IPC 消息、六 scope 状态机、envelope/revision/operationId 字段)已经和 `docs/req` 对齐;但 dvs_media 的引擎四模块与 dvs_agent 的注册心跳/维护均为空壳,IPC 需要从"一问一答"升级为"长连接 + 异步 + 事件推送",并**新增 agent→平台的 WSS 注册心跳通道**——这是当前离需求最近的一块结构性缺口。
