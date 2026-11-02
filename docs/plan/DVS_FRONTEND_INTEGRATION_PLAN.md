# DVS 前端原型集成:分阶段修改方案

> 上游设计:`docs/app/frontend_prototype_integration.md`(原型↔需求对应关系与集成架构)。
> 本文把它拆成可执行的修改步骤:每个阶段列出**改哪些文件/目录、新增什么、退出标准、验证方法**。
> 阶段编号对齐 `docs/req/05-实施路线与验证.md` 的 P0–P5;工期为相对估算(1 人)。
> 状态:draft(已对照当前代码修订;代码基线为 `feat/rk3588-apps` 分支,公共库现状见 `docs/plan/RK3588_SHARED_CODE_EXTRACTION.md`)。

## 总览

```text
P0 基座验证 (req 05 §2,前置,不在本文展开)
S0 公共库扩展(app/rk3588_common)       2 周
S1 dvs-media 引擎 + IPC             5 周   ← app 层改造主体
S2 dvs-agent 节点代理 + node-admin  3 周   ← 原型1接入
S3 平台服务 + platform 原型接入     5 周   ← 原型2接入
S4 同帧与跨格(prepare/commit)      4 周
S5 运维与规模化(退役/升级/压测)    3 周
```

依赖:S1 依赖 S0;S2 依赖 S1 的 IPC;S3 与 S2 可并行(用代理 stub);S4 依赖 S1+S3;S5 依赖 S3。

每个新目录都是**独立 meson 工程**,不接入 MTL 顶层构建(与现有两个 rk3588 app 同策略)。

代码基线说明:两 rk3588 app 已经以 `app/rk3588_common/` 为公共库唯一事实来源,但只完成 A/B 档迁移(ring_buffer、slab_pool、timer、codec_plugin.h、pipeline_common.h)。C 档(drm_renderer、mpp_decoder)与 D 档(drm_mode、display_layout、ptp_time 等)仍保持两份/单侧独有;S0 的首要任务是决定并执行 dvs-media 所需的扩展迁移,而不是新建 `app/common/`。

---

## S0 公共库扩展(2 周)

**目标**:在现有 `app/rk3588_common/` 基础上补足 dvs-media 所需的复用模块,消除 end2end 与 mp4_dist 的重复实现。不改任何运行行为。

### 当前公共库状态(2026-11-02 实测)

| 档位 | 模块 | 状态 |
| --- | --- | --- |
| A/B | `ring_buffer`、`slab_pool`、`timer`、`codec_plugin.h`、`pipeline_common.h` | 已在 `app/rk3588_common/`,两 app 本地拷贝已删除 |
| C | `drm_renderer`、`mpp_decoder` | 未合并(接口级分叉,mp4_dist 版为超集) |
| D | `drm_mode`、`display_layout`、`ptp_time`、`e2e_stamp`、`tx_tai_ts`、`au_trace`、`rx_e2e_stats` | 仅 mp4_dist 侧,留在 `shared/` |
| end2end 增量 | `dma_allocator`、`dma32_allocator`、`sei_timestamp`、`rga_converter`、`mpp_encoder`、`slice_reassembler`、`mtl_rx_bridge` | 仅 end2end 侧 |

### 修改项

1. **扩展 `app/rk3588_common/`(不新建 `app/common/`)**:
   - C 档(`drm_renderer`、`mpp_decoder`)因 dvs-media 成为第三个消费方,满足合并触发条件;以 mp4_dist 接口为基准重设计, end2end 侧做适配层。若评审决定暂不合并,则 dvs-media 直接消费 `app/rk3588_mp4_dist/shared/` 副本,并接受与 mp4_dist 的耦合。
   - D 档中 dvs-media 必需的模块(`drm_mode`、`display_layout`、`ptp_time`、`tx_tai_ts`、`e2e_stamp`)迁入 common;统计/追踪类(`au_trace`、`rx_e2e_stats`)可选,按需迁移。
   - end2end 增量中 dvs-media 需要的模块(`dma_allocator`、`dma32_allocator`、`sei_timestamp`、`rga_converter`、`mpp_encoder`、`slice_reassembler`、`mtl_rx_bridge`)迁入 common;`mtl_rx_bridge` 合并为参数化回调版本,供两 app 与 dvs-media 共用。
2. `app/rk3588_end2end/meson.build` 与 `app/rk3588_mp4_dist/meson.build`:删除被迁入 common 的源文件条目,改为引用 `../rk3588_common` 的源文件与 include 目录(现有消费方式:`include_directories('../rk3588_common/include', '../rk3588_common/include/rk3588_common')` + 直接 `files('../rk3588_common/src/...')`),**不使用** `dependency('dvs_common')`/pkg-config。
3. `codec_plugin.h` 已在 common 完成并集(A/B 档),无需再合并;end2end 的 `codec/mpp_decoder.c` 仅在 C 档合并完成后删除,统一使用 common 版本。
4. `mtl_rx_bridge.c` 两套(`end2end/mtl_rx_bridge.c` ≈ `mp4_dist/rx/mtl_rx_bridge.c`)合并入 common(桥接线程参数化为回调)。
5. 头文件路径统一为 `rk3588_common/xxx.h`(或沿用现有无前缀 include 目录),**不使用** `dvs/xxx.h`。

### 注意

- 平移用 `git mv` 保留历史;**不重写接口**,只解决头文件路径与 meson 条目。
- end2end 的 `io/drm_renderer.c`(单层)保留不动——它是单流路径,多 plane 的 common 版本供 dvs-media 用,S5 再决定是否废弃。
- common 中 `mtl` 为可选依赖,保持 MTL 可用/不可用两种构建配置。
- 公共库建成后回写 `docs/app/README.md` 的"共享代码"一节(当前"当前未抽取公共库"的描述已过时)与 `AGENTS.md` Gotchas。

### 退出标准

- 两个现有 app 用 common 库编译通过、各自单测全绿、单板链路回归正常(end2end 单板回环、mp4_dist 单板播放)。
- 无行为 diff:日志/统计输出与改造前一致。
- dvs-media 能直接包含 common 头文件并链接 common 静态库。

---

## S1 节点媒体引擎 dvs-media + IPC(5 周)

**目标**:把 mp4_dist 的 RX 合成管线改造成可运行期控制的媒体引擎,支持 IPC 消息集与能力上报。对应 req 05 P1。

### 新目录 `app/dvs_media/`

```text
app/dvs_media/
  meson.build            # 依赖 rk3588_common、mtl pkg-config、json-c
  dvs_media_main.c       # 入口:--config(静态兼容) / --ipc-sock(受控)
  engine/
    session_mgr.c        # MTL ST22/ST30 会话生命周期,引用计数
    layout_executor.c    # 布局片段 → 影子布局 → 原子提交 DRM plane
    audio_arbiter.c      # audioOwner + volume → ALSA(改自 mp4_dist rx/mtl_rx_audio.c)
    capability.c         # 能力探测与 JSON 上报
  ipc/
    ipc_server.c         # Unix domain socket,JSON 行协议
    state_machine.c      # idle → prepared → committed;revision 幂等
  tests/                 # 布局解析、状态机、能力 JSON 的单测(注册进 meson test())
```

### 修改项(文件级)

1. **`session_mgr.c`**(新写):封装 `st22p_rx_create/destroy`、`st30p_rx_create/destroy`;lane 与 session 解耦,信号按 `(mcast_ip, port)` 唯一;白名单内常驻订阅、上限 8 路 1080p60、LRU 淘汰(req 03 §7.3)。动态换源走 `st22p_rx_update_source`。
2. **`layout_executor.c`**(由 `mp4_dist/rx/rx_compositor.c` 演进,重点改三处):
   - `rx_compositor_create(:401)` 的"一次性 resolve lane→plane"拆成两段:`prepare()` 预建/复用 plane、framebuffer、解码器(保持当前画面),`commit()` 只切 plane 属性(src_crop/dst/z/visible)。
   - commit 前把当前布局存影子副本;commit 失败或 `abort` 时从副本恢复(rollback)。
   - lane 数上限沿用 `MP4_PIP_MAX_LANES=8`,但去掉 `main` 必须唯一等 MP4 场景约束,校验规则改为 req 06 §2.4 的 layout 片段校验(矩形在 canvas 内、src 裁剪在源内)。
   - 复用 `display_layout_from_pip_streams(:87)` 的解析,新增"整墙像素坐标 + 节点片段"入口(接收平台算好的 `{canvas, planes[], audioOwner, volume, baseImage}`,节点不再自行取整)。
3. **`capability.c`**(新写):探测并输出 req 03 §12 注册能力 JSON——`outputPorts`(从 DRM connector 枚举 mode 得 safeModes)、`maxDecodeSessions`(MPP 实测,S1 末填值)、`maxLayers`(DRM plane 数)、`ptp{state,offsetUs}`(封装 `shared/ptp_time.c` 的 `mp4_ptp_read_tai_ns` + 新增 MTL PTP 状态查询)、`mtl{version,backend}`。硬编码的 `rk3588-reference-hdmi` profile 字段全部改为运行时探测结果。
4. **`ipc_server.c`**(新写):`configure/prepare/commit/abort/set_sources/clear/query_state/event` 八条消息(JSON 行,请求带 `messageId`,响应带同 id),按 req 04 §3 消息集;`clear` 实现"全 plane 不可见 + 保留底图 plane",幂等。
5. **`dvs_media_main.c`**(新写):两种模式。`--config` 模式直接复用 `mp4_dist_config.c` 的 JSON 加载(`app/rk3588_mp4_dist/mp4_dist_config.c:72-286` 抽入 common 或复制),保证现有联调脚本零改动;`--ipc-sock` 模式启动后处于 idle,等 configure。
6. **PTP 状态查询**:在 `dvs_media` 内封装 `mp4_ptp_log_status` 的查询逻辑为周期采样接口(`mp4_ptp_read_tai_ns` 已有,补 `mtl_get_port_stats`/PTP portState 读取),供心跳上报 offsetUs。

### 退出标准(对齐 req 05 §3)

- 单节点 4 路 1080p60 同时显示;窗口通过 IPC 增删、移动、缩放、调层序,无需重启进程。
- prepare/commit 在帧边界切换;abort 回滚到影子布局。
- 音频跟随 audioOwner,静音/音量生效;多路竞争时节点侧仲裁。
- 引擎 kill 后由 `--config` 重启并恢复最后布局(本地持久化最后 committed 布局)。
- `query_state` 返回结构化 JSON(会话、解码、图层、PTP、统计),替代 stdout 文本(`rx_compositor.c:595-641` 保留为日志,不再是唯一出口)。
- mock 客户端(`tests/mock_client.py`,stdin/stdout 发 JSON 行)可驱动全部八条消息。

### 验证方法

- 单测:布局片段校验、状态机迁移、revision 幂等(旧 revision 拒绝)。
- 集成:RK3588 单板,`mp4_dist_tx`(x86 可跑)发流 + `dvs_media --ipc-sock`,mock 客户端依次 configure→prepare→commit,示波器/DRM 日志确认帧边界切换。

---

## S2 节点代理 dvs-agent + node-admin 原型接入(3 周)

**目标**:节点侧具备注册、心跳、配置状态机、本机页。对应 req 05 P4 的节点部分。

### 新目录 `app/dvs_agent/`

```text
app/dvs_agent/
  meson.build            # 独立 meson 工程 (C++17);依赖 json-c、SQLite amalgamation、threads
  main.cpp               # 主程序 (HTTP 服务 + 路由)
  node_store.h/.cpp      # 节点状态与六 scope 配置状态机 (SQLite kv)
  http_server.h/.cpp     # 轻量 HTTP 服务 (POSIX socket)
  media_ipc.h/.cpp       # dvs-media Unix socket 客户端
  maintenance.h/.cpp     # 重启/恢复出厂/升级
  third_party/sqlite/    # SQLite amalgamation (源自 zlmediakit 3rdpart)
  web/                   # 托管 node-device-admin 前端静态文件
  tests/
```

### 修改项

1. **配置状态机**(`config_sm.py`):六 scope(role/input/output/network/registration/sip),每 scope 维护 `desired/applied` 双 revision、`state ∈ {idle, pending_apply, applied, failed}`、`operationId/errorCode`——字段与原型 `node-state.js` 的 `configStates/configRevision` 完全一致,API 返回结构与原型 `mock-api.js` 的 `GET /api/v1/node/config/:scope` 对齐。落库到本地 SQLite(替代 localStorage)。
2. **注册与心跳**(`registration.py`):按 req 06 §2.2/§2.3 实现 `node.register` / `node.heartbeat`(WS 或 HTTP 轮询,S2 用 HTTP+stub 平台,S3 换 WS);心跳携 capabilities、媒体统计(经 IPC `query_state` 取)、PTP offset;3s 间隔、3 次超时判离线。
3. **维护操作**(`maintenance.py`):重启(有 pendingRole 则一并生效)、恢复出厂(保留网络与注册地址)、固件上传校验(SHA-256 + 清单,A/B 写入失败保留旧版本)。对应 node-admin 的 zone-ops 四个按钮。
4. **本机 HTTP**:实现原型 `mock-api.js` 已定义的节点路由,路径与原型完全一致:`/api/v1/node/login`、`/api/v1/node/info`、`/api/v1/node/role`、`/api/v1/node/config/:scope`(+retry)、`/api/v1/nodes`、`/api/v1/nodes/heartbeat`、`/api/v1/nodes/upgrade`;静态托管 `web/`。媒体相关端点(`/api/v1/node/media/status`、`/media/sources`、`/media/logs`、`/capability`)为 DVS 新增,原型 mock 中不存在,需在 S2 扩展 API 契约并同步更新 mock。
5. **前端接入**:`ref/node-device-admin/` 复制为 `app/dvs_agent/web/src/`(原型只读保留在 ref/,不修改);改造点收敛为一个文件——新增 `web/src/real-api.js` 实现 mock-api.js 的同名 `DvsMockApi.request` 接口,内部用真实 fetch + session cookie;页面代码零改动。登录态、密码哈希(Argon2)、登录限速按 req 03 §9.5。
6. **IPC 桥**:代理把 role/input/output 变更经 IPC `configure` 下发给 dvs-media;`query_state` 轮询供心跳与 `/node/media/status`。

### 退出标准

- 浏览器打开节点本机页(admin/首次改密),六个 zone 全部可操作且状态真实反映 dvs-media 与系统(不再是 localStorage 模拟)。
- 角色切换走"影响检查→平台清理→待生效→重启→新角色注册"(S2 期间平台用 stub)。
- 断网心跳超时→代理重连→上报 applied revision;断连期间 dvs-media 出画不中断。
- 升级模拟包校验失败时版本不变、可重试。

---

## S3 平台服务 dvs-platform + platform 原型接入(5 周,可与 S2 并行)

**目标**:实现平台 REST + 节点网关 + operation 框架,video-control-platform 原型接真实 API,走通 FLOW-01。对应 req 05 P2。

### 新目录 `platform/`(仓库顶层,独立于 app/)

```text
platform/
  dvs_platform/          # 建议先 Python FastAPI(原型验证),与 agent 同语言
    api/                 # REST:groups / walls / scenes / nodes / playback-tasks / audit
    ops/                 # operation 服务:revision、expires_at、回执聚合、expiry sweeper
    gateway.py           # 节点网关:WS,注册/心跳/媒体消息
    layout.py            # 像素坐标计算:cell_grid 派生、窗口→节点片段拆分、统一取整函数
    models.py            # 数据模型(SQLAlchemy):dvs_group/wall/scene/binding/whitelist/...
  web/                   # video-control-platform 原型改造副本
  tests/                 # pytest:API 契约 + 节点模拟器
```

### 修改项

1. **数据模型**(`models.py`):按 req 06 §1 建表;字段与原型 store 对齐处直接用原型命名(`rowPixels/colPixels` 派生 `cell_grid`,存储层用像素坐标)。节点表含 `configStates`、`capabilities` JSON 快照、`currentPlayback` 投影。
2. **REST API**:实现原型 `mock-api.js` 已定义的节点/播放任务路由(`GET/POST /api/v1/nodes*`、`/api/v1/nodes/heartbeat`、`/api/v1/nodes/upgrade`、`/api/v1/playback-tasks*`、`/api/v1/audit-events`),响应包 `{ok,status,code,msg,data,requestId}`、`Idempotency-Key`、审计事件。**保留 mock-api.js 作为契约规格**,变更需先改 mock 再改实现。错误码补 req 06 §4 媒体项。分组/拼墙/场景/绑定/白名单/设置/底图当前由原型 `platform-store.js` 在浏览器 localStorage 中维护,平台化时需为其新增 REST(原型 mock 中没有),以 req 06 §1 数据模型为基线。
3. **布局计算**(`layout.py`):格位快照 `{sig,c,r,cs,rs}` ⇄ 像素窗口换算;窗口→每节点片段拆分(交集、src 映射、左上下取整/右下上取整,req 04 §5.3);此模块版本化,节点不自行取整。
4. **节点网关**(`gateway.py`):WS 接收 S2 代理;`media.prepare/commit/abort/rollback` 消息按 req 06 §2.4–2.6;心跳聚合在线状态。
5. **前端接入**:`ref/video-control-platform/` 复制为 `platform/web/src/`;同 S2 策略,新增 `real-api.js` 替换 `DvsMockApi.request`,页面零改动。底图存储从 IndexedDB 改为平台对象存储/文件服务(上传接口新增)。
6. **operation 最小闭环**:S3 先实现"单节点窗口立即生效 + 离线保存待生效 revision";跨节点 prepare/commit 留 S4。

### 退出标准(req 05 §4)

- 两节点 2×1 墙走通 FLOW-01:建组→建墙→绑定→白名单→开窗→保存场景,全部真实节点出画。
- 场景切换/清屏符合 REQ-0005/0006/0007 的 P0 规则(单节点);非法参数、冲突绑定返回统一错误码。
- 节点模拟器(模拟 128 代理的注册/心跳)下 API P95 ≤ 300 ms。

---

## S4 同帧与跨格(4 周)

**目标**:跨节点 prepare/commit、统一 apply_at、跨格窗口、回滚。对应 req 05 P3,最高风险。

### 修改项

1. **平台 `ops/`**:prepare 屏障(全部 ready 才 commit)、`apply_at = now + max(2/fps, 50ms)`、回执聚合、失败 rollback(新 operation,目标 revision-1)、时钟资格判定(PTP synced、偏差 ≤2ms、epoch/fps 一致,req 03 §12.3)。
2. **dvs-media `layout_executor.c`**:S1 已有两段式提交,S4 补——`apply_at` TAI → 目标帧号 N(`epoch_tai_ns`+`fps` 全局帧号),在帧 N-1 的 vblank 上原子切 plane;回执带 `frameIndex/actualTaiNs/layoutHash`;提交路径只切 plane 属性,禁止解码器重建。
3. **跨格**:平台用 `layout.py` 拆片段后正常走 prepare/commit;`layout_executor` 支持一个窗口在本节点多格位的多个片段 plane。接缝用 2×2 物理墙实测校准,overscan 校准值作为节点配置。
4. **底图下发**:平台按节点格位并集拆子图,prepare 的 `layout.baseImage{revision,url}` 引用;节点拉取解码为最底层 plane;普通 revision 异步生效。
5. **常驻订阅调优**:白名单内常驻 + LRU 淘汰(S1 实现),S4 按"1000 次场景切换同帧偏差 ≤1 帧"实测调 GOP(0.5–1s)与保护带。

### 退出标准(req 05 §6)

- 2×2 物理墙跨格窗口无缝;场景切换 1000 次,同帧偏差 ≤1 帧(示波器/摄像头拍摄验证)。
- 任一节点时钟未同步/离线→整墙 blocked_offline,原场景保持。
- commit 失败自动 rollback,无混合场景画面。
- 关键帧等待:已订阅信号切换 P95 ≤ 1s(NFR-03)。

---

## S5 运维与规模化(3 周)

**目标**:设备管理闭环、升级、容量压测、文档回写。对应 req 05 P4/P5。

### 修改项

1. **退役/墓碑**:`platform` 实现 retire(在线先清绑定/窗口/白名单并等回执)、30 天墓碑防重复注册;device-list 页与 `dvs_agent` 联动。
2. **批量升级**:平台下发(S2 的 maintenance 承接),V1.0 串行并发 1、失败隔离;升级进度事件经网关回传。
3. **播放任务对接**:task 创建→校验→执行→playing 投影全链路接真实 operation(S3 已建状态机);`dvs_playback_task` 补 `operation_id/target_node_id`。
4. **统计落库**:`dvs_media` 的 `query_state` 经心跳聚合写入 `dvs_media_session` 投影表(排障用,可选)。
5. **代码收尾**:评估 end2end 单层 `io/drm_renderer.c` 是否切到 common 多 plane 版本;`AGENTS.md`/`docs/app/README.md`/本文状态从 draft 更新。
6. **压测**:节点模拟器 128 节点注册/心跳/operation 聚合;4 台 RK3588 扩 4×4 墙回归 S4 指标。

### 退出标准

- 退役→墓碑→重连注册为既有设备;批量升级失败节点不影响其余。
- 128 节点模拟压测通过 req 05 §9 NFR 基线;4×4 墙同帧达标。
- 文档三处(docs/app、docs/req 索引、AGENTS.md Gotchas)与代码一致。

---

## 验证矩阵汇总

| 阶段 | 无硬件可验 | 需 RK3588 硬件 |
| --- | --- | --- |
| S0 | 两 app 编译 + 单测 | 单板链路回归 |
| S1 | 状态机/布局单测、mock 客户端 | 4 窗口热更新、帧边界切换 |
| S2 | API 契约 pytest、页面 mock 关闭 | 真实注册心跳、断网保画 |
| S3 | 节点模拟器 128 代理 | FLOW-01 两节点实测 |
| S4 | 拆分算法单测(取整/交集) | 2×2 墙同帧 1000 次 |
| S5 | 压测脚本 | 4×4 墙、升级/退役实测 |

## 风险与回退

| 风险 | 阶段 | 缓解 |
| --- | --- | --- |
| DRM plane 数不足 8 | S1 | `maxLayers` 探测后不足走 RGA 预合成(req 03 §7.2);S1 只承诺 4 路 |
| 帧边界切换抖动 | S4 | 提交路径只切 plane 属性;保护带实测调参 |
| 原型改造破坏 mock 演示路径 | S2/S3 | 保留 `mock-api.js`,`real-api.js` 做运行时开关 |
| dvs-agent 语言选型返工 | S2 | 接口(JSON/IPC)与语言解耦,Python 原型期可整体替换 |
| MPP 并发解码实测 <8 路 | S1/S4 | 能力上报按实测值;常驻订阅上限随之调整 |
| C 档合并范围返工 | S0 | 以 mp4_dist 接口为基准, end2end 做适配层;不合并时 dvs-media 直接消费 mp4_dist 副本 |

## 与现有文档的关系

- 本文档取代 `docs/app/frontend_prototype_integration.md` §5 的粗粒度步骤,细化到文件级。
- 完成后回写:`docs/app/frontend_prototype_integration.md` 状态、`docs/req/05-实施路线与验证.md` 的阶段勾稽、`AGENTS.md`(新增 app/dvs_media、dvs_agent、platform 目录说明)。
- 公共库现状以 `docs/plan/RK3588_SHARED_CODE_EXTRACTION.md` 为准;本文 S0 在该文档基础上扩展,不重复其 A/B 档细节。
