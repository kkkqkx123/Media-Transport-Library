# dvs_platform 功能分析

> 分析对象：`app/dvs_platform/`（平台服务，C++17）。编写时间：2026-11-08，基于 `feat/rk3588-apps` 分支。

## 1. 定位

多节点媒体系统的**平台侧控制服务**，与 `dvs_agent` 同栈（json-c + 内置 SQLite，无其他运行时依赖，复用 `dvs_common` 的 HTTP/SHA-256/SQLite 封装，并 in-tree 编译 `dvs_agent` 的部分源码，不预抽公共库）。职责：节点注册/心跳网关、分组/墙/场景/绑定/白名单文档管理、整墙布局像素拆分、跨节点 prepare/commit 屏障与回退、退役墓碑与串行批量升级。纯控制面，不碰媒体数据。

```bash
cd app/dvs_platform && meson setup build && ninja -C build && meson test -C build
```

## 2. 模块结构

| 文件 | 职责 |
|---|---|
| `main.cpp` | 入口：HTTP 路由（文档 CRUD、注册/心跳、操作编排）+ 静态前端；统一 JSON 响应封装（`ok/status/code/msg/data/requestId`），支持 `Idempotency-Key` 幂等 |
| `platform_store.cpp/.h` | SQLite 通用文档存储 `docs(kind,id,data)`：kind = node/group/wall/scene/binding/whitelist/playback/op；另有 tombstones 表（退役墓碑，30 天内拒绝重注册）与本地审计日志（`audit`/`audit_list`） |
| `layout.cpp/.h` | 布局像素拆分与业务规则校验（纯 JSON 逻辑，无 store 访问，全部可单测） |
| `ops.cpp/.h` | `OpCoordinator` 跨节点操作协调器：prepare/commit 屏障、超时、回退、升级、墓碑 |
| `web/` | 静态管理前端（分组/墙/场景管理页面） |
| `tests/test_basic.cpp` | 基础自测（`meson test`） |

## 3. 关键机制

### 3.1 布局像素拆分（layout.cpp）

平台**独占所有取整**（节点永不取整）：约定左边/上边向下取整，右边/下边向上取整。核心函数：

- `fragment_for_node`：把全局 flat 窗口集（墙像素：id/sig/x/y/w/h/z/muted）与某节点画布矩形求交，产出节点本地窗口列表（dst 为节点本地目标矩形，src 为相对窗口原点的源裁剪；窗口矩形与信号帧 1:1 映射）；
- `base_subrect`：底图子矩形 = 墙画布 ∩ 节点画布（无重叠返回 false）；
- **预算与约束**：`max_windows_per_node`（单节点画布最多窗口数，镜像 MP4_PIP_V1_NV12_LANES 上限）、`wall_min_window_side`（最小窗口边 = max(64px, 最小 cell 边 1/4)）、`snap_windows_to_cells`（固定模式窗口吸附到主 cell）、`validate_per_node_budget`（超预算点名列出首个节点）；
- **节点资格门**：`node_clock_ok`（在线 + output 角色 + PTP 同步且偏移 ≤ `ptp_max_offset_us`，时钟未知视为不合格）；
- `multicast_pool_next`：多播地址池分配 (addr, port)，可钉端口，耗尽即失败；
- `layout_apply_at_ns`：目标提交时刻 = now + 两个帧周期保护带（50ms 下限），TAI 纳秒；
- `layout_windows_hash` / `layout_dirty_compare`：窗口数组规范化哈希与脏比对（{dirty, diff[], savedHash, liveHash}），支撑幂等与回执匹配；
- 文档形状校验族（group/wall/scene/binding/whitelist/playback/image_adjust/base_image/volume）：失败返回 `CODE:reason`，**大声失败**，整单拒绝。

### 3.2 跨节点屏障（OpCoordinator）

纯逻辑无 socket，传输层（短 HTTP 或 WS 通道）喂入节点回执、读出下一次广播：

- 状态机：`preparing → committed | rolledback | blocked`；`on_reply` 记录每节点 prepare/commit 回执，`missing` 给出未齐成员；
- **超时**：`begin_with_deadline` 设 TAI 纳秒截止，`expire_sweep` 把过时仍 preparing 的屏障置 blocked；
- **回退**：`rollback_op` 生成回退到 revision-1 的重发描述（携带扇出快照 `windows_json`/墙尺寸/场景音量，恢复失败前的音量）；打 `__rolledback__` 标记保证跨重启幂等可重放（`pending_rollbacks` + `find_committed`）；
- **崩溃恢复**：操作以 `operation_to_json/from_json` 持久化为 docs("op") 文档，重启后 `restore` 重建；`gc` 按 TTL 回收终态操作（preparing 永不 gc，归 expire_sweep 管）；
- 一致性约束：`wall_has_preparing` 保证同一面墙同时只有一个准备中操作，`wall_committed_revision` 跟踪墙已提交的最大修订号；
- **帧锚定**：Operation 携带 (epoch_ns, fps, frame_index)（frame_index<0 = 立即提交，epoch=0 = 传统无帧语义，跳过帧检查而非失败），与 dvs_media 的帧锚定提交对接；回执侧 `barrier_lanes_ready`（所有 lane ready）与 `barrier_lanes_frame_consistent`（同 ack 内帧序一致）把关节点确认。

### 3.3 节点网关与升级

- 接收各 `dvs_agent` 注册/心跳，可经 `http_client` 反向调用节点；`node_decode_budget` 读节点上报的 maxDecodeSessions（未知返回 -1，调用方不得据此设卡）；
- 大墙扇出支持分片并行（`fanout_parallelism`，env 可调，钳位到合理区间）；成员规模上限 `max_wall_members`；
- 串行批量升级节点代理，进度经代理 `uplink_upgrade_progress` 回传；
- 退役节点写墓碑（tombstone，30 天过期 + gc），防止重注册复活。

## 4. 与其他模块的边界

- **dvs_agent**：平台是决策端（布局拆分、屏障编排、升级调度），代理是执行端点；两端以帧锚定 commit 报告对齐。
- **dvs_media**：平台不直接对话引擎；子布局经代理 `translate_platform_layout` 翻译后进入引擎 prepare/commit。
- **dvs_common**：HTTP 服务/客户端、SHA-256、SQLite 封装；并复用 dvs_agent 部分源文件 in-tree 编译。
