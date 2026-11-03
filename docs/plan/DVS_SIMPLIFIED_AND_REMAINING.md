# DVS 简化项与剩余项 — 专项修改方案

本文件针对 `DVS_REMAINING_TASKS.md` 中两类未收口工作给出独立、可落地的修改方案：

- **简化项（S4 媒体引擎细化）**：T3/T4 落地的 `recreate-on-commit` 基线与空 stub 仍存在的三个 S4 简化点。
- **剩余项（T7–T9 / S3–S5）**：跨层平台服务、跨节点 S4、S5 退役/批量升级/容量。

> 验证环境约定：✅ x86 可构建/单测；🔧 代码已写但本机(x86, 缺 MPP/MTL)无法链接构建，仅 RK3588/devs 可编译。

---

## 一、简化项（S4 媒体引擎细化）

T3/T4 的基线把"布局切换"实现为 `stop+free+create+start` 重建 compositor，并把 per-session 会话管理、音频仲裁留作 S4。下面逐项给出修改方案。

### S-A 帧边界原子切换（最高风险，硬件相关）

- **当前**：`engine/engine_ctx.c::dvs_engine_apply_layout` 每次 commit 重建整个 `rx_compositor`（极短黑帧，且解码器重建开销大）。
- **目标（req 06 §2 / S4）**：两阶段 `prepare`(预建/复用 plane、framebuffer、解码器，保持当前画面) + `commit`(仅切 plane 属性 src_crop/dst/z/visible，在 vblank 帧边界原子切换)；commit 前存影子副本，失败/abort 从副本回滚。
- **修改方案**：
  1. 在 `mp4_dist/rx/rx_compositor.c` 拆出 `rx_compositor_prepare(layout)` 与 `rx_compositor_commit()`（复用 `:396` 的 lane→plane resolve，但把"切换"与"构建"分离）。**这是共享模块，改动需 mp4_dist 联调回归。**
  2. `engine_ctx.c` 改为：commit 调 `rx_compositor_commit()`；`apply_at` 字段驱动延迟到指定帧号切换。
  3. `engine/engine_ctx.h` 增加 `pending` 影子布局 + `apply_at` 字段。
- **验证**：RK3588 单板 4 路热切验证无黑帧、abort 回滚；**本机不可构建**。
- **状态**：⬜ 规划中（需改共享 `rx_compositor`，留待硬件联调）。

### S-B per-session 会话 + 白名单/LRU 准入（中风险）

- **当前**：`engine/session_mgr.c` 仅 1:1 委托 `engine_ctx`；无按 `(mcast_ip, port)` 唯一会话复用、无白名单准入、无 8 路 1080p60 上限 + LRU 淘汰（req 03 §7.3）。
- **修改方案（本次已落地基线）**：
  1. 新增 `engine/session_admit.{h,c}`（**自包含，不依赖 MTL/MPP，x86 可语法校验**）：维护 `(ip:port)→lane` 会话表，支持
     - `dvs_session_whitelist_set(csv)`：逗号分隔允许源；空=允许全部；
     - `dvs_session_admit(ip,port,&lane)`：白名单校验 → 命中复用(LRU 刷新) → 空位分配 → 满则淘汰最久未用(LRU)；
     - `DVS_MAX_SESSIONS=8` 上限（1080p60 约束，平台可调）。
  2. `engine_ctx.c::build_compositor` 在组装 layout 前对每个 stream 调 `dvs_session_whitelist_check` 拒绝非法源，并在成功后 `dvs_session_admit` 刷新 LRU。
  3. 动态换源（st22p_rx_update_source）留 S-A/S4 细化。
- **验证**：`tests/test_session_admit.c` 在 **x86 实跑通过**（白名单 exact/any-port、命中复用同 lane、满 8 淘汰 LRU、release 递减）；`engine_ctx` 接线需 RK3588 链接构建。
- **状态**：✅ `session_admit` 逻辑已单测验证；🔧 `engine_ctx` 白名单接线已写、待 RK3588 构建。

### S-C 音频 ALSA 仲裁（中风险，已落地）

- **当前**：`engine/audio_arbiter.c` 为纯 stub（仅注释）。
- **修改方案（本次已落地）**：
  1. `audio_arbiter.c` 解耦 `engine_ctx.h`（前向声明 `struct dvs_engine`），仅依赖 ALSA；`dvs_audio_set(owner, volume)` 实现：
     - owner 非空且以 `hw:` 开头 → 直连该 ALSA 设备；否则 `default`；owner 空 → 静音(volume=0)；
     - 用 `snd_mixer` 打开设备，对 `Master` 元素按 0..100 设播放音量（无 Master 则软静音）。
  2. `engine_ctx.c` 增加 `audio_owner/audio_volume` 字段 + `dvs_engine_set_audio()`；`apply_layout` 在 compositor 启动后调 `dvs_audio_set` 应用。
  3. `layout_executor` 解析布局 JSON 的 `audioOwner`/`audioVolume` 并在 commit 时通过 `dvs_engine_set_audio` 下发。
- **验证**：`audio_arbiter.c` x86 语法校验通过（ALSA 头存在）；运行时音频路由需 RK3588。
- **状态**：🔧 已落地（语法校验通过，未链接构建）。

---

## 二、剩余项（T7–T9 / S3–S5）

### T7 — S3 平台服务 `platform/` + 视频控制平台接入（⬜ 未开始）

- **内容**：节点代理 `dvs-agent` 已具备注册/心跳；但"平台侧" `platform/` 服务（控制链路、批量指令、拓扑聚合）尚无代码。需与"video-control-platform"原型对接（见主文档 S3）。
- **修改方案**：
  1. 新建 `app/platform/`（C++17，复用 agent 的网络栈/JSON）：`platform_service` 聚合多节点状态、下发 bulk `prepare/commit`、提供 `/api/v1/grid/...`。
  2. agent 侧新增 `platform` scope 或沿用 `control` scope，支持被平台纳管。
  3. 对接 `video-control-platform` proto（gRPC/REST 二选一，待定）。
- **验证**：x86 可构建平台服务桩 + 单测；端到端需平台原型。
- **状态**：⬜ 未开始。

### T8 — 跨节点 S4（prepare/commit 跨格、apply_at 帧对齐、回滚）（⬜ 未开始）

- **内容**：单节点 S4（S-A）完成后，跨节点需：
  - 全局 `revision`/opId 协调；
  - 跨格 `prepare`（各节点预建）→ `commit@apply_at`（统一帧号切换）→ 任一节点失败整体回滚；
  - 由平台服务(T7)编排。
- **修改方案**：在 `platform/` 增加跨节点事务协调器；节点侧复用 S-A 的两阶段接口 + `apply_at`。
- **验证**：依赖 T7 + S-A；本机不可构建。
- **状态**：⬜ 未开始。

### T9 — S5 退役/墓碑、批量升级、容量压测、文档三处回写（⬜ 未开始）

- **内容**：
  1. 节点退役/墓碑：agent `maintenance` 增加 `retire`/`tombstone` 软状态 + 平台侧可见。
  2. 批量升级：agent `maintenance::upgrade` 已做 SHA-256 + A/B；平台侧批量编排 + 断点续传。
  3. 容量压测：节点 `capacity` 自上报（CPU/内存/解码负载/显示负载），平台侧聚合压测。
  4. 文档三处回写：`DVS_FRONTEND_INTEGRATION_PLAN.md` S0/S1/S2 状态、`AGENTS.md`、`docs/app/README.md` 同步真实进度。
- **修改方案**：
  - agent 侧 `maintenance.cpp` 增 `retire`/`tombstone`；新增 `/api/v1/node/capacity` 自上报（读 `/proc`，**x86 可构建+单测**）。→ **已实现并实测**：`GET /api/v1/node/capacity` 返回 `cpuBusyPercent/memTotalKB/memAvailableKB/memUsedPercent/load1..15/uptimeSec`。
  - 平台侧批量编排留 T7。
- **验证**：agent 侧容量端点 **x86 实跑通过**（curl 返回真实 /proc 数据）；退役/墓碑待实现；平台侧留 T7。
- **状态**：✅ 容量自上报已落地+实测；⬜ 退役/墓碑与平台侧待做。

---

## 三、执行状态总表

| 项 | 状态 | 说明 |
|---|---|---|
| S-A 帧边界原子切换 | ⬜ 规划中 | 需改共享 `rx_compositor`，硬件联调 |
| S-B 白名单/LRU 准入 | ✅ 逻辑已单测（x86 实跑）/ 🔧 引擎接线待构建 | `test_session_admit` 1/1 通过；`engine_ctx` 接线需 RK3588 |
| S-C 音频 ALSA 仲裁 | 🔧 已落地（x86 语法校验通过） | 运行时路由需 RK3588 |
| T7 平台服务 | ⬜ 未开始 | 需新建 `app/platform` |
| T8 跨节点 S4 | ⬜ 未开始 | 依赖 T7 + S-A |
| T9 退役/批量升级/容量 | ✅ 容量自上报(x86 实测) / ⬜ 退役·批量·压测 | `/api/v1/node/capacity` 已实测；其余待做 |

> 本次在 `DVS_REMAINING_TASKS.md` 基础上，把 T3/T4 的三个 S4 简化点单列如上，并补齐 T7–T9 文件级方案。代码侧已落地 S-B（session_admit）、S-C（audio_arbiter ALSA），其余留待硬件/平台联调。
