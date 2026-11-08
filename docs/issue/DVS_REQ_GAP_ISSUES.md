# DVS 需求符合度问题清单（对照 docs/req）

> 基准：`docs/req`（01 全景、03 映射、04 方案、06 接口建议）逐条 REQ 对照当前工作树静态审计。
> 方法：只读代码，未做编译与单板运行；“已实现”指代码已存在，不代表已在 RK3588 上验证。
> 与既有文档关系：`docs/plan/dvs/BACKEND_STATUS_AND_REMAINING_TASKS.md` 与
> `DVS_P1_P3_REMAINING_GAPS.md` 中部分“未实现”结论已过时，见第 0 节，**不要据此重复立项**。
> 修复排期见 `docs/plan/dvs/DVS_REQ_GAP_REMEDIATION_PLAN.md`。

## 0. 复核结论：以下旧缺口在当前树中已实现，只剩硬件验证

| 旧文档说法 | 当前树实际状态 | 依据 |
|---|---|---|
| PL-1/PL-2：业务规则、场景快照、未保存比较、默认场景、生效保护缺 | 已实现 | `app/dvs_platform/main.cpp` 的 `route_docs` 全分支校验 + `layout.cpp` 六个 `validate_*` + `/scenes/compare` + 新墙默认场景 + 生效场景禁删 |
| 分组名称唯一、拼墙软删、端口空闲、离线 pending、组播池、绑定白名单约束缺 | 已实现 | 同上；`multicast_pool_next` + `/whitelist/allocate` + `promote_pending_bindings` |
| ME-8：音频归属仅 Master 音量 | 通道已实现，待验证 | `engine/audio_arbiter.c`、`audio_owner.c`、agent 取“最新未静音窗口” + 引擎侧二次仲裁 |
| 白名单准入缺 | 已实现 | `session_mgr` 白名单 + 八槽 LRU，代理与引擎桥双侧逐项校验 |
| 退役/墓碑/串行升级缺 | 已实现 | `tombstone_*`、`retire-queue`、升级串行队列 |
| barrier/rollback 只有描述 | 已真实扇出 | `OpCoordinator` + `fanout_layout_parallel`（WS 优先、HTTP 回退）+ `rollback` 重发 + 持久化恢复 |

## 1. 严重：阻塞 V1.0 验收

### GAP-07 无全局帧号语义，提交不是帧边界原子切换（REQ-0005/0006/0011）

- 现象：平台只下发 `applyAtNs`；节点 `engine_apply_layout_at` 是“sleep 到 TAI 时刻再换布局”，未映射到目标帧号，未在 vblank 做原子提交；06 要求的 `epoch_tai_ns / frameIndex / fps` 三元组全缺。
- 影响：NFR-04（同帧偏差 ≤1 帧）无法证明；跨独立显示器同帧验收关不了单。
- 涉及：`app/dvs_platform/main.cpp`（prepare/commit 扇出）、`app/dvs_media/engine/engine_ctx.c`、`engine/layout_executor.c`、`app/dvs_common/include/dvs_contract.h`（契约缺字段）。
- 建议：平台维护墙级 epoch/fps，下发 `frameIndex`；节点把 `apply_at` 映射到目标帧号，在 N-1 的 vblank 原子切 plane；commit 回执带 `frameIndex/appliedAtTaiNs/layoutHash`；任一节点错过目标帧标记 `late` 并触发回滚。

### GAP-08 编码策略与订阅策略双缺，切换耗时无保障（REQ-0005）

- 现象：`tx_config` 只有编码、宽高、帧率、码率四项，无 GOP/IDR 间隔、CBR、无 B 帧参数；无“白名单内常驻订阅 + 超上限淘汰冷门 + 页面提示正在准备”策略；prepare 不等待“每信号至少解出一帧”。
- 影响：NFR-03（场景切换 ≤1s）取决于 GOP 碰运气；输出节点解码路数超限时行为未定义。
- 涉及：`app/dvs_agent/tx_config.*`、`app/dvs_media/engine/tx_session.*`、`tx_backend_live_ingest.c`、平台扇出与 `node_clock_ok` 门控。
- 建议：编码参数加 GOP/CBR 开关（默认短 GOP）；prepare 加首帧就绪门控与超时；平台加常驻订阅上限保护与淘汰排序。

### GAP-03 高清底图端到端全缺（REQ-0003/0007）

- 现象：平台只有 `baseImageRevision` 数字；无原图存储、无按节点格位并集裁剪子图、无节点拉取、无最底层 plane、无“下次切换/清屏生效”规则。媒体侧只有清屏保留 backdrop 的注释。
- 影响：一键清屏后无底图可留；拼墙设置“高清底图”页无后端。
- 涉及：`app/dvs_platform/main.cpp`（墙设置接口）、`app/dvs_media/engine/layout_executor.c`、`engine_ctx.c`。
- 建议：平台加底图原图 + 子图缓存 + revision 下发；节点加底图 framebuffer 层（z 最低），更新走普通 revision。

### GAP-04 图像调节全缺（REQ-0003）

- 现象：亮度/对比度/饱和度/色调在三端零命中；无 DRM plane 属性探测，无 RGA/GPU 兜底。
- 涉及：`app/dvs_media/engine/capability.c`（探测位）、`engine_ctx.c`（应用位）、平台墙设置接口。
- 建议：能力探测先行；平台只下发目标值，节点回读实际应用值，不支持项如实上报而非静默忽略。

### GAP-21 输入盒音频链缺失（REQ-0003/0008）

- 现象：TX 数据面只有视频（V4L2→RGA→MPP→ST22），ALSA 采集→ST2110-30 TX 整条缺失；`tx_backend_live_ingest.c` 有音频预留注释但无实现。
- 影响：端到端音频链是断的；输出侧音频仲裁再完整也无源可播。
- 涉及：`app/dvs_media/engine/tx_backend_live_ingest.c`、`tx_session.*`、白名单 `audioPort` 已有可直接用。
- 建议：补采集侧音频会话与启停编排，与视频会话同生命周期。

### GAP-12 角色切换原子作业未编排（REQ-0008）

- 现象：agent 只有“记意图 + 待重启”，平台只有 `pendingRole` 镜像；“影响检查→平台原子清理（解绑/关窗/移白名单）→重启→注册验证→成功/回退”无人编排。
- 涉及：`app/dvs_platform`（缺清理作业）、`app/dvs_agent/main.cpp`（角色域）。
- 建议：平台加角色切换作业，引用清理完成前拒绝重启放行；平台不可达时沿用现有“只登记意图”语义。

### GAP-15 网络应用失败无自动回滚（REQ-0009）

- 现象：只有格式校验、旧快照、`confirm` 意图门；无“应用后可达性探测 + 断连超时自动回滚旧配置”。
- 涉及：`app/dvs_agent/network_apply.*`、`main.cpp` 网络域。
- 建议：应用后探测网关/平台可达，超时恢复快照并上报 `failed`，审计留痕。

### GAP-18 播放任务与场景 operation 未关联（REQ-0011）

- 现象：`playback-tasks` 八态机是孤儿状态机，无 06 要求的 `operation_id` / `target_node_id` 关联；`executing→playing` 不创建场景切换 operation。
- 影响：“任务状态与画面一致性”缺口未闭合；同一节点互斥已有，但任务与实际布局无因果链。
- 涉及：`app/dvs_platform/main.cpp`（播放任务路由）。
- 建议：执行阶段创建关联 operation，任务状态由 operation 回执驱动；结束/取消/过期清除当前播放投影。

## 2. 中等：应补，影响体验或可测性

### GAP-05 白名单缺 SSM 源过滤字段（REQ-0003）

- 白名单无 `sourceIp`，与 04 组播规划（SSM、指定源、TTL、VLAN）对不上；endpoint 组装也无源地址。补字段 + 保存校验 + RX 侧透传。

### GAP-09 媒体错误码未对齐 06 第 4 节（REQ-0005/0011）

- `DECODE_NOT_READY / CLOCK_UNSYNCED / APPLY_AT_TOO_SOON / FRAME_DEADLINE_MISSED / LAYOUT_CONFLICT / CAPABILITY_EXCEEDED / MEDIA_SESSION_FAILED / ROLLBACK_FAILED` 在三端无映射，多为泛 `blocked/INVALID_ARGUMENT`。补码表 + 平台透传 + 前端可视化（失败节点/阶段/重试入口）。

### GAP-10 窗口规则服务端校验缺两项（REQ-0006）

- `fixed` 模式无“吸附格位、一格一窗”逻辑；最小尺寸无“1/16 格”校验（仅 64px 下限）。平台补校验，节点不重复算（取整权已在平台）。

### GAP-02 墙删除缺“无窗口”前置（REQ-0002）

- 删除检查了绑定/operation/播放任务，未校验场景窗口已清空。补一项拒绝，或明确“删墙前必须先清屏”的操作顺序并在接口返回引导。

### GAP-06 绑定生效后无布局联动（REQ-0004）

- 绑定与布局是两次独立写操作，绑定变化不重算受影响窗口。补“绑定生效后自动起一笔布局 operation”，失败可单独重试。

### GAP-11 清屏跨节点屏障路径待确认（REQ-0007）

- 清屏通道代码存在（空窗口数组），需确认平台清屏同样走 `ops/prepare` 全有或全无屏障，而非直写节点。确认后补一条端到端用例。

### GAP-14 输出模式 EDID 语义缺失（REQ-0008）

- `set_output` 有 `mode` 透参但无 EDID 读取/热插拔跟随逻辑，“强推 vs 热插拔”只在页面文案。补 EDID 探测 + 模式协商 + 超能力拒绝（与 AG-3 合并）。

### GAP-17 固件升级缺签名与 A/B（REQ-0009/0010）

- 只有 SHA-256、分版本 staging、无签名验签、无 A/B 分区、无断电回滚证据。变砖风险为高。补签名校验 + A/B 落盘 + 断电用例（与 AG-6 合并）。

### GAP-20 统计与运行态投影缺失（REQ-0011）

- `mtl_get_port_stats` / RX timing 未接入；`query_state` 部分计数仍是布局回显；06 的 `dvs_media_session` 投影表缺失。先接 MTL 端口统计与 lane 丢包实测，再决定投影表是否落库（建议节点侧内存 + 按需拉取，不高频写库）。

### GAP-25 平台反向写配置边界未文档化（REQ-0010）

- 平台经 `/node/config/sync` 反向写节点，与“本机页是配置真源”存在张力。补文档：明确允许平台推送的 scope 白名单，其余只读。

## 3. 可延期：决策先行或 P5

### GAP-13 无信号端口语义与需求文字冲突（REQ-0008，决策）

- REQ 要求“无信号保存成功、页面显示无信号”，`tx_port_map` 直接拒绝无信号端口。评审二选一后统一实现与文案，勿两头并存。

### GAP-16 SIP 无协议栈（REQ-0009，决策，D11）

- 现状仅 `connectivity-probe` 存根。V1.0 要么接入 SIP 栈（PJSIP/baresip 二选一），要么把 REQ 中的 SIP 显式降级为“状态显示”，并关闭相关页面入口。

### GAP-19 ST 2022-7 冗余零实现（REQ-0011，决策）

- 单端口初始化，无双网口会话。若 V1.0 不做，需在能力上报与文档中声明不支持，避免验收误判。

### GAP-22 审计保留 180 天无策略（REQ-0011）

- 平台审计只有写入无保留/GC 策略；agent 环形 200 条易失只算运维日志。补平台审计保留与归档策略（NFR-12）。

### GAP-23 传输安全缺失（REQ-0011/04 安全设计）

- TLS/mTLS、证书签发轮换、平台登录限速（agent 已有限速）全缺。列入 P5，部署拓扑先用网络隔离兜底并文档化。

### GAP-01 平台列表缺搜索分页（REQ-0001）

- 全量返回，大规模下前端自过滤。补 `q/page/pageSize`，低风险。

### GAP-24 容量与故障注入无代码落点（05 第 9 节/P5）

- 128 节点模拟器、64 墙/64 窗压测、十项故障注入矩阵、24h 烤机均无实现。列入 P5，但两节点 1000 次切换实测必须提前到 P3 出口。

## 4. 不变量检查（跨 REQ，易漏）

- revision 单调 + 旧回执只审计：平台/媒体两端已有，保留回归用例。
- operation/revision/幂等/过期：已有，注意 `expires_at` 语义在平台是 deadline sweep，在媒体是 prepare 超时，两端阈值需对齐成同一配置源。
- 能力回读唯一真源：能力探测已有，但 `max_decode_sessions` 仍是固定值加环境覆盖，单板实测前不要把页面写死。
- 删除语义：分组硬删、拼墙软删 7 天、场景生效保护、节点墓碑 30 天——前三者已实现，最后确认墓碑 GC 周期确为 30 天。
- 离线保画：架构（双进程隔离）已有，行为（保最后一帧 30 分钟、重启恢复布局）待单板验证，不得回退为“控制断开即清屏”。

## 5. 实施状态（阶段一/二，当前工作树）

以下 GAP 的代码修改已落地；凡标“待单板验证”的均需 RK3588 实机确认，
不得仅以 x86 单元测试关单。

- GAP-07：已落地。平台维护墙级 epoch/fps（建墙/自愈/冻结）、下发目标帧号；
  节点按帧起始时刻提交并回执实际帧号/时刻/是否错过；平台做帧号一致性
  与 late 判定并触发回滚。单元测试覆盖帧数学往返与契约字段。
  残留硬件步骤：vblank 原子 plane 翻转（MTL ST22_RX_FLAG_ENABLE_VSYNC +
  notify_event 接入 DRM 原子提交，见 rx_compositor 渲染路径）。
- GAP-08：已落地一半。TX GOP 间隔端到端打通（agent 解析校验 → 引擎参数
  → tx_pipeline gop_size）；平台按下发前做单节点解码配额门控
  （CAPABILITY_EXCEEDED）；引擎提交失败区分 DECODE_NOT_READY / 
  LAYOUT_CONFLICT / MEDIA_SESSION_FAILED 并透传回执。
  未做：常驻订阅淘汰排序与页面“正在准备”提示（需解码器占用实测数据）。
- GAP-09：已落地。媒体规范错误码进入共享契约，三端透传；平台审计
  commit-reason/frame-mismatch/frame-late。
- GAP-05：已落地。白名单 sourceIp → 端点表 → 引擎流 → MTL 会话源过滤
  （mcast_sip_addr）；try_fast_apply 把源过滤变化视为换源。
- GAP-04： plumbing 落地（墙设置校验 → 扇出 → 代理透传 → 引擎暂存与
  query_state 回读，unsupported 如实上报）。应用层缺显示后端能力，
  待驱动属性支持后接。
- GAP-03：已落地。平台存原图 + 按节点算子区 → 代理按版本缓存下载 →
  引擎 FFmpeg 软解 + 裁剪 + 拉伸到画布 → 背景 plane；清屏保留底图
  （0-lane 合成器），提交/清屏/重建后重推。待单板验证解码格式覆盖
  与显存占用。
- GAP-21：已落地。TX 音频链（ALSA 采集 → ST2110-30，48k 立体声 PCM16，
  10ms 帧），带启动就绪握手，失败则整个 TX 启动 loud 失败。
  待单板验证声画同步与长期稳定性。
- GAP-10/GAP-02/GAP-06/GAP-11：已落地。固定模式吸附存快照、墙级最小窗、
  绑定生效自动起布局单、清屏走同一屏障（待一条端到端用例确认）。
- GAP-18 关联项：播放任务别名归一化（wall/scene/node/targetNodeIds →
  规范字段）已落地，修复前端真实创建失败；operation_id 关联仍未做。
- ME-8/音频仲裁：通道代码已存在，待真实音频流联调。
- 基线测试漂移（非本次引入，勿顺手改协议）：平台 test_basic 有 9 个
  预存失败（hello 未知键严格性、playback 别名），与实现/前端三方口径
  不一致，需产品决策后再统一。
