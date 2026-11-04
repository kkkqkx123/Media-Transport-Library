# DVS IPC 异步化实现审计与修改方案

> 对应方案：`docs/plan/dvs/DVS_IPC_ASYNC_PLAN.md`
> 对应代码：`app/dvs_media/ipc/`、`app/dvs_agent/media_ipc.*`、`app/dvs_agent/main.cpp`
> 说明：本文件仅记录审计结论与修改方案，不包含完整代码；修改时遵循原方案的非目标与依赖约束。

## 0. 设计合理性总评与修改方向

原审计结论主体成立：协议与状态层已落地，多连接、幂等、事件时效、prepare 接入、payload 解包、abort 收紧确为缺口，实施顺序 payload 优先正确。

逐项判定：

- 合理并保留：P1 多连接线程化、P2 messageId 幂等、P5 prepare 接 hook、P6 payload 解包、P7 abort 收紧、P9 事件补齐、P11 legacy 清理、P12 测试补齐。
- 不成立，需修正：P10 verb 缺口已不存在，server 早已支持 tx_start/tx_stop，P10 改为已解决，不再安排工作。
- 欠规定，需补强：3.5 未规定 slot 复用时 worker 写归属与 wmu 生命周期，存在错写与 use-after-destroy；3.6 全局缓存键在多连接下会误命中，且未处理同 op 不同 messageId 的重复 worker；3.1 未说明 prepare 体含 windows 时 hook 应放行，否则接 hook 即回归。
- 过度设计，需简化：3.2 后台事件线程为达成事件可见性引入常驻读线程、响应分发、条件等待与双路重连竞争，而事件消费端本就是轮询（HTTP events 拉取与上报线程轮询），同互斥下的 drain 主动非阻塞排空已达到相同可见性，复杂度与风险远小。V1 改为 drain 排空，线程延期。
- 遗漏，需新增：P13 同 op 幂等重放会重复 spawn worker；P14 worker 持裸指针的 slot 复用竞争；P15 query 大 envelope 在固定 1024 缓冲下截断；P16 平台 windows 形态与引擎 streams/layout 形态不一致。

修改方向定稿：server 集中解包 body，prepare 先校验 hook 再进状态机且重放不重复起 worker；abort 由状态机自卫并返回码；事件在 commit/clear/abort 补 state_changed；server 每连接一线程加代际号与常驻锁；幂等改为每连接环形缓存；agent 不加线程，drain 排空加 connected 与 requestId 核对；bridge 对纯操作帧与 windows 放行；补 set_sources 别名；放大 envelope 缓冲；移除 legacy 整型状态与外部可变入口。

## 1. 审计结论

- 协议与状态层主体已落地：状态机、长连接读循环、异步 prepare、事件推送、revision/operation 校验、agent 客户端基础、HTTP 透传端点均已存在。
- 与方案相比，仍存在多连接、messageId 幂等、事件时效、prepare 与引擎 hook 接入、payload 解包、abort 校验等缺口。
- 集成链路存在实际断点：agent 将业务字段嵌套在 `payload`，而 server 与 engine hook 按顶层字段读取；事件链路因 drain 不读 socket 而不能及时暴露。
- 存在少量 legacy/兼容代码，建议清理。
- P10 经复核已解决：server 已实现 tx_start/tx_stop 分支，agent 调用可达，仅无 hook 时回 NOT_IMPLEMENTED 属预期。

## 2. 问题清单

### P1 服务端不是“每连接一线程”

- 现状：`dvs_ipc_server_run()` 在 accept 后于主线程同步读取该连接直到 EOF。
- 影响：第二个客户端必须等第一个断开；不符合方案 G1 的多连接前提。
- 位置：`app/dvs_media/ipc/ipc_server.c` accept 循环。

### P2 messageId 级幂等未实现

- 现状：server 无 requestId 响应缓存；幂等仅在 state_machine 层按 operationId + revision 处理。
- 影响：agent 重试时 server 可重复执行非幂等消息。
- 位置：`ipc_server.c` handle_line；`media_ipc.cpp` request。

### P3 agent 事件仅在请求顺带读取

- 现状：事件仅在 request() 过程中顺带读取并缓存，无独立及时性。
- 影响：prepare 结果与广播在无后续请求时滞留 socket，`/events` 取不到。
- 位置：`app/dvs_agent/media_ipc.cpp`、`media_ipc.h`。

### P4 drain_events 不读取 socket

- 现状：`drain_events()` 只返回内存缓存。
- 影响：新事件滞留在 socket 缓冲区，直到下次 request 顺带读出。
- 位置：`media_ipc.cpp` drain_events。

### P5 prepare 未调用引擎 hook

- 现状：`hook_prepare` 仅在 configure 分支调用；prepare 分支只做状态机校验。
- 影响：布局/白名单/输出校验与暂存被跳过。
- 位置：`ipc_server.c` prepare 分支；`engine/ipc_engine_bridge.c` hook_prepare。

### P6 payload 解包未实现

- 现状：agent request 将 payload 嵌套为 `{"payload": ...}`；server 所有字段读取均为顶层。
- 影响：HTTP `/prepare`、`/layout`、configure 转发的业务字段不可见，布局链路实际断裂。
- 位置：`media_ipc.cpp` request；`ipc_server.c` handle_line。

### P7 abort 校验过宽

- 现状：abort 仅拒绝 `IDLE && 无 opid`；state_machine abort 不检查 op_state。
- 影响：已 committed/idle operation 也可被 abort，违反方案“仅 PREPARING/PREPARED 可中止”。
- 位置：`ipc_server.c` abort 分支；`state_machine.c` dvs_state_abort。

### P8 agent 缺少 connected()

- 现状：文档声明 `bool connected() const`，但头文件与实现均无。
- 位置：`media_ipc.h`、`media_ipc.cpp`。

### P9 缺少 media.state_changed 事件

- 现状：server 仅推送 `media.prepare.result` 与 `media.layout_applied`。
- 影响：方案 G4 的三类事件不完整。
- 位置：`ipc_server.c` broadcast_event 调用点。

### P10 verb 集合已对齐，无需工作

- 现状：server 已支持 tx_start/tx_stop，agent 调用可达；无 hook 时回 NOT_IMPLEMENTED 属预期。
- 结论：原缺口描述过期，标记已解决。另补 set_sources 别名以覆盖方案 verb 表。

### P11 legacy / 兼容代码

- 现状：`state_machine` 保留 `int state` 与 legacy accessors（get/set name、set revision），仅测试使用；与方案 `dvs_state_t` 设计不一致，且 query 实际按 op_state 映射，committed 值从不可见。
- 位置：`state_machine.h/c`；`tests/test_state_machine.c`。

### P12 测试覆盖缺口

- 现状：agent 测试仅覆盖 empty socket（UNCONFIGURED），未覆盖不存在 sock path 的 UNREACHABLE；无 messageId 幂等、多连接、事件时序测试。
- 位置：`app/dvs_agent/tests/test_basic.cpp`；`app/dvs_media/tests/test_ipc.c`。

### P13 prepare 幂等重放重复起 worker

- 现状：同 op 同 revision 重入 begin_prepare 返回成功，调用方无区分，再次 spawn worker。
- 影响：双 worker 竞争 finish，一快一慢可致后者 INVALID_STATE 回落，事件重复。
- 位置：`ipc_server.c` prepare 分支。

### P14 worker 持裸 slot 指针

- 现状：prepare job 存 `dvs_client_t*`，客户端断开复用后 worker 错写新连接；wmu 在断开销毁而 worker 仍可触及。
- 位置：`ipc_server.c` prepare_job_t 与断开清理。

### P15 大 envelope 截断风险

- 现状：响应与广播 envelope 固定约 1K 栈缓冲，query_state 含 lanes/tx/ptp 放大后可超长截断成非法 JSON。
- 位置：`ipc_server.c` send_envelope 与 broadcast_event。

### P16 平台 windows 形态引擎不识别

- 现状：agent `/layout` 转发 windows/geometry，引擎 layout 仅识 streams/layout，hook 直通则误拒。
- 影响：按 3.1 接 hook 若不放行即回归，202 变 400。
- 位置：`ipc_engine_bridge.c` hook_prepare；`layout_executor.c`。

## 3. 修改方案

### 3.1 payload 与 prepare 接入（优先）

1. server 集中归一业务体：type/messageId 取顶层，若 payload 为对象则业务字段取 payload，否则回退顶层；operationId/revision/scope/value/whitelist 与 tx 参数统一走该业务体，hook 统一收业务体。
2. prepare 先调 hook 做存在性校验，失败直接回错误不进状态机；通过再进 begin_prepare。hook 仅校验出现的媒体字段，纯操作帧与 windows 形态放行，不误拒。
3. configure 继续走同一 hook，天然按字段存在性分支，无需双写。
4. 幂等重放不重复起 worker：快照已是同 op 同 revision 的 PREPARING/PREPARED 时直接回 202。
5. 补 set_sources 别名，走同一白名单路径，覆盖方案 verb 表。

### 3.2 agent 事件时效（V1 不加线程）

1. 不引入常驻读线程：request 与 drain 共用同一互斥串行，drain 以 poll 零超时排空可读行，事件入环，非事件杂散丢弃，半包留存。
2. request 保留顺带缓存，并核对响应 requestId 与本次 messageId，不一致继续等，超时断线重连语义不变。
3. 环满按新挤掉旧，drain 取走全部，poll 侧断线时关闭句柄促重连。
4. 增加 `connected()`，反映句柄有效状态。
5. 后台线程延期理由：消费端本就轮询，线程不降低端到端时延，反增响应分发与重连竞争；后续若事件率证明需要，可不改协议加线程。

### 3.3 abort 与状态机收紧

1. 状态机自卫：仅 PREPARING/PREPARED 可中止，否则保持不变并返回码；opid 缺省中止在途单，opid 不匹配返回参数错。
2. server 按返回码映射 INVALID_STATE 与 INVALID_ARGUMENT，仅成功时调引擎 abort。
3. committed/idle 的 abort 明确拒绝，避免误清最近 operation 导致 commit 重放失效。

### 3.4 事件完整性

1. commit 成功后依次广播 layout_applied 与 state_changed；clear/abort 成功后广播 state_changed。
2. prepare worker 事件保持 requestId 回带，广播事件体带 operationId/revision/state/opState，与 agent 前缀判定一致。

### 3.5 服务端多连接

1. accept 循环为每客户端起分离线程，线程内读行调 handle_line，EOF 标记 inactive 关句柄。
2. slot 锁常驻不销毁，加代际号；worker 存槽号加代际加句柄，写前验代际与有效，失效只落状态不投递。
3. 写锁顺序统一先全局后单连接，广播沿用该顺序；V1 限最大客户端数，超限回 BUSY。

### 3.6 messageId 幂等

1. 每连接维护有限响应环，命中直接回放首次响应，不重复执行；仅缓存客户端显带 messageId 的即时响应，事件不缓存，服务端自生成 id 不缓存。
2. 该缓存只被本连接读线程触及，无需另加锁，避免全局键跨连接误命中。
3. 与 operation 幂等并存：messageId 命中优先，operation 重放由 3.1 的不重复起 worker 承接。

### 3.7 legacy 清理

1. 移除整型 state 与外部 get/set 入口，query 统一按 op_state 映射，revision 只经 prepare/commit 演进。
2. 单测改走 transition 与快照验证，不再直设 revision。
3. 移除后 committed 不再作为稳态名，revision 即已生效版本。

### 3.8 测试与验证（不编译/运行时仅方案）

1. dvs_media：增加 messageId 重复幂等、多连接 accept、abort 状态矩阵、payload 解包与 windows 放行测试。
2. dvs_agent：增加不存在 sock path 返回 MEDIA_ENGINE_UNREACHABLE、connected()、drain 排空测试。
3. 联调：mock client 走 configure → prepare → 事件 → commit → 广播 → query_state，并验证 `/events` 可读到广播事件。

## 4. 建议实施顺序

1. payload 解包 + prepare 调用 hook 加 windows 放行（打通实际布局链路）。
2. agent drain 排空 + connected（打通事件链路，不加线程）。
3. abort 状态收紧。
4. 多连接线程化加代际号。
5. messageId 每连接幂等加 worker 去重。
6. legacy 清理与测试补齐。

## 5. 非目标保持不变

- 不实现真实会话/布局/音频引擎。
- 不更换 UDS + JSON 行传输。
- 不引入新第三方依赖。
- 不实现平台 WSS 通道。
