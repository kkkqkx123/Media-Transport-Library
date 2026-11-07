# DVS 平台 windows 布局 → 引擎 pip_stream 映射方案（下一阶段）

> 状态注记（2026-11-07）：本文已实施完毕，但其中两条结论被 `DVS_WINDOWS_TRANSLATION_GAPS_DESIGN.md` 修正后**以修正为准**：①下发形状不是本文 §2.1 的 `windows[{id,sig,x,y,w,h,z,muted}]+sources[{sig,sip,udpPort}]`，而是扁平分片 `windows[{windowId,sig,dst,src,fullW/fullH,partial,z,muted}]+endpoints[{sig,address,port,fps,audioPort,mode}]`（另见 2026-11-07 补的 `fps`/`audioPort` 端点表字段）；②音频归属不是本文 §3 的“非 muted 中 z 序最高者”，而是“最新未静音窗口”（req 03 §7.4），且线上钥为窗口标识、代理翻译为 `lane<N>`。③本文 §4.2 的 windows 放行兼容分支、§4.1-4 的“无 windows 字段纯操作帧”已作废（前者改显式拒绝，后者被契约 `windows` 必填取代）。按本文旧形状对接会整单被拒（查表全 miss），新对接方请以 GAPS 设计 §4 与本注记为准。

> 前置：`docs/plan/dvs/DVS_IPC_ASYNC_AUDIT_AND_FIX_PLAN.md`（payload 解包、hook 放行、pending_valid 已落地）。
> 对应代码：`app/dvs_agent/main.cpp`（/layout 转发）、`app/dvs_media/ipc/ipc_server.c`、`app/dvs_media/engine/{ipc_engine_bridge.c, layout_executor.c, engine_ctx.c}`。
> 状态：方案，未实施。

## 0. 背景与问题

平台侧布局以 **windows 形态** 表达：窗口 `{id, sig, x, y, w, h, z, muted}`，sig 是平台信号源编号；引擎可执行的布局是 **pip_stream 形态**：`{lane, sip, udp_port, audio_udp_port, width, height, fps, x, y, w, h, codestream_size}`（见 `layout_executor.c dvs_layout_from_json`）。

当前链路断在两处：

1. **形态不通**：`hook_prepare` 对 windows/geometry 形态只放行不解析（上一阶段修复了误清屏：无有效 pending 时 commit 保持画面），因此平台窗口几何**永远不生效**。
2. **语义不通**：sig→(sip, udp_port) 的绑定关系只存在于平台（sources 列表 `{id, name, ip, transMode}`、场景 layout 的 sig）；引擎的 `session_admit` 只认 `(mcast_ip, udp_port)`，不认识 sig。

## 1. 目标与非目标

**目标**

- 平台 windows 布局经 prepare/commit 后在引擎上真实生效：窗口按 z 序映射 lane，几何走 compositor fast path，applyAtNs 定时生效保留。
- 引擎不引入 sig 语义：sig→endpoint 的翻译在 agent 侧完成，引擎只见 `sip/udp_port`。
- 单节点子布局（平台已做像素拆分，节点不再二次分片，沿用 `main.cpp` 现有注释约束）。

**非目标（保持不变）**

- 不实现平台 WSS 通道；不更换 UDS + JSON 行协议。
- 不改动 `rx_compositor` 的 lane/渲染模型；不做跨节点屏障协议修改。
- 音频混音/多音轨仍是单 owner 模型（`dvs_engine_set_audio`）。

## 2. 方案总览：翻译发生在 agent，不在引擎

```
平台(拆分后子布局) ── HTTP /api/v1/node/media/layout ──▶ agent
agent: windows + 本节点 sig→endpoint 解析 ── IPC prepare(pip_stream 形态) ──▶ dvs_media
dvs_media: hook 校验 + 暂存 pending ── commit ──▶ engine apply(fast path / 重建)
```

**决策理由**：agent 是平台上配置（sources、bindings、场景）在本节点的持有者；引擎按设计保持"哑执行"（AGENTS.md：engine 只做 configure/prepare/commit 执行）。若把 sig 映射放进引擎，引擎就需要平台数据库镜像，跨进程状态同步成为新的一致性负担。

### 2.1 平台侧需补充分发字段

平台 `/node/media/layout` 下发体在现有 `{operationId, revision, applyAtNs, windows[], geometry}` 基础上，**增加 `sources` 数组**（本节点窗口涉及的信号源，而不是全量源，避免无关源泄漏到节点）：

```json
{
  "operationId": "op-xxx", "revision": 7, "applyAtNs": 1730000000000000000,
  "geometry": {"x": 0, "y": 0, "w": 1920, "h": 1080},
  "windows": [
    {"id": "w1", "sig": "s1", "x": 0,   "y": 0,   "w": 1280, "h": 720, "z": 1, "muted": false},
    {"id": "w2", "sig": "s3", "x": 1280, "y": 360, "w": 640,  "h": 360, "z": 2, "muted": true}
  ],
  "sources": [
    {"sig": "s1", "sip": "239.1.1.1", "udpPort": 20001},
    {"sig": "s3", "sip": "239.1.1.3", "udpPort": 20003}
  ]
}
```

`sip/udpPort` 来源即平台 sources 列表的 `ip` 与端口（单播/组播由 `transMode` 决定，传输层不变，引擎本来就按 `dvs_session_admit(sip, udp_port)` 接入）。

## 3. 字段映射规则

| window 字段 | pip_stream 字段 | 规则 |
|---|---|---|
| `sig` | `sip` + `udp_port` | 查 `sources` 表；查不到 → 整个 prepare 拒绝（见 §5 错误处理） |
| `z`（含窗口数组顺序） | `lane` | 按 `z` 升序（同 z 按数组序）稳定排序后依次取 lane 0..N-1；N ≤ `MP4_PIP_MAX_LANES`（复用现有 8-slot 上限） |
| `x,y` | `x,y` | 直接透传。注意：平台坐标是**拼墙全局坐标**，agent 拆分时已换算为本节点局部坐标——若平台当前下发的是全局坐标，agent 必须先减去本节点 `geometry.x/y` 再入 IPC（见 §4.1 待确认项） |
| `w,h` | `w,h` | 直接透传；compositor fast path 判定几何是否可原位重排 |
| `id` | — | 引擎不需要；仅用于 agent 日志与错误回带 |
| `muted` | 音频 owner | 非 muted 窗口中 z 序最高者作为 `audioOwner`（取其 sig）；全部 muted 或无窗口时 owner 为空。`audioVolume` 沿用场景 volume，agent 转发时随体下发 |
| — | `width/height/fps` | 源媒体参数，非窗口几何；沿用现有 `streams` 形态请求的字段，platform 下发的 sources 条目可选携带，未携带时引擎按现有默认探测 |
| — | `codestream_size` | 沿用 `streams` 形态的既有处理，windows 形态 V1 不下发 |

**语义保留**：`windows: []`（空数组 + 有 sources/operationId）= 显式空布局 → `pending_valid=1, count=0` → commit 清屏（与 `streams: []` 语义一致）。这区别于"无 windows 字段"（纯操作帧/白名单等，保持现画面）。

## 4. 修改点清单

### 4.1 agent（`app/dvs_agent/main.cpp` /layout handler）

1. 解析 `sources` 数组构建 `sig → (sip, udpPort)` 表；`windows` 中每个 sig 必须命中，未命中 → 400 `INVALID_ARGUMENT`（`"unresolved sig <x>"`），不转发 IPC。
2. 坐标换算：用本节点 `geometry` 把窗口坐标从拼墙全局换算为节点局部，并裁剪到本节点画布（部分越界窗口 clip；完全越界的窗口剔除且不占 lane）。
3. 转发体改为 **pip_stream 形态**：`{"operationId", "revision", "applyAtNs", "streams":[{lane,sip,udp_port,x,y,w,h}...], "audioOwner", "audioVolume"}`。agent 做完翻译后，引擎侧不再需要 windows 分支——IPC 协议收敛到单一形态，`hook_prepare` 的 windows 放行分支降级为遗留兼容（见 4.3）。
4. 空布局语义透传：平台明确 `windows: []` 时转发 `streams: []`；平台下发体无 `windows` 字段时转发纯操作帧（现状）。

> **待确认项（实施前与平台侧核对）**：平台当前下发的窗口坐标是全局还是已按节点拆分的局部坐标；`main.cpp` 注释称"节点不再二次分片"，若平台已做拆分则 4.1-2 只做防御性裁剪。

### 4.2 IPC/引擎（`app/dvs_media`）

**协议层（ipc_server.c）**：无需改动。prepare/commit/abort、幂等、事件广播全部复用；payload 解包、`pending_valid` 语义均已就绪。

**hook（ipc_engine_bridge.c）**：
1. `hook_prepare` 现有 streams/layout 分支即为目标路径，不动；windows 放行分支保留为兼容（旧 agent 未升级时仍 202 + 保持画面），注释标注 deprecated。
2. 无新增 hook。

**layout_executor.c**：
1. `dvs_layout_from_json` 已支持 `lane/sip/udp_port/x,y,w/h`，补两项：
   - `audioOwner` 解析（现仅 `audioOwner/audioVolume` 顶层键，已支持，确认即可）；
   - 校验收紧：`sip` 非法 IPv4 或 `udp_port` 越界 → 返回错误（防引擎收到不可接入的流）。`lane` 未携带时按数组序自动填 0..N-1（与 §3 z 序结果一致，agent 已排好序，这里只是兜底）。
2. 无需新 API：agent 已翻译为 streams 形态，`dvs_layout_prepare_from_json` 原样复用。

**engine_ctx.c**：无改动。几何-only 变更自动走 `rx_compositor_try_fast_apply`（不重建 decoder）；源增减走重建 + `dvs_session_release` 差量释放；applyAtNs 由 `engine_wait_until` 快路径消化。白名单准入复用 `dvs_session_admit`（window 的 sip/port 未在白名单时 commit 返回失败 → `layout apply failed` 500，状态机已处理该回退）。

## 5. 错误处理矩阵

| 场景 | 处理 | 返回 |
|---|---|---|
| window 的 sig 不在 sources 表 | agent 拒绝，不转发 | HTTP 400 INVALID_ARGUMENT |
| 窗口完全越出本节点画布 | agent 剔除该窗口 | 正常（日志记录） |
| 窗口部分越界 | agent clip | 正常 |
| windows 数 > MP4_PIP_MAX_LANES | agent 按 z 序截断前 N 个？否——**拒绝**（平台拆分器职责缺陷应显式暴露） | HTTP 400 |
| streams 的 sip/port 未过白名单 | commit 时 session_admit 拒绝 | IPC 500 INTERNAL "layout apply failed"（现有回退） |
| 旧 agent（未升级）发 windows 形态 | hook 放行，保持画面 | IPC 202/200（现兼容行为） |

## 6. 测试计划

1. **agent 单测**：sig 解析命中/未命中、坐标全局→局部换算、clip、全 muted 音频 owner 选择、windows 空数组 → `streams: []`。
2. **dvs_media 单测**（无 hook 模式，`test_ipc.c` 风格）：streams 形态 prepare→commit 全链路 revision/事件断言（windows 用例 9 保留，验证旧形态兼容不变）；`dvs_layout_from_json` 对 `lane` 缺省自动编号、非法 sip 拒绝。
3. **联调（编译服务器 + 双机）**：平台下发 2×2 拼墙含跨节点窗口 → 各节点仅收到本节点窗口与相关 sources → 几何变更走 fast path（日志确认未重建 decoder）→ `/events` 可观测 `layout_applied` + `state_changed`。

## 7. 实施顺序

1. 平台侧 `/node/media/layout` 下发体增加 `sources`（与平台服务联调排期对齐）。
2. agent /layout 翻译层（§4.1）+ agent 单测。
3. layout_executor 解析收紧与 lane 兜底（§4.2）+ dvs_media 单测。
4. 双机联调 + 文档回填（`docs/analysis/` 更新链路说明；若知识库与本实现不符，同改动修正知识库）。

## 8. 风险

- **坐标换算歧义**是最大风险点（§4.1 待确认项），实施第一步必须先冻结平台坐标语义，否则画面位置错排。
- lane 竞争：z 变化导致 lane 重排时，若源集合不变仅几何变化，fast path 依赖 compositor lane 与 sip/port 对应关系稳定；翻译层保证排序稳定（同 z 按数组序），避免同 prepare 两次下发产生不同 lane 分配。
- 旧/新 agent 与引擎混布期间，windows 放行分支保证前向兼容，但混布时平台窗口不会生效——升级顺序应先引擎后 agent（引擎向后兼容旧 streams-only agent 不受影响；agent 升级后引擎无需改动）。
