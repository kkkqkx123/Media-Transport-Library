# DVS 后端状态与剩余任务 — 真相源

> 本文件是 `docs/plan/` 中 DVS 后端相关文档的唯一真相源。  
> 其他 plan 文档（`DVS_REMAINING_TASKS.md`、`DVS_REMAINING_TASKS_DETAILED_PLAN.md`、`DVS_SIMPLIFIED_AND_REMAINING.md` 等）仅作为历史细化记录，状态以本文件为准。  
> 前端相关剩余任务与实现方案见 `docs/plan/web/`。

## 1. 当前实现状态

### 1.1 模块成熟度

| 模块 | 状态 | 说明 |
|---|---|---|
| `app/dvs_common` | 可用 | HTTP server/client、SHA256/PBKDF2、SQLite 封装。 |
| `app/dvs_media` | 单节点 MVP | 复用 `rk3588_file_playout` 的 RX compositor/DRM/MPP/ALSA；IPC 状态机、白名单准入、布局 prepare/commit、能力探测已实现。 |
| `app/dvs_agent` | 单节点控制面 MVP | 节点存储、六 scope 配置状态机、注册/心跳、本机 HTTP、维护操作、媒体 IPC 转发已实现。 |
| `app/dvs_platform` | 通用文档库 + 协调器原型 | 通用 CRUD、tombstone、operation barrier、布局拆分数学、串行升级入口已实现。 |

### 1.2 已实现且基本对齐需求的部分

- MTL RX/TX、组播、PTP 读取、DRM 多 plane compositor 复用现有 RK3588 链路。
- 单节点多窗口：`rx_compositor` 支持最多 8 路 lane（V1 NV12 实际 4 路）。
- IPC 两阶段：`configure / prepare / commit / abort / clear / query_state`。
- revision 幂等与并发保护：`state_machine.c` 支持 stale revision、commit-before-prepare、并发 operation 拒绝、幂等 replay。
- 白名单准入与 LRU：`session_admit.c` 实现 whitelist + 8-slot LRU。
- 音量：`audio_arbiter.c` 可调 ALSA Master 音量。
- 能力探测：DRM connector/plane、max decode sessions、PTP/MTL 字段。
- 节点注册、心跳、离线判定（3 次失败）、墓碑 30 天。
- 节点本机配置 scope 与 desired/applied revision。
- 工厂重置保留网络/注册；升级 SHA256 校验与 A/B staging。
- 幂等键缓存、审计日志、tombstone GC。
- `OpCoordinator` prepare barrier + rollback 描述。
- 布局拆分数学：`layout_cell_grid`、`layout_window_fragment`、`layout_apply_at_ns`。

### 1.3 主要未实现 / 仅占位

- 平台业务校验：分组删除、拼墙生命周期、白名单地址分配、绑定约束、设备退役清理。
- 场景快照、未保存改动比较、默认场景、生效场景保护。
- 平台到节点的真实下发与回执聚合；prepare 只记录 members，不下发。
- 媒体引擎真实 prepare：组播加入、RX 会话、首帧解码、plane 预分配、影子布局。
- 跨格窗口源裁剪：媒体引擎缺少 src crop 字段与 DRM plane src 参数。
- 音频归属与路由：仅 ALSA Master 音量，无窗口级音频仲裁。
- 底图拆分与下发。
- 同帧提交：无 VSYNC/vblank 原子提交、无影子布局、无 rollback。
- 时钟资格判定：仅 PTP 读取占位。
- 节点角色切换作业、网络回滚、SIP、固件 A/B 激活。
- 节点本机媒体状态接口多为占位。

## 2. 剩余任务总清单

### 2.1 媒体引擎（P0）

| ID | 任务 | 优先级 | 依赖 | 退出标准 |
|---|---|---|---|---|
| ME-1 | 真实能力探测：outputPorts/maxDecodeSessions/maxLayers/ptp/mtl | P0 | RK3588 | 能力 JSON 为真实枚举值 |
| ME-2 | query_state 真实化：媒体计数与 PTP 状态来自引擎 | P0 | ME-1 | query_state 返回真实计数 |
| ME-3 | `--config` 真正启动媒体管线 | P0 | 无 | 单板出画 |
| ME-4 | session_mgr / layout_executor / audio_arbiter 真实实现 | P0 | ME-3 | 单节点 4 路热更新 |
| ME-5 | 帧边界原子切换：预建 + commit 只切 plane 属性 | P1 | ME-4 | 热切无黑帧 |
| ME-6 | 运行期动态换源与 LRU 常驻订阅 | P1 | ME-4 | 单路换源不闪断 |
| ME-7 | pip_stream 增加 src crop 字段并让 DRM renderer 使用 | P1 | ME-4 | 跨格源裁剪生效 |
| ME-8 | 音频归属路由与静音/音量 | P1 | ME-4 | 音频跟随指定窗口 |

### 2.2 平台服务（P1/P2）

| ID | 任务 | 优先级 | 依赖 | 退出标准 |
|---|---|---|---|---|
| PL-1 | 业务模型校验：group/wall/scene/binding/whitelist | P1 | 无 | CRUD 符合 REQ 规则 |
| PL-2 | 场景快照与未保存改动比较 | P1 | PL-1 | 场景保存/切换/清屏符合 REQ |
| PL-3 | 节点网关下发 prepare/commit 并聚合回执 | P1 | PL-2, ME-4 | 两节点 operation 闭环 |
| PL-4 | 布局像素计算与跨格拆分 | P1 | PL-1 | 跨格窗口无接缝 |
| PL-5 | 底图按格位并集拆分子图并下发 | P2 | PL-4 | 清屏后只剩底图 |
| PL-6 | 时钟资格判定 | P2 | PL-3, ME-1 | 不满足则整墙 blocked |
| PL-7 | prepare/commit 同帧提交与 rollback | P2 | PL-3, ME-5 | 1000 次切换偏差 ≤1 帧 |
| PL-8 | 退役/墓碑/批量升级/播放任务 | P3 | PL-1 | 设备管理符合 REQ |

### 2.3 节点代理（P2/P3）

| ID | 任务 | 优先级 | 依赖 | 退出标准 |
|---|---|---|---|---|
| AG-1 | 登录态严格化与改密用户参数化 | P2 | 无 | x86 单测通过 |
| AG-2 | 角色切换受控作业 | P2 | 平台清理 | 平台不可达只保存意图 |
| AG-3 | 输出参数强推/热插拔与 EDID | P2 | ME-1 | 超出能力值拒绝 |
| AG-4 | 网络配置回滚与自动/手动注册 | P2 | 无 | 断连自动回滚 |
| AG-5 | SIP 栈接入 | P3 | 无 | SIP 不可达不影响媒体 |
| AG-6 | 固件 A/B 激活与进度上报 | P3 | 无 | 升级失败保留旧版本 |
| AG-7 | 本机媒体状态/能力/日志接口真实化 | P2 | ME-1/ME-2 | 不再返回空 |

### 2.4 文档与构建

| ID | 任务 | 优先级 | 退出标准 |
|---|---|---|---|
| DC-1 | 文档回写：AGENTS.md、docs/app/README.md 共享代码节 | P1 | 文档与代码一致 |
| DC-2 | dvs_media 构建约束说明（需 rockchip_mpp） | P1 | x86 不可构建说明清晰 |

## 3. 前端需要的后端功能缺口

以下功能是前端原型（`video-control-platform` / `node-device-admin`）或需求要求前端使用的，但后端尚未提供。

### 3.1 平台前端需要

| 前端页面/功能 | 需要的后端能力 | 当前状态 |
|---|---|---|
| 分组管理 | 删除前墙计数校验、名称唯一 | 未实现 |
| 拼墙管理 | cell_grid 固化、LED 像素校验、软删除 7 天、恢复不恢复绑定 | 未实现 |
| 端口绑定 | 节点能力上报、端口空闲校验、离线 pending、node_origin/canvas | 部分 |
| 传输配置 | 组播地址池分配、端口唯一性、编码参数 | 未实现 |
| 图像调节 | DRM/RGA 属性下发、整墙一次下发 | 未实现 |
| 高清底图 | 底图拆分、子图缓存、revision 下发 | 未实现 |
| 画面调度 | 场景快照、未保存改动比较、prepare/commit、清屏 operation | 未实现 |
| 设备列表 | 退役清理、墓碑、批量升级进度 | 部分 |
| 播放任务板 | 八态状态机、operation 关联、节点 currentPlayback 投影 | 未实现 |

### 3.2 节点本机前端需要

| 前端页面/功能 | 需要的后端能力 | 当前状态 |
|---|---|---|
| 登录/改密 | 严格登录态、用户参数化 | 部分 |
| 角色切换 | 影响检查、平台清理、重启、注册验证 | 未实现 |
| 输入口 | 硬件能力 resolve、信号状态 | 部分 |
| 输出设置 | safeModes 过滤、自定义范围校验、强推/热插拔 | 部分 |
| 网络与注册 | DHCP/静态、回滚、自动/手动注册 | 部分 |
| SIP | 可选 SIP 栈、状态上报 | 未实现 |
| 本机信息 | 型号/SN/固件/角色/IP/MAC/注册状态/当前播放/CPU/内存/温度 | 部分 |
| 维护操作 | 重启、恢复出厂、升级校验与 A/B | 部分 |
| 媒体状态 | 收发会话、解码、图层、音频、PTP | 占位 |
| 能力上报 | 完整 capabilities、PTP 实时状态 | 部分 |

## 3.3 前端缺口修复实施状态

以下状态对应 `docs/plan/dvs/DVS_AGENT_FRONTEND_GAPS_FIX_PLAN.md` 的六个配置域与支撑能力。代码基线为当前工作树，仅静态审计，未运行测试/编译。

| 域 | 当前状态 | 已实现 | 剩余任务 |
|---|---|---|---|
| 角色域 | 基本完成 | 保存校验 input/output、pendingRole、撤销回当前角色、重启应用 pending role、输入角色 IPC clear | 平台侧绑定/白名单清理仍由平台事务完成 |
| 输入口域 | 部分完成 | 采集口校验与能力命中、按字段转发白名单、媒体 whitelist 分支 | 白名单逐项地址/端口校验未落地；代理转发失败不应 mark_failed |
| 输出设置域 | 部分完成 | 能力上限校验、写入 node output、标记已生效、媒体更新模式与重建 | 合成器重建应转为后台方式；无画面时仅保存待下次布局 |
| 网络域 | 未完成 | 静态/动态格式校验、写入 network/server | 旧快照、系统应用、失败回滚、env 门控、节点 IP 更新未实现 |
| 注册域 | 基本完成 | 校验服务器、写入 registerMode/server、auto 异步注册、手动显式触发、心跳摘要 | 无新增 |
| 会话接入域 | 完成 | 关闭/启用校验、3 秒可达探测、失败/成功状态 | 无新增 |
| 登录与改密 | 完成 | 随机会话、过期、登出、env 鉴权门、改密用户默认 admin | 无新增 |
| 本机信息与能力 | 部分完成 | 节点种子端口、IPC 合并、媒体 DRM/decode/layers/ports | MTL 版本/PTP 刷新、输入口真实枚举、启动时主动拉取未完成 |
| 播放任务投影 | 完成 | 状态透传、布局转发、心跳摘要 | 无新增 |
| 维护操作 | 部分完成 | 重启 env 门控、恢复出厂保留字段、升级校验与 staging、上传端点 | 升级真实切换 env 门控、前端上传二进制链路未打通 |
| 遗留清理 | 完成 | 删除未使用 `media_ok`、更新 registration 注释 | 无新增 |

剩余高优先任务：

- NET-1：网络域旧快照、系统应用、失败回滚与 `DVS_ALLOW_NETWORK` 门控。
- CAP-1：媒体启动后刷新 MTL 版本与 PTP 状态，并在代理启动时主动合并能力。
- WL-1：白名单逐项地址/端口校验，非法项返回参数错误。
- FWD-1：媒体转发失败仅记日志，不改变已生效判定。
- OUT-1：输出模式重建转为后台方式，无画面时仅保存。
- UPG-1：升级真实切换 env 门控与前端二进制上传链路。

## 4. 实施顺序

1. 补齐媒体引擎 MVP（ME-1～ME-4），单节点 4 路出画。
2. 补齐平台业务模型与节点网关（PL-1～PL-4），两节点 operation 闭环。
3. 补齐同帧与跨格（ME-5/ME-7/PL-6/PL-7）。
4. 补齐节点运维与设备管理（AG-2～AG-7、PL-8）。
5. 能力上报与音频仲裁收尾（ME-6/ME-8）。
6. 文档回写与构建约束说明（DC-1/DC-2）。

## 5. 验证矩阵

| 任务 | x86 可验证 | 需 RK3588 |
|---|---|---|
| ME-1 能力探测 | 否 | 是 |
| ME-2 query_state 真实化 | 否 | 是 |
| ME-3 --config 启动管线 | 否 | 是 |
| ME-4 引擎实现 | 否 | 是 |
| ME-5 原子切换 | 否 | 是 |
| ME-6 动态换源 | 否 | 是 |
| ME-7 src crop | 否 | 是 |
| ME-8 音频路由 | 否 | 是 |
| PL-* 平台服务 | 是 | 端到端需 RK3588 |
| AG-* 节点代理 | 是（部分） | 端到端需 RK3588 |
