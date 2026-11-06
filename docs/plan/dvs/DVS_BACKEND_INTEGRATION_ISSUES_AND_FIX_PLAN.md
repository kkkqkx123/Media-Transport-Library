# dvs_agent / dvs_platform 后端集成问题清单与修改方案

日期：2026-11-06（上游合并后修订）
范围：`app/dvs_agent`、`app/dvs_platform`（控制面，json-c + SQLite 栈）
说明：本文档先列问题、再给方案与实施状态。上游 pull（`2fee07ca..ca4defbb`，含 WS 通道/契约校验/升级任务队列）已独立实现了部分修复，本版按合并后的现状重写；持久化细案见同目录 `DVS_OP_STATE_PERSISTENCE_DESIGN.md`。

## 一、集成方式摘要（合并后现状）

- **分层**：`dvs_platform`（编排/像素拆分/文档存储）→ WS 通道优先、短连接 HTTP 兜底 → `dvs_agent`（节点配置状态机 + 转发）→ Unix socket JSON 行协议 → `dvs_media`（MTL/MPP/RGA 数据面）。
- **agent 对下**：`MediaIpcClient` 持久连接、messageId 回显、结构化错误 envelope、64 槽事件环形缓存、`hello` bundle/IP C 版本握手。
- **agent 对上**：`Registration` 线程，空 URL 走本地自治；3s 心跳、3 连败判 offline；心跳携带引擎 lanes/tx/sip 摘要。
- **platform 对下**：泛型文档 CRUD；`OpCoordinator` 两阶段屏障（`begin_with_deadline`/`expire_sweep`），prepare 时 detached 线程按节点 geometry 拆窗 fanout（WS `kLayoutPrepare` 优先、HTTP 回退）；屏障回执走 `/ops/commit`（HTTP）或 WS `kBarrierAck`；升级改为任务队列 worker 串行投递；退役墓碑。

## 二、问题清单与状态

| # | 级别 | 问题 | 状态 |
|---|---|---|---|
| P1 | 安全 | `/ops/prepare` 未做鉴权 | **上游已修**：prepare/commit/config-ack/upgrade-progress 均已 `require_bearer` |
| P2 | 正确性 | 屏障无超时 | **上游已修**：`deadline_ns` + `begin_with_deadline()` + `expire_sweep()`，1s 周期 sweeper（`barrier-timeout` audit） |
| P3 | 正确性 | 回退只有描述符没有执行 | **本次已修**（见 §三）：窗口快照 + `find_committed` + `execute_rollback`，blocked 分支（WS/HTTP/超时 sweeper）三处接入 |
| P4 | 安全 | audit 日志拼接注入 | **本次已修**：`json_escape()` 覆盖全部拼接点 |
| P5 | 小缺陷 | agent 心跳 `lastHeartbeat` 写字面量 `"now"` | **本次已修**：改为 `iso_now()` |
| P7 | 正确性 | op 状态仅内存，重启丢光 | **本次已修**：复用 `docs(kind="op")` 持久化，见持久化细案 |

### 记录在案、未改（需更大改动或产品决策）

- **P6** 节点身份单一静态 token（`DVS_API_TOKEN` 共用），无逐节点凭证。建议后续注册时由平台签发逐节点 token。
- **P8** detached 线程无界（fanout/rollback/commit reporter）。建议后续改有界 worker 池 + 重驱队列。
- **P9** 两个巨型 `main.cpp`（agent ~2000 行 / platform ~1200 行单 lambda 路由）。建议后续拆模块。
- **P10** agent 认证默认关闭（`DVS_REQUIRE_AUTH` opt-in）、双库节点文档手工同步易漂移。属产品/联调决策。

## 三、本次实施（在上游 HEAD 上重新落地）

### P3 回退真正执行
- `Operation` 增加 `windows_json`/`wall_w`/`wall_h`（fanout 快照）与 `created_ns`（GC 用，begin 后由 main 经 `mutable_find` 打点）。
- `OpCoordinator::find_committed(revision)`：最近的、已完结（committed/rolledback）且 revision 匹配、带完整快照的 op。
- `main.cpp::execute_rollback(rollback_op_id, to_revision, members)`：找到快照则按原几何拆分对成员重发 prepare（新 operationId、`applyAtNs=0` 立即生效；投递与 fanout 同路径——WS 优先、HTTP 兜底）；找不到快照只落 `executed:0` audit，不伪造成功。
- 接入点：WS `kBarrierAck` blocked 分支、HTTP `/ops/commit` blocked 分支、超时 sweeper（上游原来只回滚不执行）。
- 回退幂等重放：`rollback_op` 写入 `result["__rolledback__"]` 标记并随状态落库；`pending_rollbacks()` 返回 blocked 且无标记的 op，sweeper 每 1s 重放补跑（audit `op-rollback-replay`）。崩溃窗口（blocked 已落库、回退未执行）由此闭合。

### P4 audit 注入修复
`json_escape()`（转义 `"` `\` 与控制字符），全部拼接点（kind/commit/ws-hello/upgrade-progress/config-ack/ws-offline/register/incomplete-register/rename/retire/config-push-ignored/fanout*/upgrade-enqueue/upgrade-push/barrier-timeout 等）统一过转义。

### P5 心跳时间戳
agent 两处 `lastHeartbeat` 由 `"now"` 改为 `iso_now()`。

### P7 操作状态持久化
细案见 `DVS_OP_STATE_PERSISTENCE_DESIGN.md`。要点：复用 `PlatformStore` 泛型 `docs(kind="op", id=op_id)`；`OpCoordinator::set_store()` 后每个状态变更（begin_with_deadline/set_deadline/on_reply/rollback_op/expire_sweep）末尾同步落库；启动恢复循环 `list_ids("op")` → `operation_from_json` → `restore`（不重发 fanout）；GC 终态超 24h 清除（内存 + 文档），sweeper 每小时触发。

## 四、验证

- 本机无 C++ 工具链，未编译；编译环境跑 `meson test -C build`（平台侧 `test_basic.cpp`：原有 test_ops/test_ops_deadline/test_contract/test_ws_codec 不受影响，新增 test_op_serialization/test_ops_persistence）。
- 建议行为用例：① prepare 无 token 返 401（上游行为，回归确认）；② 不回 commit 的 prepare 到 deadline 后 audit 出现 `barrier-timeout` 且节点收到 revision-1 重发（有快照时）；③ prepare 后 kill 平台、重启，op 从 docs 恢复并照常超时回退；④ blocked 未回退时 kill，重启后第一个 sweeper 周期出现 `op-rollback-replay`。
