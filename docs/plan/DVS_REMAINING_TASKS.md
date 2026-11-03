# DVS 剩余任务修改方案（S0 收尾 / S1 引擎 / S2 收尾 / S3–S5）

> 本文基于 `DVS_FRONTEND_INTEGRATION_PLAN.md` 与 `DVS_S2_IMPLEMENTATION_PLAN.md` 的 S0–S2 段，
> 结合**实测代码状态**（非仅文档声明）整理剩余任务，给出文件级修改方案、验证方法与执行追踪。
> 代码基线：`feat/rk3588-apps` 分支。
> 生成日期：2026-11-03。

---

## 0. 现状复盘（实测结论）

| 阶段 | 文档声明 | 实测结果 |
|---|---|---|
| **S0** 公共库扩展 | "扩展 `rk3588_common`" | **未落地**。`rk3588_common/` 仍只有 A/B 档（ring_buffer/slab_pool/timer + 5 头文件）。`dvs_media/meson.build` 实际走文档允许的 fallback：直接把 `file_playout/shared/*.c`、`file_playout/rx/*.c` 编进 `dvs_shared`/`dvs_rx` 静态库。两 app 各自仍持有 `shared/` 副本。 |
| **S1** 引擎 + IPC | "引擎各模块实现" | **协议层完成、引擎层全空**。`ipc_server.c`（8 消息 + 信封 + 事件广播）与 `state_machine.c`（idle→prepared→committed + revision 幂等）真实可用且被单测覆盖。`engine/session_mgr.c`、`engine/layout_executor.c`、`engine/audio_arbiter.c`、`engine/capability.c` **均为空 stub**（`dvs_media_stub()`）。`dvs_media_main.c` 在 `--config` 模式仅 `sleep(1)` 退出，从不启动 RX 管线；`query_state` 在 IPC 层直接返回写死 `rxSessions:0, decoding:0, layers:0, ptp:unknown/0`。 |
| **S2** 代理 + node-admin | "实现完成（21/21 单测）" | **agent 层基本完成**：`node_store.cpp`（PBKDF2+legacy 迁移）、`registration.cpp`（空 URL 本地自治 / 非空注册心跳）、`main.cpp`（登录限速、改密、六 scope 路由、IPC 桥、全部节点/媒体/维护路由、静态托管）、`maintenance.cpp`（reboot/factory_reset/upgrade 语义）、`web/src/real-api.js` + `packs/node-admin/node-admin.html:517` 已激活。**偏差**：`main.cpp:188` 严格登录态校验被注释；改密接口硬编码用户 `admin`。媒体态来自 S1 stub，故"六 zone 真实反映 dvs-media"仅部分成立。 |

**根因**：S1 引擎未接线，导致 S2 端到端无法真正驱动媒体。下一步最高优先是补齐 S1 引擎与接线。

---

## 1. 任务清单与优先级

| ID | 任务 | 阶段 | 优先级 | 验证环境 |
|---|---|---|---|---|
| T1 | `engine/capability.c` 真实能力探测（outputPorts/maxDecodeSessions/maxLayers/ptp/mtl） | S1 | P0 | 需 RK3588（libdrm/MPP/MTL 链接） |
| T2 | `ipc_server.c` `query_state` 改为调用真实能力/状态提供者 | S1 | P0 | 需 RK3588 |
| T3 | `dvs_media_main.c` `--config` 真正启动媒体管线（抽取 `file_playout_mtl_init`/`rx_main_loop` 为库） | S1 | P0 | 需 RK3588 |
| T4 | 实现 `session_mgr`/`layout_executor`（两段式 prepare/commit + 影子回滚）/`audio_arbiter`，由 `rx_compositor` 演进 | S1 | P0 | 需 RK3588 |
| T5 | S0 文档回写：`AGENTS.md` + `docs/app/README.md` 共享代码节 | S0 | P1 | 文档（x86 可审） |
| T6 | `dvs_agent` 登录态严格化（env 门控）+ 改密接口用户参数化 | S2 | P2 | **x86 可构建+单测** |
| T7 | S3 平台服务 `platform/` + video-control-platform 接入 | S3 | P3 | x86（C++17） |
| T8 | S4 跨节点 prepare/commit + `apply_at` 帧对齐 + 回滚 + 跨格 | S4 | P3 | 需多 RK3588 |
| T9 | S5 退役/墓碑/批量升级/压测/三处文档回写 | S5 | P3 | 需多 RK3588 |

> x86 本机（当前环境）仅 `dvs_agent` 可构建（json-c + vendored SQLite）；`dvs_media` 因 `rockchip_mpp` 缺失会被 `meson` 直接 `error()`，故 T1–T4 只能在 RK3588/devs 服务器构建验证。

---

## 2. 各任务文件级修改方案

### T1 — `engine/capability.c` 真实能力探测（P0）

**现状**：空 stub。
**目标**：实现 req 03 §12 注册能力 JSON 的运行时探测，替代 `query_state` 写死零值。

新增函数（头文件 `engine/capability.h`）：
- `int dvs_capability_probe(dvs_capability* cap);` — 探测并填充：
  - `outputPorts`：枚举 DRM connector（`drmModeGetResources`/`drmModeGetConnector`），取 `safeModes`（由 `shared/drm_mode.c` 的 mode 枚举推导）。
  - `maxLayers`：DRM plane 数（`drmModeGetPlaneResources`）。
  - `maxDecodeSessions`：MPP 实测解码并发上限（S1 末填值；探测失败时填保守值，如 4）。
  - `ptp{state, offsetUs}`：封装 `shared/ptp_time.c` 的 `mp4_ptp_read_tai_ns` + 新增 MTL PTP portState 读取（`mtl_get_port_stats`）。
  - `mtl{version, backend}`：`mtl_version()` / 编译期 backend 宏。
- `char* dvs_capability_to_json(const dvs_capability* cap);` — 序列化为 `capability` 报文（调用方 `json_object_put` 释放）。
- `int dvs_capability_state_json(char* buf, size_t n, int rx_sessions, int decoding, int layers, const dvs_capability* cap);` — 供 `query_state` 使用（live 计数由引擎运行时填充，未运行时为 0，属真实状态）。

**实现要点**：所有探测以"失败降级"为原则——任一子系统（DRM/MPP/MTL）不可用时填默认值并记 `warn()`，绝不 `abort`；保证 `dvs_media` 在缺模块时仍能起 IPC 服务并上报部分能力。

**验证**：RK3588 单板 `dvs_media --config cfg.json` 启动后，经 agent `/api/v1/node/capability` 或直连 IPC `query_state` 确认 `outputPorts`/`maxLayers`/`ptp` 为真实枚举值。

### T2 — `ipc_server.c` `query_state` 真实化（P0）

**现状**：`handle_line` 中 `query_state` 分支写死 `rxSessions:0, decoding:0, layers:0, ptp:unknown`。
**修改（已落地，未构建）**：
- 在 `ipc_server.c` 引入 `engine/capability.h`，新增全局 `dvs_capability g_cap`；`dvs_ipc_server_run` 启动时调用 `dvs_capability_probe(&g_cap)`。
- `query_state` 改为从 `g_cap` 取真实 `ptp{state,offsetUs}`，媒体计数 `rxSessions/decoding/layers` 来自新增全局 `g_rx_sessions/g_decoding/g_layers`（默认 0，由引擎经 `dvs_ipc_set_media_counts()` 在媒体启动后刷新）。
- 新增 `dvs_ipc_set_media_counts()`（`ipc_server.h` 导出）供 T4 引擎回填实时计数。
- 注：未采用"给 `dvs_ipc_server_run` 加引擎上下文参数"的原始设想，改为全局能力 + setter，以避免改动 `dvs_media_main.c` 调用点带来的额外风险；引擎上下文（T3/T4）仍通过 setter 注入实时数据。

**验证**：x86 不可构建 dvs_media；RK3588 上 `tests/mock_client.py` 发 `query_state` 得真实 JSON（`ptp` 为探测值，媒体计数为引擎回填）。

### T3 — `dvs_media_main.c` `--config` 真正启动管线（P0，已落地）

**现状**：`--config` 仅 `file_playout_config_load` + `sleep(1)` 退出，无媒体。
**关键发现**：`file_playout_mtl_init` 实际定义在 `file_playout_config.c`（已被 `dvs_media` 链接），无需抽取 `file_playout_run.c`——故直接复用即可，不改动 `file_playout` 侧代码。
**修改方案（已落地）**：
1. 新增 `engine/engine_ctx.c/.h`：封装 MTL 实例 + 单个 `rx_compositor`，提供 `create/destroy/start/stop/apply_layout` 与后台 run-loop 线程（复用 `run_compositor` 的字段映射与 `file_playout_mtl_init`/`file_playout_mode_dims`/`file_playout_pip_validate`/`file_playout_apply_drm_mode`）。
2. `dvs_media_main.c`：`--config` 分支 `dvs_engine_start` 真正出画（test_time 内阻塞，SIGINT/SIGTERM 优雅退出）；`--ipc-sock` 分支先 `dvs_engine` 起引擎（有初始布局则直接应用，否则仅 init MTL），再 `dvs_ipc_server_run`。
3. IPC 与引擎经 `dvs_ipc_engine_hooks_t` 解耦（`engine/ipc_engine_bridge.c` 仅编译进真实二进制），no-MTL 单测不受影响。

**验证**：RK3588 单板 `dvs_media --config cfg.json` 实测出画（沿用 file_playout RX 验证脚本）；x86 已对 no-MTL 测试路径做语法校验。

### T4 — `session_mgr` / `layout_executor` / `audio_arbiter` 真实实现（P0，已落地基线）

**现状**：三个文件原为全空 stub。
**修改方案（已落地，recreate-on-commit 基线——非帧边界原子切换）**：
- `session_mgr.c`：引擎生命周期 owner，`dvs_session_mgr_create/destroy/apply_layout/request_stop` 委托 `engine_ctx`。
- `layout_executor.c`：两段式 `prepare`（校验 + 暂存 shadow 布局，不触碰运行中的 compositor）/ `commit`（经引擎 recreate compositor 应用布局）/ `abort`（丢弃 pending）/ `clear`（移除媒體）；`dvs_layout_from_json` 解析 `streams`/`layout` JSON 数组（camelCase/snake 兼容）。
- `audio_arbiter.c`：`dvs_audio_set(owner, volume)` 作为音频路由入口（完整 ALSA 仲裁留 S4）。
- **已知简化**：当前 commit = stop+free+create+start 重建 compositor（正确但非帧边界原子）；真正的 `apply_at` 帧对齐 + rollback（S4）未做。per-session MTL 会话/LRU 白名单（req 03 §7.3）亦为 S4 细化项。

**验证**：RK3588 单板 4 路 1080p60 同时显示（recreate 切换会有短暂黑帧）；IPC `configure→commit` 热更新布局无需重启进程；`tests/mock_client.py` 驱动 8 条消息（状态机路径已单测通过）。

### T5 — S0 文档回写（P1，x86 可审）

- `AGENTS.md` 第 13–14 行 `app/dvs_media/`、`app/dvs_agent/` 说明补充：S0 实际采用 fallback（`dvs_media` 直接消费 `file_playout/shared`、`rk3588_common` 仅 A/B 档）；dvs_agent 选型要点（C++17 / 自研 POSIX 网络栈 / PBKDF2 / vendored SQLite / 平台 stub 空 URL 自治）。
- `AGENTS.md` Gotchas 增加：dvs_media 构建需 `rockchip_mpp`（x86 不可构建，仅 RK3588/devs）。
- `docs/app/README.md` 第 22–23 行"共享代码"节更正：dvs_media 已通过 meson 直接复用 `file_playout/shared` 与 `rk3588_common`（A/B），公共库抽取尚未完成。

### T6 — `dvs_agent` 登录态与改密修正（P2，x86 可构建+单测）

- `main.cpp` 改密接口（`POST /api/v1/node/password`）：将硬编码 `verify_password("admin", oldp)` 改为取 `body.username`（缺省 `admin`），与登录接口一致。
- 严格登录态：将 `main.cpp:188` 注释块改为受 `DVS_REQUIRE_AUTH=1` 环境变量门控——默认保持原型宽松（匿名可读 `/api` 便于前端），置 1 时强制 401。避免破坏 node-admin 原型联调。
- 补 `tests/test_basic.cpp` 用例：改密接口用户参数化（若需）。

**验证**：`cd app/dvs_agent && meson setup build && ninja -C build && meson test -C build --print-errorlogs`（x86 全绿）。

### T7 / T8 / T9 — S3/S4/S5（P3）

按 `DVS_FRONTEND_INTEGRATION_PLAN.md` 既有细化为准，新增 `platform/` 顶层工程（C++17）、跨节点 operation 框架、`apply_at` 帧对齐、退役/墓碑/批量升级/压测。本阶段仅规划，不在本次执行范围。

---

## 3. 验证矩阵

| 任务 | x86 可验证 | 需 RK3588 |
|---|---|---|
| T1 capability 探测 | 否（需 libdrm/MPP/MTL 链接） | 是 |
| T2 query_state 真实化 | 否 | 是 |
| T3 --config 启动管线 | 否 | 是 |
| T4 引擎三模块 | 否 | 是 |
| T5 文档回写 | 是（评审） | — |
| T6 agent 登录/改密 | **是（构建+单测）** | — |
| T7/T8/T9 | S7 部分(x86 C++17) | S8/S9 需硬件 |

---

## 4. 执行状态追踪

| ID | 状态 | 说明 |
|---|---|---|
| T5 | ✅ 已执行 | AGENTS.md + docs/app/README.md 已回写（x86 可审） |
| T6 | ✅ 已执行+验证 | agent 改密用户参数化 + 登录态 env 门控；x86 构建+单测通过（1/1 OK） |
| T1 | 🔧 已落地代码（x86 语法校验通过，未链接构建） | `engine/capability.{h,c}` 真实探测（DRM/MPP/MTL 失败降级）+ `dvs_media_main.c` 启动打印 |
| T2 | 🔧 已落地代码（x86 语法校验通过，未链接构建） | `ipc_server.c` query_state 经 hook 取真实能力；`dvs_ipc_install_hooks` 解耦 |
| T3 | 🔧 已落地代码（x86 语法校验通过，未链接构建） | `engine/engine_ctx.c` 复用 `file_playout_mtl_init`+`rx_compositor`；`--config` 真正出画；`--ipc-sock` 起引擎 |
| T4 | 🔧 已落地代码（x86 语法校验通过，未链接构建） | `session_mgr/layout_executor/audio_arbiter` 为真实模块（recreate-on-commit 基线）；帧边界原子切换留 S4 |
| S-A 帧边界原子切换 | ⬜ 规划中 | 需改共享 `rx_compositor`，硬件联调 |
| S-B 白名单/LRU 准入 | ✅ 逻辑已单测(x86) / 🔧 引擎接线待构建 | `engine/session_admit.{h,c}` + `test_session_admit` 1/1 通过；`engine_ctx` 接线 |
| S-C 音频 ALSA 仲裁 | 🔧 已落地（x86 语法校验通过） | `audio_arbiter.c` 真实 ALSA 路由 |
| T7–T8 | ⬜ 规划中 | 平台服务/跨节点，详见专项方案文档 |
| T9 | ✅ 容量自上报(x86 实测) / ⬜ 其余 | `/api/v1/node/capacity` 已实现并实测；退役/批量/压测待做 |

> 标记说明：✅ 已验证 / 🔧 代码已写但本机(x86, 缺 MPP/MTL)无法链接构建验证（已做 no-MTL 路径语法校验）/ ⬜ 未开始。
>
> **专项方案**：T3/T4 的三个 S4 简化点（S-A/S-B/S-C）与 T7–T9 的文件级修改方案、风险与验证环境，见 [`DVS_SIMPLIFIED_AND_REMAINING.md`](./DVS_SIMPLIFIED_AND_REMAINING.md)。

> 设计要点：IPC 服务与引擎通过 `dvs_ipc_engine_hooks_t` 解耦（`engine/ipc_engine_bridge.c` 仅编译进真实二进制），故 no-MTL 的 `test_ipc` 单测仍可独立构建，不引入 MTL/MPP 依赖。
