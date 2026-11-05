# 前端原型(ref/)与 DVS 需求、RK3588 app 层的对应关系与集成设计

> 输入:`docs/req/`(11 条 REQ 归并分析)、`ref/video-control-platform/`(平台后台原型)、`ref/node-device-admin/`(节点本机后台原型)、`app/rk3588_live_tx/`、`app/rk3588_playout_rx/`。
> 本文回答两个问题:原型页面/数据模型与需求的对应关系;如何把两个原型承载的业务能力集成到现有 rk3588 app 层,形成"平台控制面 + 节点代理 + 节点媒体引擎"的可落地架构。
> 状态:draft,供评审;不替代 REQ 的验收口径。

## 1. 结论摘要

1. 两个原型与 `docs/req` 的 11 条 REQ 是一一对应的业务载体:`video-control-platform` 覆盖 REQ-0001~0007、0010、0011(平台侧),`node-device-admin` 覆盖 REQ-0008、0009(节点侧)。原型的 `_shared/` 三件套(`platform-store.js` / `node-state.js` / `playback-tasks.js`)已经把需求文档建议的数据模型(组、墙、场景、绑定、白名单、能力、播放任务、八态任务状态机)具象成了可运行的字段结构,可直接作为平台后端数据模型的契约基线。
2. 原型是"零依赖、零构建"的原生 HTML/JS + localStorage 模拟,API 全部由 `mock-api.js` 在浏览器内拦截;真实集成时这些 API 契约(路径、响应包 `{ok,status,code,msg,data}`、`Idempotency-Key`、审计事件)应原样保留,由真实平台服务(REST)和节点代理(HTTP)实现。
3. 现有 rk3588 app 层没有任何运行期控制通道(无 IPC/HTTP,一次性 argv+JSON 静态配置),这是集成的主要缺口。`file_playout/shared/` 已具备公共库雏形(slab/ring/timer/ptp_time/drm_renderer 多 plane/display_layout/mpp_decoder),应以它为基础抽出"节点媒体引擎"(`dvs-media`),补上 IPC 状态机(configure/prepare/commit/clear/query_state)、能力上报、布局热更新与动态会话管理。
4. 端到端集成路径:媒体引擎(MTL/MPP/RGA/DRM/ALSA)→ 节点代理(HTTP 本机页 + 到平台的注册/心跳/媒体回执通道)→ 平台服务(operation/revision/prepare-commit 框架)→ 前端(直接复用/改造两个原型)。媒体引擎的接口按 `docs/req/06-数据与接口建议.md` 第 2 节协议实现。
5. 实施顺序遵循 `docs/req/05-实施路线与验证.md` 的 P1→P2:先抽公共库 + dvs-media IPC MVP(对应原型 node-admin 页的能力),再做平台控制面并把原型页面接到真实 API(对应 platform 原型页的能力)。

## 2. 原型 ↔ 需求对应关系

### 2.1 video-control-platform(平台后台,入口 A)

| 原型页面 | 覆盖 REQ | 关键业务规则(原型已实现) | 对应 req 文档 |
| --- | --- | --- | --- |
| `group-list.html` 分组管理 | 0001 | 搜索、重命名、删除校验"组下有墙禁止删除"、8/页分页 | 01 §3.1 |
| `wall-list.html` 拼墙列表 | 0002 | 新建抽屉(LCD/LED、标准/非标准、行 1–6 列 1–15、首格分辨率、LED 逐行逐列像素 64–7680、实时总分辨率)、7 天软删除恢复、删除需绑定/窗口/operation 均为 0 | 01 §3.1、03 §3 |
| `wall-basic.html` 基本设置 | 0003 | 名称与分组唯一修改入口、色彩格式、质量、缓冲帧(仅 LED)、时间服务器、同步时钟 min≤max 校验 | 03 §4 |
| `port-bind.html` 端口绑定 | 0004 | 可用输出盒拖拽到格位矩阵、节点级绑定(一台输出盒只属一墙)、一格一口、离线节点"保存待生效、回线重校验"、整节点解绑 | 03 §5、06 §1.3 |
| `wall-image.html` 图像调节 | 0003 | 亮度/对比度/饱和度 0–100、色调 0–360,整墙一次下发全部已绑定输出盒 | 03 §4、04 §11.2 |
| `wall-transfer.html` 传输配置 | 0003 | 白名单表(单播/组播)、新墙默认为空、保存即时生效 | 03 §4、06 §1.2 |
| `wall-backdrop.html` 高清底图 | 0003 | JPG/PNG ≤10MB、整墙铺满、IndexedDB 存储 | 03 §4.3 |
| `screen-schedule.html` 画面调度 | 0005/0006/0007 | 信号源面板(白名单+在线过滤,拖拽开窗)、画布(自由/固定、吸附格位、最小 1/16 格、等比缩放、层序、静音、音量)、场景面板(保存/切换/重命名/删除,生效场景禁删)、一键清屏(不产生脏改动)、未保存改动模态(放弃/另存,不提供覆盖保存)、离线格位整墙阻止切换 | 01 §4 FLOW-01/02/03、03 §6/7/8 |
| `device-list.html` 设备列表 | 0010 | 筛选、别名编辑、退役(在线 retired / 离线 retire_pending 并清绑定)、批量升级 | 03 §11 |
| 底部播放任务板 | 0011 | 任务创建→pending_check→pending_execution→executing→playing→success/failed/cancelled/expired 八态状态机、同节点互斥、当前播放投影、retry | 03 §12.5、01 §4 FLOW-05 |

### 2.2 node-device-admin(节点本机后台,入口 B)

| 原型区块 | 覆盖 REQ | 关键业务规则(原型已实现) | 对应 req 文档 |
| --- | --- | --- | --- |
| 登录遮罩 | 0008 | 本机登录不依赖平台在线;忘记密码=物理复位键恢复;首次改密 | 03 §9.5 |
| zone-role 节点角色 | 0008 | 输出盒/输入盒二选一、切换需平台影响确认(解绑/移白名单/关窗)、待重启生效、撤销切换 | 03 §9.2 |
| zone-input 输入口 | 0008 | 采集口选项由硬件能力 resolve(不是硬编码)、信号状态只读 | 03 §9.3 |
| zone-output 输出设置 | 0008 | safeModes 过滤 + 自定义宽/高/刷新率范围校验(640–7680/480–4320/24–60)、强推/热插拔、"以节点回读能力为准" | 03 §9.4 |
| zone-net 网络与注册 | 0009 | DHCP/静态、注册服务器地址、自动/手动注册、立即注册、SIP 可选接入 | 03 §10 |
| zone-playback 播放任务 | 0011 | 当前播放卡片、非终态任务操作(检查/开始/结束/取消)、播放记录、失败重试 | 03 §12.5 |
| zone-info 本机信息 | 0009/0010 | 型号/SN/固件/角色/IP/MAC/注册状态/当前播放/CPU 温度/内存,全部只读 | 03 §10 |
| zone-ops 维护操作 | 0009 | 重启(待生效角色一并生效)、恢复出厂(保留网络与注册地址)、固件上传+升级(校验文件名、失败保留旧版本)、九态 runtimeState | 03 §10 |

### 2.3 共享数据模型(`_shared/`)与 req §06 建议的对照

原型的字段结构可直接映射到 `docs/req/06-数据与接口建议.md` 的数据表:

| 原型对象(key/命名空间) | 字段要点 | 对应表/协议 |
| --- | --- | --- |
| `DvsPlatformStore` wall(`dvs.platform.wall.v1`) | `rowPixels[]/colPixels[]`(逐行逐列像素)、`bindings` 矩阵、`whitelist[]`、`settings.image`、`scenes[].layout[]{sig,c,r,cs,rs}`、`windows[]{id,sig,x,y,w,h,z,muted}`、`layoutMode`、`volume`、`deletedAt` | `dvs_wall`(cell_grid 可由 rowPixels/colPixels 派生)、`dvs_binding`、`dvs_whitelist`、`dvs_scene` |
| `DvsNodeState` node(`dvs.node.state.v1`) | `capabilities.{inputPorts,outputPorts}(id,label,kind,maxWidth,maxHeight,maxRefreshHz)`、`configStates`(六 scope × idle/pending_apply/applied/failed)、`configRevision{desired,applied}`、`runtimeState` 九态、`currentPlayback` 投影 | `dvs_capability`、节点代理配置状态机、desired/applied revision |
| `DvsPlaybackTasks`(`dvs.playback.tasks.v1`) | `id(PUB-*)`、`sceneId/wallId/nodeIds[]`、八态 `status`、`ALLOWED` 迁移表、`expiresAt`、`errorCode`(NODE_UNAVAILABLE/TASK_CONFLICT/…)| `dvs_playback_task`(补 operation_id/target_node_id) |
| `DvsHardwareCapabilities` profile `rk3588-reference-hdmi` | `safeModes[]`、`customRange{min/max 640×480@24 – 7680×4320@60}`、`resolve(node)` 节点回传能力覆盖基线 | `dvs_capability` 快照,节点注册上报 |
| `DvsMockApi` 响应包与幂等 | `{ok,status,code,msg,data,requestId}`、`Idempotency-Key` 缓存、审计事件(`dvs.audit.events.v1`) | 平台 API 网关约定(06 §不变量:幂等、审计) |

需要注意的偏差(集成时修正):

1. 原型场景快照 `layout[]{sig,c,r,cs,rs}` 用格位坐标,req 03/06 要求整墙像素坐标 `(x,y,w,h)`;平台布局计算层需要把格位快照换算为像素窗口(建议:存储层用像素坐标,前端编辑器可继续用格位快照,换算函数版本化在平台侧)。
2. 原型未实现 `dvs_operation`(operation/revision/prepare/commit/回执聚合)、跨节点同帧提交、底图按格位拆分子图下发——这些在原型里只是 350ms 的模拟提示,是平台服务必须真正实现的部分。
3. 原型 mock API 缺 `media.prepare/commit`、心跳结构化媒体字段(`rxSessions/decoding/layers/ptp`)、`dvs_media_session` 投影;节点媒体通道契约以 req 06 §2 为准。

## 3. 现有 rk3588 app 层盘点(集成起点)

详细盘点见附录 A,核心结论:

| 维度 | rk3588_live_tx | rk3588_playout_rx |
| --- | --- | --- |
| 进程 | `live_ingest` 单进程 tx/rx/both | `file_playout_tx` / `file_playout_rx` 双可执行 |
| 配置 | argv + JSON(仅单流) | argv + 完整 JSON(pip_streams[]/tx_streams[]、DRM/ALSA/PTP/CPU 亲和) |
| 控制通道 | 无(SIGINT 之外零接口) | 无 |
| 布局 | 单流 DRM 单层 | 多 lane:8 lane 上限、NV12 直通实测 4 路,`display_layout` → DRM 多 plane |
| 复用件 | dma_allocator、rga_converter、mpp_encoder、slice_reassembler、supervisor | shared/(slab、ring、timer、ptp_time、drm_renderer 多 plane、display_layout、mpp_decoder、codec_plugin) |
| 重复代码 | 与 file_playout 的 slab/ring/timer/decoder/bridge/config 各一套 | — |

file_playout 是第二代实现,`shared/` 已是事实上的公共库;以它为底座上收 end2end 的增量(DMA 分配器、编码器、RGA、切片重组)即可得到节点媒体引擎的素材。

## 4. 集成架构设计

### 4.1 目标分层(与 req 01 §6 四层对齐)

```text
┌ 平台前端(原型 video-control-platform 改造)────────────┐
│  分组/拼墙/设置5Tab/端口绑定/画面调度/设备列表          │
├ 平台前端(原型 node-device-admin 改造,由节点代理托管)──┤
│  角色/输入口/输出/网络注册/SIP/本机信息/维护            │
├ 平台服务(dvs-platform,新建)───────────────────────────┤
│  REST API(mock-api 契约) · operation/revision 框架     │
│  布局计算(像素坐标拆分) · 播放任务状态机 · 审计        │
├ 节点网关(WS/TLS)───────────────────────────────────────┤
├ 节点代理(dvs-agent,新建)──────────────────────────────┤
│  注册/心跳 · 配置状态机 · 本机 HTTP + 静态前端 · 升级   │
├ 节点媒体引擎(dvs-media,由 file_playout/shared 演进)────────┤
│  MTL TX/RX · MPP · RGA · DRM 多plane · ALSA · PTP      │
└────────────────────────────────────────────────────────┘
```

节点内 `dvs-agent` 与 `dvs-media` 进程隔离(req 04 §3):代理崩溃媒体继续出画(离线保画),引擎崩溃由代理拉起恢复。

### 4.2 节点媒体引擎(dvs-media)——app 层改造核心

新目录建议 `app/dvs_media/`(独立 meson 工程,与现有两个 app 并列),分层:

| 层 | 来源 | 说明 |
| --- | --- | --- |
| `common/` 公共库 | file_playout `shared/` 为主 | slab_pool、ring_buffer、timer、ptp_time、drm_renderer(多 plane)、drm_mode、display_layout、mpp_decoder、codec_plugin;从 end2end 上收 dma_allocator/dma32_allocator、mpp_encoder、rga_converter、slice_reassembler、sei_timestamp |
| `engine/` 引擎核心 | 新写 | 会话管理器(动态创建/销毁 MTL ST22/ST30 会话)、布局执行器(接收 JSON 布局片段→影子布局→原子提交 DRM plane)、音频仲裁(audioOwner+volume→ALSA)、能力探测(分辨率/路数/plane 数/编解码) |
| `ipc/` 控制接口 | 新写 | Unix domain socket(JSON 行协议,req 04 §3 消息集):`configure / prepare / commit / abort / set_sources / clear / query_state / event` |
| `cli/` | 改造 mp4_rx_app | 启动参数兼容:`--config`(静态 JSON,兼容现有联调)、`--ipc-sock`(受控模式) |

关键设计决策:

1. **布局热更新**:现有 `rx_compositor` 在 create 时一次性 resolve lane→DRM plane。改为 prepare 阶段在影子布局中预建/复用 plane 与解码器,commit 在帧边界只切 plane 属性(src_crop/dst/z/visible),失败 rollback 到影子副本——这正是 req 03 §6.2 同帧流程的节点侧。`display_layout` 数据结构可直接复用,把"create 时定死"改为"prepare/commit 两段式"。
2. **动态会话**:lane 与 MTL RX session 解耦为引用计数;白名单内常驻订阅(上限 8 路 1080p60,LRU 淘汰),换源用 `st22p_rx_update_source`(req 03 §7.2/7.3)。
3. **能力上报**:`query_state` / 注册时返回结构化 capabilities(端口、safeModes 探测自 DRM connector、maxDecodeSessions、maxLayers、ptp state/offsetUs),替代原型 `hardware-capabilities.js` 里的硬编码 profile——前端页面的 `resolve(node)` 逻辑不变,只是数据源改为真实节点上报。
4. **静态兼容模式**:不带 `--ipc-sock` 时按现有 JSON 一次性配置运行,保证现有联调脚本和单测不受影响。

### 4.3 节点代理(dvs-agent)与 node-device-admin 原型落地

- 新建 `app/dvs_agent/`(C++17)。职责:本地 HTTP(`/api/v1/node/*`,托管 node-admin 前端静态文件)、配置状态机(六 scope,desired/applied revision,`configStates`)、注册/心跳(3s,携能力+媒体统计+PTP)、维护操作(重启/恢复出厂/升级校验)。
- 原型改造量小:node-admin 页面本身不依赖 mock 之外的框架,把 `mock-api.js` 替换为真实 fetch(或保留 mock 文件名做薄适配层),数据源从 localStorage 的 `DvsNodeState` 换成本地 API。
- 代理到媒体引擎:仅通过 IPC(configure/prepare/commit/…),不直接碰 MTL。

### 4.4 平台服务与 video-control-platform 原型落地

- 新建平台服务(dvs-platform):实现 mock-api.js 的全部 REST 契约(§2.1 表所列页面背后的接口)+ req 06 §2 的节点网关协议(register/heartbeat/media.prepare/media.commit/media.abort/media.rollback/事件)。
- **保留 mock-api.js 作为 API 契约规格**:`{ok,status,code,msg,data,requestId}` 响应包、`Idempotency-Key`、审计事件流、错误码(VALIDATION_ERROR/…,并补 req 06 §4 的媒体错误码 MEDIA_SESSION_FAILED/CLOCK_UNSYNCED/FRAME_DEADLINE_MISSED 等)。
- 画面调度页(`screen-schedule.html`)保持"编辑态立即上屏 + 未保存改动"交互;平台把运行态窗口换算为像素坐标后走 prepare/commit 下发;单节点窗口可立即生效,跨节点窗口走同帧流程。
- 播放任务板直接对接真实 `dvs_playback_task`(八态、operation_id 关联),节点 `currentPlayback` 由心跳投影回填。

### 4.5 需要自研而原型完全没有的部分(风险清单)

| 缺口 | 归属 | 说明 |
| --- | --- | --- |
| prepare/commit 同帧提交、apply_at、影子布局、rollback | 平台 + dvs-media | req 03 §6、04 §6;最高风险项,P3 单独立项验证 |
| 跨格窗口像素拆分(节点片段/取整规则) | 平台布局计算 | req 04 §5;原型只有格位快照 |
| 底图按格位并集拆分子图并下发 | 平台 + 节点 | req 03 §4.3;原型只整墙铺满预览 |
| PTP 时钟资格判定 | 平台 + 节点代理 | req 03 §12.3 |
| 组播地址池管理、SSM、IGMP | 平台 + 交换机配置 | req 03 §4.2、04 §8.3 |
| TLS/mTLS、固件签名、墓碑、审计持久化 | 平台 + 代理 | req 04 §14、06 §6 |

## 5. 实施步骤(映射 req 05 阶段)

1. **P1-a 公共库抽取**:建 `app/common/`(或 dvs_media 内 common/),合并两 app 的重复实现,现有 end2end/file_playout 改为引用,跑通双方单测。
2. **P1-b dvs-media MVP**:engine + ipc,UML 消息集按 req 06 §2;退出标准=req 05 §3(单节点 4 窗口、音频、恢复布局)。此阶段前端可用 node-admin 原型 + 手工 mock 验证 IPC。
3. **P1-c dvs-agent MVP**:本地 HTTP + node-admin 原型接入真实 `/api/v1/node/*`;注册/心跳可用一个最小平台 stub。
4. **P2 平台服务**:实现 mock-api 契约 + 节点网关;video-control-platform 原型页替换数据源,走通 FLOW-01(两节点 2×1 墙:建组→建墙→绑定→白名单→开窗→存场景)。
5. **P3 同帧**:prepare/commit/回执聚合/影子布局/跨格拆分;`apply_at` 用 ptp_time + MTL VSYNC/用户 pacing 落地。
6. **P4 运维**:退役/墓碑/升级接 device-list 与 node-admin zone-ops。

## 6. 待评审决策

| 问题 | 选项 | 建议 |
| --- | --- | --- |
| 场景快照坐标 | 格位快照(原型)/整墙像素(req 06) | 存储像素、前端编辑用格位换算 |
| dvs-media 语言/IPC | C + Unix socket JSON(req 04 建议) | 采纳;后续可换 protobuf |
| dvs-agent 语言 | C++17 | 与平台服务统一语言,避免第二运行时 |
| dvs-media 是否并入 MTL 顶层构建 | 独立 meson(现状)/并入 | 维持独立,避免 DPDK 探测复杂化 |

---

## 附录 A:rk3588 app 层盘点明细

### A.1 rk3588_live_tx

- 入口 `live_ingest.c:459 main()`,模式 `tx/rx/both`(`run_tx_mode:349`、`run_rx_mode:373`、`run_both_mode:410`)。
- argv 解析 `parse_args(:49-93)`;JSON 配置 `e2e_config.c:82` 仅取 `streams[0]/interfaces[0]`(:98、:115),单流设计。
- TX 线程链:V4L2 采集(`io/v4l2_capturer.c:449`)→ RGA → MPP 编码(`codec/mpp_encoder.c:330` collector 线程)→ tx_pipeline 编码线程 + supervisor 看门狗(`pipeline/tx_pipeline.c:534、552`)→ MTL TX。
- RX 线程链:MTL RX → bridge(`mtl_rx_bridge.c:121`)→ 解码(`pipeline/rx_pipeline.c:205`)→ DRM。
- 独有可复用件:`hal/dma_allocator.c`、`hal/dma32_allocator.c`、`hal/sei_timestamp.c`、`codec/rga_converter.c`、`codec/mpp_encoder.c`、`pipeline/slice_reassembler.c`。

### A.2 rk3588_playout_rx

- TX:`mp4_tx_app.c:25 main()` → `tx/tx_scheduler.c:500、511` 每 lane video 线程 + audio 线程,`tx_streams[]`(`shared/display_layout.h:46-57`)。
- RX:`mp4_rx_app.c:387 main()`,`pip_stream_count>0` 走 `run_compositor(:114)`,否则 `run_legacy_pipeline(:205)`。
- 配置:`file_playout_config.c:72-286 file_playout_config_load`(端口/IP、DRM、ALSA、CPU 亲和、`ptp_mode:172-180`、`display_viewport:189-200`、`pip_streams[]:203+`)。
- 合成:`rx/rx_compositor.c`,`MP4_PIP_MAX_LANES=8`、NV12 直通 4 路(`shared/display_layout.h:16-17`);layout 由 `display_layout_from_pip_streams(:87)` 解析为 `{dst_x/y/w/h,zpos,plane_id}`;create 一次性构建(`rx_compositor.c:401、414-437、498-572`),运行期不可变;音频只取 main lane(`:467-482`);统计 stdout(`:595-641`)。
- shared 静态库:`meson.build:69-84`。

### A.3 缺口汇总(→ §4.2)

无 IPC/HTTP 控制通道;能力硬编码;PTP 只有读取与一次性打印(`shared/ptp_time.c:22、25`);布局不可热更;MTL 会话与 lane 绑定不可动态增删;统计为 stdout 文本。对应改造见 §4.2 四条设计决策。
