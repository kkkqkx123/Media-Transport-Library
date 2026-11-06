# OpCoordinator 操作状态持久化设计（P7）

日期：2026-11-06
关联：`docs/plan/dvs/DVS_BACKEND_INTEGRATION_ISSUES_AND_FIX_PLAN.md` 的 P7。
范围：`app/dvs_platform`（ops.h/ops.cpp/main.cpp、tests/test_basic.cpp）。

## 一、目标与非目标

**目标**：跨节点两阶段操作（`Operation`）的状态跨平台重启保留，消除"平台重启丢光进行中操作"的问题：

1. 崩溃/重启后，`preparing` 的操作不被遗忘——到达 deadline 后照常由 sweeper 判定超时并回退，节点不会停留在无人管辖的中间态；
2. 已 `committed` 的操作作为回退快照保留，重启后仍能对 revision-1 执行 `execute_rollback` 重发；
3. 终态操作可被 GC，防止 `ops` 文档无限增长。

**非目标**（本次不做，见关联文档 P6/P8/P9）：
- 节点侧 agent 的操作状态持久化（agent 是转发者，状态由平台屏障收敛）；
- 跨重启后自动重新 fanout（见"恢复语义"——不重发，只保证超时回退路径闭合）；
- 操作历史审计（audit_log 已覆盖事件流）。

## 二、方案总览

**复用泛型文档存储，不新建表。** `PlatformStore` 已有 `docs(kind,id,data)` 泛型 CRUD（`put_doc`/`get_doc`/`list_ids`/`del_doc`，借用语义），操作记录作为 `kind = "op"`、`id = op_id` 的文档存储。零 schema 变更，事务/并发由现有 `mu_` 互斥保证。

**持久化挂在 OpCoordinator 内部**，而非散落在 main.cpp 各调用点：`OpCoordinator` 增加可选的 `PlatformStore*`（默认空 = 纯内存，现有单测不受影响，调用方仍持 `g_ops_mu`）。每次状态变更（begin_with_deadline / set_deadline / on_reply / rollback_op / expire_sweep）末尾同步落一条文档。写放大可接受：操作频率是秒级/分钟级的控制面事件，SQLite 单条 upsert 微秒级。

## 三、持久化记录格式

`docs("op", op_id)` 的 data 字段：

```json
{
  "opId": "op-3f2a",
  "revision": 7,
  "applyAtNs": 1770000000123456789,
  "deadlineNs": 1770000030000000000,   // 屏障超时（TAI ns），0 = 不设超时
  "createdNs": 1770000000000000000,    // gc TTL 基准，begin 后由 main 打点
  "state": "preparing",               // preparing | committed | rolledback | blocked
  "windowsJson": "[{...}]",           // fanout 窗口快照原文（execute_rollback 重发用）
  "wallW": 7680, "wallH": 4320,
  "members": ["node-a", "node-b"],
  "ready":   ["node-a"],
  "result":  { "node-a": "ready" }    // 含 "__rolledback__": "1" 标记（回退已执行）
}
```

由 `operation_to_json(const Operation&)` / `operation_from_json(json_object*, Operation*)` 双向转换，声明在 ops.h（main.cpp 恢复逻辑需要），实现于 ops.cpp。`from_json` 校验 `opId`、`state` 非空，非法记录返回 false（调用方丢弃并删除该文档）。

## 四、崩溃窗口与恢复语义（如实声明）

| 崩溃时机 | 重启后状态 | 后果 |
|---|---|---|
| begin 之后、fanout 线程完成前 | op 为 `preparing`，部分节点没收到子布局 | 不重新 fanout；sweeper 到 deadline 将其超时→回退。节点侧最坏情况是"收到过 prepare 但平台放弃"→由 revision 单调性 + 下一次操作的 revision 覆盖收敛 |
| fanout 完成、commit 收集中 | op 为 `preparing`，ready 集为崩溃前已知值 | 同上；迟到的 commit 回来会命中 `on_reply` 的 "unknown"（内存 map 已恢复该 op，实际会命中），状态继续推进或超时回退 |
| 已 blocked、回退执行中 | op 为 `blocked`（无 `__rolledback__` 标记） | **可重放**：sweeper（1s 周期）扫描 `pending_rollbacks()`（blocked 且无标记的 op），补跑 `rollback_op` + `execute_rollback`；标记随状态落库，重放幂等 |
| 已 committed/rolledback | 原样恢复 | committed 作为回退快照继续可用 |

恢复入口：`OpCoordinator::restore(const Operation&)` —— 直接写入内存 map、**不再回写**（避免重启风暴中的无谓写）。main.cpp 启动时遍历 `list_ids("op")` 逐条 `get_doc` → `operation_from_json` → `restore`；解析失败的文档 `del_doc` 清除。

**不重发 fanout 的理由**：重发需要节点幂等且需要区分"崩前已送达/未送达"，而 prepare 在节点侧已有 revision 防陈旧保护；自动重发引入的新风险（半新半旧布局叠加）大于收益。保守选择：恢复后只保证**超时回退**这条收敛路径必然闭合。

## 五、GC 策略

- 终态定义：`committed` / `rolledback` / `blocked`。
- `OpCoordinator::gc(now_ns, ttl_ns)`：删除 `created_ns + ttl < now_ns` 的终态 op（内存 erase + `del_doc`）。TTL 取 **24h**——需要覆盖"回退快照至少活到下一次正常操作"的窗口。
- `preparing` 态永不被 GC（交给 expire_sweep 收敛）。
- 触发点：复用上游 1s 周期 sweeper 线程，每小时检查一次（静态 `last_gc` 时间戳门控）。

## 六、并发与锁

- 上游已引入 `g_ops_mu` 保护 `OpCoordinator`（HTTP handler / WS loop / sweeper 均持锁进出），本次持久化沿用同一锁面：恢复循环、快照打点（`mutable_find`）、sweeper 重放与 gc 全部在 `g_ops_mu` 内完成；`execute_rollback` 内部自行取锁，调用方不得持锁调用。
- `put_doc` 内部有 `mu_`，多线程落库安全。

## 七、代码改动清单

| 文件 | 改动 |
|---|---|
| `ops.h` | `#include <json-c/json.h>`；前置声明 `PlatformStore`；`Operation` 增 `created_ns`/`windows_json`/`wall_w`/`wall_h`（上游 `deadline_ns` 保留）；新增 `operation_to_json`/`operation_from_json` 声明；OpCoordinator 增 `set_store`/`restore`/`mutable_find`/`find_committed`/`pending_rollbacks`/`gc`/`persist`（私有）/`store_` 成员 |
| `ops.cpp` | 序列化实现；begin_with_deadline/set_deadline/on_reply/rollback_op/expire_sweep 末尾 `persist`；gc 实现；`pending_rollbacks()`（blocked 且无 `__rolledback__` 标记）；`rollback_op` 写入 `__rolledback__` 标记使回退可重放幂等；`#include "platform_store.h"` |
| `main.cpp` | `g_store` 创建后 `g_ops.set_store(g_store)`；启动恢复循环（含坏文档清除）；prepare handler 内经 `mutable_find` 打点快照与 `created_ns`；三个 blocked 分支（WS kBarrierAck / HTTP /ops/commit / 超时 sweeper）接入 `execute_rollback`；sweeper（1s 周期）对 `pending_rollbacks()` 重放回退（audit `op-rollback-replay`）+ 每小时 `g_ops.gc(now_ns, 24h)` |
| `tests/test_basic.cpp` | 新增：roundtrip 序列化测试；带 store 的 begin_with_deadline→on_reply→重启（新 coordinator + restore）→committed 快照仍可 `find_committed`；`gc` 清终态留 preparing；rollback 标记使 `pending_rollbacks()` 清空；模拟崩溃（blocked 无标记落库）→恢复→重放回退→标记落库 |

## 八、验证

本机无 C++ 工具链未编译；编译环境跑 `meson test -C build`（test_basic 全量断言）。行为用例：
1. prepare → kill 平台 → 重启 → audit 里该 op 到 deadline 出现 `barrier-timeout` + 回退重发；
2. commit 达成屏障 → 重启 → 构造新 op blocked → 回退能拿到重启前 revision 的快照重发；
3. blocked 未回退时 kill → 重启 → 第一个 sweeper 周期出现 `op-rollback-replay`；
4. `sqlite3 /tmp/dvs_platform.db "select id,data from docs where kind='op'"` 观察 gc 后 24h 前的终态行消失。
