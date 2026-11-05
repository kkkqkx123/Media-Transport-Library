# dvs_media 公共化与 live_ingest TX 集成：完成状态与剩余项

> 输入：`docs/analysis/DVS_MEDIA_RK3588_INTEGRATION.md`、
> `docs/analysis/RK3588_DVS_INTEGRATION_TECHNICAL_SOLUTION.md`、
> `docs/plan/DVS_MEDIA_INTEGRATE_LIVE_INGEST.md`。
> 本文是前三者的**决策收敛与实施状态回写**：在原设计基础上，按当前代码库状态更新完成情况、
> 偏差与剩余项。实施中若与知识库/代码矛盾，以代码为准。

## 1. 结论先行

- **common 迁移第一批次已完成**：`sps_vui`、`sei_timestamp`、`display_layout`、`drm_mode`、`ptp_time`
  已 git mv 至 `rk3588_common`，donor 内不留副本，dvs_media 与两个 donor app 同源编译通过。
- **live_ingest TX 链已接入 dvs_media**：新增 `dvs_tx` 静态库 in-tree 编入 capture/encode/TX 链，
  backend 已实现，input 角色可走真实数据面（RK3588 单板验证仍待做）。
- **MTL 双端口初始化已落地**：`file_playout_config.c` 新增 `file_playout_mtl_init_dual`，
  dvs_media 单进程同时承载 RX 与 ST22 TX 会话，单/双端口行为兼容。
- **backend 归属按设计落地**：`tx_session` 保持纯状态机，数据面句柄由 engine 与
  `engine/tx_backend_live_ingest.c` 持有；teardown 在角色切换、TX 停止、进程退出三处对称执行。
- **V4L2/HDMI 稳定映射已实现**：`engine/tx_port_map.c` + `capability.c` 共用同一套物理身份扫描，
  capability 上报 `device/stableId/signal` 四元组，backend 启动前重扫并拒绝无信号目标。
- **E2E 时间戳互通**：已在 RX 侧增加 SEI NAL 回退解析（`mtl_rx_bridge.c`），
  live_ingest TX 无需改动；MP4 分发仍使用 `e2e_stamp` 头。音频 TX 本期不实现；
  第二批其余 common 模块仍延后；单板冒烟与互通实测待执行。

## 2. 完成情况矩阵

| 设计章节 | 状态 | 代码落点 |
|---|---|---|
| §4.1 第一批 common 迁移 | 已完成 | `app/rk3588_common/src/{sps_vui,sei_timestamp,display_layout,drm_mode,ptp_time}.c` |
| §4.2 第二批模块延后 | 按计划延后，未迁移 | 分歧记录在 `docs/analysis`，未行动 |
| §4.3 留在 donor | 已遵守 | `tx_pipeline`、`mtl_tx`、`v4l2_capturer`、`rga_converter`、`mpp_encoder` 等仍位于 live_ingest |
| §5.1 MTL 双端口初始化 | 已完成（函数形态有偏差，见 §6） | `file_playout_config.c:file_playout_mtl_init_dual` |
| §5.2 backend 归属 | 已完成 | `engine/tx_backend.h`, `engine/tx_backend_live_ingest.c`, `engine_ctx.c` |
| §5.3 参数映射 | 已完成 | `tx_backend_live_ingest.c`（codec/bitrate/GOP/slice/默认值） |
| §5.4 meson 接入与 x86 保护 | 已完成 | `app/dvs_media/meson.build`（`has_tx_backend` 条件） |
| §5.5 音频 TX 范围 | 按计划未实现 | `dvs_tx_params_t` 仅视频；backend 显式注明 out of scope |
| §9.1 V4L2/HDMI 稳定映射 | 已完成 | `engine/tx_port_map.c`, `engine/capability.c` |
| §9.2 音频 TX | 未实现（触发条件未满足） | 无 ALSA 采集 donor，留待后续 |
| §9.3 第二批统一条件 | 仅分析，未执行 | 无代码变更 |
| E2E 互通 | RX 回退已实现 | RX 先试 `e2e_stamp`，失败再试 SEI NAL 并剥离 |

## 3. 现状盘点（更新）

供给侧（donor 持有，与原盘点一致，第一批次已迁移）：

- `rk3588_common`：`ring_buffer`、`slab_pool`、`timer`，头文件 `codec_plugin.h`、`pipeline_common.h`，
  plus 已迁移的 `sps_vui`、`sei_timestamp`、`display_layout`、`drm_mode`、`ptp_time`。
- `rk3588_live_ingest`：TX 链 `io/v4l2_capturer`、`codec/rga_converter`、`codec/mpp_encoder`、
  `codec/sps_vui`（源已搬，include 仍经 common）、`pipeline/tx_pipeline`（经 `tx_mtl_sink_ops`
  对接 `mtl_tx`）、HAL 的 `dma_allocator`/`dma32_allocator`/`sei_timestamp`（源已搬）。
- `rk3588_file_playout/shared`：`display_layout`、`drm_mode`、`ptp_time` 源已搬；
  `mpp_decoder`、`drm_renderer`、`e2e_stamp`、`au_trace`、`rx_e2e_stats`、`tx_tai_ts` 仍 donor。
- `rk3588_file_playout/rx`：RX 数据面全套，dvs_media output 角色复用。

需求侧：

- output 角色：已闭环，无新增需求。
- input 角色：参数校验 + 状态机 + 真实 backend 已接通；RK3588 单板冒烟与对端收流验证待执行。

## 4. 上移 common 的判定准则（不变）

common 只收“共享、稳定、无应用策略”的模块，硬件相关不再是排除条件。判定三条：

1. 至少被两个应用使用；
2. 行为稳定（接口与语义已收敛）；
3. 无应用策略（不规定线程归属，不做布局/平面/准入/会话编排决策）。

## 5. 搬迁清单状态

### 5.1 第一批（已完成）

- `codec/sps_vui`、`hal/sei_timestamp`（live_ingest）→ `rk3588_common/src`
- `shared/display_layout`、`shared/drm_mode`、`shared/ptp_time`（file_playout）→ `rk3588_common/src`

搬迁方式为源码级移动（git mv），donor 内不留副本；两个 app 的 meson 与 include 路径已同步切换；
`dvs_shared`/`dvs_tx` 静态库直接编入 common 源。完成标准（双 app 编译 + 纯 C 自测）已达成于 aarch64 构建。

### 5.2 第二批（延后，未动）

- 两份 `mpp_decoder`：投喂/背压语义不同，TX 路径不触及 decoder，dvs_media 继续用 file_playout 那份。
- 三处 DRM 渲染：平面分配与布局策略不同，是策略不是工具，不进 common。
- 三套 E2E 时间戳：线格式互不兼容，合并等于改线协议；本期只记录分歧。
- `au_trace`、`rx_e2e_stats`、`tx_tai_ts`：默认留在 file_playout。

### 5.3 留在 donor（状态未变）

- `tx_pipeline`、`rx_pipeline`、`rx_compositor`：自带线程模型与重建/统计策略。
- `mtl_tx`/`mtl_rx` 系列会话包装：参数形状与 app 的端口/队列规划绑定。
- `v4l2_capturer`、`rga_converter`、`mpp_encoder`：单消费者（live_ingest），未满足准则第 1 条。
- `dma_allocator`/`dma32_allocator`：无策略候选，但源仍位于 live_ingest；dvs_media 通过 `dvs_tx` in-tree 复用，
  未上移。触发条件（第二个消费者）已满足，但上移未排期，本期接受。
- `alsa_audio`、`av_sync`、`playout_buffer`：单消费者且内嵌 RX 链路策略。

## 6. live_ingest 集成状态与偏差

### 6.1 MTL 双端口初始化（已完成，形态有偏差）

原设计要求改写 `file_playout_mtl_init` 的单端口写死逻辑。实际实现为**同文件新增**
`file_playout_mtl_init_dual`，保留单端口 `file_playout_mtl_init` 给独立 TX/RX 应用：

- 双端口函数按 cfg 的 `tx_port`/`rx_port` 规划注册 P/R 两个端口；同口时退化为单端口；
  缺失一侧不注册；与原设计的“端口缺省的一侧不注册”一致。
- dvs_media 在 `dvs_engine_init_mtl`、`dvs_engine_apply_layout_at`、`dvs_engine_tx_start` 中均调用
  `file_playout_mtl_init_dual(&e->cfg, 1, 1, ...)`，避免二次 `mtl_init`。
- 单/双端口行为兼容：file_playout TX/RX 独立应用仍走单端口路径，回归不受影响。

偏差说明：原设计的“不另起模块、直接改原函数”在函数粒度上放宽为“同模块新增对偶函数”，
未新增模块，符合架构规则；单端口原函数保留是为了不破坏独立应用。

### 6.2 backend 归属（已完成）

- `tx_session` 保持纯状态机：只做参数校验与状态机，不知道 backend 的存在（除函数指针类型外），
  现有 x86 单测零改动。
- `engine/tx_backend_live_ingest.c` 归 engine 所有：把 `dvs_tx_params_t` 映射到 live_ingest 组件参数，
  创建并串联 `mtl_tx` 与 `tx_pipeline`，持有运行期句柄，提供 `dvs_tx_backend_stop` 对称释放入口。
- `engine_ctx` 的角色切换与启停入口调用该释放入口；停止操作幂等、可重入。

注意：`dvs_tx_stop`（session 层）本身仍是状态清理，不直接调用 backend；
engine 层在 teardown 路径显式先调 `dvs_tx_backend_stop()` 再调 `dvs_tx_stop(&e->tx)`。
这是设计明确的“engine 持句柄、session 保纯洁”分工，不是遗漏。

### 6.3 参数映射（已完成）

- 捕获端口：`HDMIk` 经 `tx_port_map` 解析为 `/dev/videoN`，与 capability 的输入口探测共用同一枚举逻辑。
- 编解码：`dvs_tx_codec_from_name` 集中映射，非法值在 backend 入口即拒绝。
- 分辨率/帧率直通捕获器、RGA 转换器、编码器与 MTL 会话四方一致。
- 码率 kbps→bps；GOP 取帧率同量级；slice 数/RGA 池深/捕获缓冲数取工程默认值
  （`DVS_TX_DEF_*`），后续按实测调优，未在设计中锁定。
- PTP 时间源复用引擎已有 MTL 句柄，与 output 侧取时方式一致。

### 6.4 meson 接入与 x86 保护（已完成）

- `dvs_tx` 静态库 in-tree 编入 live_ingest 的 HAL（DMA 分配、SEI）、IO（V4L2 捕获）、
  Codec（RGA 转换、MPP 编码、SPS/VUI）、`pipeline/tx_pipeline`、`mtl_tx`；应用编排层与解码器不引入。
- 新增 `librga` 依赖；include 路径追加 live_ingest 根与 codec 目录。
- x86 保护：backend 源文件仅在 MPP 与 RGA 均可用时编入；缺失时引擎编译为无 backend 形态，
  启动 TX 诚实报告 configured+pending；`tx_session` 单测在任何机器可跑。

### 6.5 音频 TX（按计划未实现）

`dvs_tx_params_t` 当前仅视频；input 角色的音频发送（ST2110-30）本期未实现。
backend 对音频字段预留但不实现，视频链路不受影响。触发条件与实施方案见 §9.2。

## 7. V4L2/HDMI 稳定映射（已完成，细节）

- 稳定 ID 定义为物理身份（采集卡 card + bus_info + 输入索引），而非设备号；
  进程启动时枚举建表，`/dev/videoN` 仅作为运行期路径。
- 能力上报每个输入口给出四元组：`port`（HDMIk，agent 兼容）、`device`（当前路径，诊断）、
  `signal`（占位分辨率与无信号语义）、`stableId`（物理身份字符串）。
- backend 解析顺序：稳定 ID 优先，设备路径回退；目标端口无信号时直接拒绝启动并附可用端口表，
  不静默误用。
- 刷新：TX 启动前重扫设备表；信号中断走捕获器已有的 signal-change 回调重建链路。
- 映射表构造逻辑抽为探测与 backend 共用的纯函数（`dvs_tx_table_build`/`dvs_tx_resolve_device`），
  可在 x86 单测表构造（缺号、重排、空表）。

## 8. 实施顺序执行情况

1. MTL 双端口初始化：完成（§6.1）。
2. 第一批 common 搬迁：完成（§5.1）。
3. meson 接入 `dvs_tx`：完成（§6.4）。
4. backend 实现与 teardown：完成（§6.2、§6.3）。
5. RK3588 单板冒烟：**未执行**（本环境无单板；需在编译服务器/单板执行）。
6. 第二批模块记录分歧：完成（仅文档，无代码）。

## 9. 验收状态

- x86：`tx_session` 单测与 `tx_port_map` 单测存在；common 纯 C 自测位于 donor；
  缺 MPP/RGA 的机器上 dvs_media 仍可编译为无 backend 形态。
- 编译服务器：dvs_media aarch64 构建已成功（build/ 目录存在，含 libdvs_tx）；
  file_playout TX/RX 独立应用行为无变化（仍走单端口路径）。
- 单板：input 出流、对端收流、output/input 往返切换、E2E 度量**未验证**。
- 互通：input 发→output 收时 RX 侧 E2E 统计**未验证**，缺口仍存在（§10.3）。

## 10. 开放问题与剩余任务

### 10.1 V4L2/HDMI 稳定映射

已完成（§7），无剩余。

### 10.2 input 角色音频 TX

本期不实现，触发条件与实施方案保持：

- 触发条件：业务确认 input 节点需要带音频，并明确采样率、声道数、目标端口三要素。
- 实施方案：参数与 IPC 加 audio 子对象；本期 backend 遇到音频字段返回明示不支持且不影响视频链路；
  数据面新增 ALSA 采集到 ST30 发布链，会话包装 in-tree 复用 file_playout 的 `tx/mtl_tx_audio`，
  采集线程新建，时钟取 MTL PTP 与视频同源；MTL 双端口初始化已覆盖 TX 队列，音频会话建在同一句柄上。
- 状态查询加 audio 状态（缺省 absent），验收以 input→output 回环的音画同步为准。

### 10.3 第二批模块与 E2E 互通

- 解码器两份分歧（投喂粒度、主动拉取、EOS/复位、分辨率变更、统计口径）：以 file_playout 实现为基准，
  live_ingest 侧适配；TX 集成不受影响（只用编码器）。
- DRM 渲染器三份职责不同，本体不合并；合并触发条件仅为实证的同源 plane 竞争缺陷。
- E2E 时间戳：互通已按 “RX 侧加 SEI 解析” 方案落地——`mtl_rx_bridge.c`
  先试 `e2e_stamp` 头，失败再试 SEI NAL 并原地剥离；live_ingest TX 与 MP4 分发 TX
  均无需改动。单板冒烟仍待验证 E2E 度量有数。

### 10.4 仍留 donor 的候选上移

- `dma_allocator`/`dma32_allocator`：触发条件已满足（dvs_tx 第二消费者），但上移未排期；
  继续 in-tree 引用。若上移，按 common 三条准则执行，源码级移动、不留副本。

## 11. 风险与未决

1. 单板冒烟未执行：backend 代码路径仅经 aarch64 编译验证，运行时行为（V4L2 信号、RGA/MPP、MTL TX）待实测。
2. E2E 互通未验证：§10.3 的 SEI vs `e2e_stamp` 缺口可能使 RX 侧 E2E 度量无数，需实测后二选一。
3. in-tree 强耦合蔓延：dvs_media 现绑定 file_playout 与 live_ingest，修改任一方内部头都会波及；
   共享边界需在 `docs/analysis` 持续记录。
4. RGA 依赖：dvs_media 此前无 `librga` 依赖；集成后 RGA 与 MPP 同级别必需（`has_tx_backend` 为 false 时仍可编译无 backend）。

## 12. 与初版方案的关系

本设计继承初版方案（`DVS_MEDIA_INTEGRATE_LIVE_INGEST.md`）的复用策略、组件取舍表与角色互斥流程，
修正与补充：

- §4.4 双端口：由“扩展配置字段加新初始化路径”的模糊描述，明确为同模块新增
  `file_playout_mtl_init_dual`，单/双端口兼容（§6.1）。
- §4.5 上下文传递：由“模块级全局保存 engine 上下文”，明确为句柄归 engine 持有、
  session 不感知数据面（§6.2）。
- §6 风险第 2/3 条（decoder/hal 重复）：按 §5.2/§5.3 收敛为“本期不统一”，只记录分歧，不行动。