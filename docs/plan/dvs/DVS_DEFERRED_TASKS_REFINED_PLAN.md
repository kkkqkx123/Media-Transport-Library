# DVS 延期任务细化方案（终稿：全部决策点已定）

> 源文档：`docs/plan/dvs/DVS_DEFERRED_TASKS_DESIGN.md`（初步设想）只定目标与边界，
> 留下三处待定问题。本文件关闭全部待定问题，给出最终决策与文件级改造清单。
> 实施按本文执行，不再回看初步设想中的开放问题。
> 约束：按职责划分、不建替代模块；显式失败优于掩盖；状态诚实；安全默认；
> 无历史包袱；注释只描述意图。

## 决策总览

| 待定问题 | 最终决策 |
|---|---|
| 网络底座单一样式选型 | systemd-networkd `.network` 文件为唯一持久化样式，`networkctl reload` 为唯一应用入口，`ip` 只做快照诊断 |
| 隔离环境连通检查标准 | `server` 字段冻结为 `host[:port]`（缺省 80），应用后对解析地址做 2 次 TCP 建连（3s 超时），DNS 失败计失败 |
| 生产开启操作入口 | 环境 `DVS_ALLOW_NETWORK_APPLY=1` ＋ 请求体 `confirm:true` ＋ 非空 `operationId` 三者缺一即只做意图落库 |
| 采集口与编码最小规格 | 采集口复用能力上报 `inputPorts`；编码 `{codec h264/h265 缺省 h264, 640x480@24～3840x2160@60, bitrate 500～20000kbps 缺省 4000}` |
| 发送目标与传输最小规格 | 单目标 `{sip: IPv4, udpPort: 1～65535}`，与白名单同一校验 |
| 状态字段最小集合 | `tx{state: idle/configured/running/failed, port, codec, width, height, fps, bitrateKbps, target{sip,udpPort}, error, backendPending}`，进 `query_state.data`，不污染 `lanes`（`lanes` 保持 RX 语义） |
| 单节点自环验证方式 | 控制面 x86 可测（单元＋代理集成）；数据面出流验证排期到单板＋编译服务器，不在开发机验证 |
| 平台是否依赖会话接入 | 平台零依赖（register/heartbeat/config-push/ops 均未消费 sip）→ 保持连通性探测语义，不做 REGISTER/保活/呼叫 |

## 一、网络真实应用

做法：代理原位扩展，不拆新模块。纯函数（server 解析、掩码转前缀、iface 校验、
networkd 文件渲染）放入新文件 `app/dvs_agent/network_apply.{h,cpp}`（职责划分：
HTTP 路由留在 `main.cpp`，系统执行放 helper），`main.cpp` 只保留 gating 与状态翻转。

执行流程（真实执行路径）：校验 → 快照（networkd 文件内容＋`ip addr/route` 文本）
→ 写 `.network` 文件 → `networkctl reload`（fork/exec，不拼 shell，15s 超时，
输出记 `/var/lib/dvs_agent/net_apply_<opid>.log`）→ 连通检查（2 次 TCP 3s）
→ 成功则持久化意图并 `mark_applied`；任一步失败则恢复快照＋reload，
回滚失败报 `NET_ROLLBACK_FAILED` 并如实保留原因。超时按失败回滚。

状态表达：config state machine 仍跟踪意图（`applied`＝意图已落库）；
node 新增 `networkSystem{executed, method, at, error, rolledBack}` 记录系统侧真相；
三种响应消息互不混淆：意图落库（默认）、系统已切换、系统失败已回滚。
`DVS_FORCE_NETWORK_FAIL=1` 保留为注入失败（覆盖两条路径，演练回滚）。

静态 DHCP 与静态 IP 同一 `.network` 样式表达（`DHCP=yes` vs `Address=/Gateway/DNS`），
不引入第二种管理方式。iface 缺省 `eth0`，字符集限 `[A-Za-z0-9._-]{1,16}`，
非法直接 `INVALID_ARGUMENT`。server 主机限 IPv4 或 DNS 名（`[A-Za-z0-9.-]{1,253}`），
端口缺省 80。

前端：网络保存默认 `confirm:false`（意图落库），勾选「隔离环境已确认」才发
`confirm:true`；静态/动态两套文案按 confirm 切换，不谎称已切换。

## 二、输入采集与发送编排

角色互斥是简化依据：同一时刻只处于输入或输出其一。`input` 角色激活时停止接收
合成器；`tx_start` 仅当角色为 `input` 才允许；切回 `output` 与 `clear` 停止现行
管线。`whitelist` 语义冻结为 RX 准入（仅 output 角色有效），不混用于发送。

参数模型：input scope 的 `desiredValue` 扩展为 `{port, whitelist?, encode?, target?}`，
其中 `encode{codec,width,height,fps,bitrateKbps}`、`target{sip,udpPort}` 缺字段填缺省、
超范围报 `INVALID_ARGUMENT`。上限取「上报 `capability.tx`（若有）与 SoC 冻结规格」
之已有值（上报优先，不用冻结值掩盖更小的上报值）。无 `encode/target` 时走旧
白名单意图路径（兼容旧前端）。

引擎：新增 `app/dvs_media/engine/tx_session.{c,h}`（职责划分：TX 会话 vs 合成器引擎；
纯 C、无 MTL/MPP 依赖、x86 可单测）。`engine_ctx` 增 `role[16]` 与 `tx` 字段及
`dvs_engine_tx_start/stop` 包装；`set_role` 记录角色并执行互斥停止；
`clear` 同时复位 TX。数据面启动点为 `dvs_tx_backend_start` 钩子：
默认空＝`configured`＋`backendPending:true`（诚实：参数已存、未出流），
单板任务按 `rk3588_live_ingest` 的采集→转换→编码→发送环节按职责取用后填实，
在编译服务器构建验证（不在开发机验证）。

IPC：新增 `tx_start`（要求 port＋encode＋target 齐全，缺一 `INVALID_ARGUMENT`；
角色非 input 报 `TX_ROLE_MISMATCH`）与 `tx_stop`（幂等）；
`query_state.data` 新增 `tx` 对象（hook 为空时报 `idle`，单元测试即此路径）。
`configure` 的 scope 语义不变（output/role/whitelist），不复用它表达 TX。

代理 input 分支：白名单沿旧同步路径转发；含 `encode/target` 时调 `tx_start`：
`running`→`mark_applied`（已启动）；`configured`＋backendPending→`mark_applied`
但消息如实写「参数已下发，数据面待单板联调」；失败→`mark_failed`（引擎 code/reason
原样保留）。新增 `GET /api/v1/node/media/tx` 直透 `data.tx`；
心跳增 `txDigest{state,port,target}`，平台存入 `metrics.txDigest`（加法，不做逻辑）。

能力：`capability` 增静态 `tx{codecs:[h264,h265], maxWidth:3840, maxHeight:2160,
maxFps:60, maxBitrateKbps:20000}`（SoC 公开上限，非探测，注释写明）；
`capture` 复用 `inputPorts`，不新增字段。代理种子 capabilities 同步加 `tx` 静态值；
引擎合并逻辑不变（上报优先）。

前端：`hardware-capabilities.js` 的 `resolve()` 增 `tx`（上报优先、冻结值兜底）
与 `hasReportedTx`；输入区增编码 5 字段＋目标 2 字段，按 `tx` 上限校验；
保存发送完整 `{port, encode, target}`；tx 状态行从 `/media/tx` 渲染。
信号状态行保持占位文案不变（不伪造信号）。

## 三、会话接入完整实现

结论：不做完整会话。平台四处入口（register/heartbeat/config-push/ops fanout）
均未消费 sip，无契约依赖；补最小注册与保活无消费者，是无效工作。

落地（探测语义补齐，非扩范围）：sip scope 四态持久化为
disabled/probing/connected/failed，其中 probing 为探测窗口内可见态
（请求处理先写 probing 再做同步 TCP 探测，结果覆盖为 connected/failed）；
失败 `mark_failed(SIP_UNREACHABLE)` 且不污染媒体状态（现有隔离保持）。
心跳增 `sip{enabled,state,server,port}` 摘要，平台两处 heartbeat 入口原样存为
`sipDigest` 顶层键（加法，无逻辑分支）。前端 sip 区标注「连通性探测（非完整会话）」，
探测中态可视。若将来平台提出契约，另立任务补最小注册与保活，本轮不预留协议字段。

## 四、顺序与文件级清单

顺序：网络（代理）→ TX 会话（媒体）→ 代理 input 转发 → 心跳摘要（代理＋平台）
→ SIP 补齐（代理＋平台＋前端）→ 前端输入区/网络区 → 单测 → 文档回写。

| 文件 | 改动 |
|---|---|
| `app/dvs_agent/network_apply.{h,cpp}`（新，职责：系统执行） | server 解析、掩码转前缀、iface 校验、networkd 渲染、fork/exec 执行＋快照回滚 |
| `app/dvs_agent/tx_config.{h,cpp}`（新，职责：TX 参数） | encode/target 解析＋缺省＋上限校验、tx_start 负载构造 |
| `app/dvs_agent/main.cpp` | network 分支 gating＋networkSystem＋诚实消息；input 分支 encode/target＋tx_start 转发；`GET /media/tx`；sip probing 预写 |
| `app/dvs_agent/registration.cpp` | 心跳增 `sip`＋`txDigest` |
| `app/dvs_agent/node_store.cpp` | 种子 capabilities 增静态 `tx` |
| `app/dvs_agent/meson.build` | 新文件进 agent 与 test 目标 |
| `app/dvs_agent/tests/test_basic.cpp` | network_apply＋tx_config 用例 |
| `app/dvs_media/engine/tx_session.{c,h}`（新，职责：TX 会话） | 校验＋状态机＋backend 钩子＋状态 JSON |
| `app/dvs_media/engine/engine_ctx.{h,c}` | role＋tx 字段、互斥停止、tx 包装 |
| `app/dvs_media/engine/capability.{c,h}` | 静态 tx 规格＋序列化 |
| `app/dvs_media/ipc/ipc_server.{h,c}` | tx_start/tx_stop＋query_state 嵌 tx（hook 为空报 idle） |
| `app/dvs_media/engine/ipc_engine_bridge.c` | hook 接线 |
| `app/dvs_media/tests/test_tx_session.c`（新）＋ `tests/meson.build` | 角色门、后端三态、停止幂等、JSON |
| `app/dvs_platform/main.cpp` | 两处 heartbeat 存 `txDigest`（metrics 下）＋`sipDigest`（顶层） |
| `app/dvs_agent/web/src/_shared/hardware-capabilities.js` | tx 规格 resolve |
| `app/dvs_agent/web/src/pages/node-admin/node-admin.html` | 输入区编码/目标表单、网络 confirm＋iface、sip 探测标注 |
| `app/dvs_agent/web/src/pages/node-admin/node-admin.js` | 输入完整对象＋校验、tx 渲染、网络 confirm、sip probing 渲染 |

## 五、验证

代理/平台：本地 `ninja` 增量构建＋`dvs_agent_test`＋`dvs_platform_test`；
 допол curl 烟雾（意图落库默认、confirm 无 env 仍意图落库、sip 不可达 failed、
/media/tx 无引擎时结构化错误、heartbeat 含新摘要）。
媒体：`test_tx_session` 独立编译运行（`engine/tx_session.c`＋json-c，无 MTL）；
`capability.c`/`ipc_server.c` 做 `gcc -fsyntax-only`；
`engine_ctx.c`/`ipc_engine_bridge.c` 需 MTL 头文件，只在编译服务器验证，
本轮不本地编译，改动保持机械最小。
前端：`node --check` 过两处 JS；networkd 真实执行、TX 出流、单板自环只在隔离环境
＋编译服务器验证，不在开发机验证。
门禁：接收回归不受影响（RX 路径零改：whitelist/configure/prepare/commit/lanes 语义不变）。
