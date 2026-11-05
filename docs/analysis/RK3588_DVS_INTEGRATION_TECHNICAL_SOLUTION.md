# RK3588 应用与 DVS 应用融合技术方案

> 位置：`docs/analysis/RK3588_DVS_INTEGRATION_TECHNICAL_SOLUTION.md`
> 基准：仓库当前工作树（`feat/rk3588-apps` 之后的 DVS 阶段），MTL `26.09.0.DEV`
> 输入：`app/rk3588_live_ingest`、`app/rk3588_file_playout`、`app/rk3588_common`、`app/dvs_media`、`app/dvs_agent`、`app/dvs_platform`、`app/dvs_common`，以及 `docs/req`、`docs/plan`、`docs/analysis` 全部现有文档
> 目的：把 RK3588 媒体基座、DVS 节点媒体引擎、节点代理、平台服务四层的职责、复用边界、集成顺序与验收标准收敛成一份可执行的技术方案

---

## 1. 目标与范围

本方案回答四个问题：

1. RK3588 上的 MTL 媒体基座如何被 DVS 节点媒体引擎复用，而不是重新实现。
2. DVS 节点媒体引擎如何同时具备 output（解码/显示）与 input（采集/编码/发送）两类角色，从而支撑节点角色切换。
3. 节点代理与平台服务如何通过 IPC 与 operation 机制驱动媒体引擎，形成端到端闭环。
4. 共享代码、内核/驱动、PTP 时钟等外部约束如何纳入实施顺序，避免后期返工。

范围限定为 V1.0：单节点多窗口、两节点到四节点墙、prepare/commit 同帧、底图与音频归属。不包含 SIP、A/B 固件激活、多输出口等 P3/P4 任务，但保留扩展点。

非目标：不重新定义 REQ/PAGE 业务规则；不修改 `docs/req` 中的需求文本；不替代硬件/固件/测试签字。

---

## 2. 现状盘点

### 2.1 RK3588 媒体基座层

| 应用 | 角色 | 可复用程度 | 关键能力 |
|---|---|---|---|
| `rk3588_live_ingest` | 采集→编码→MTL TX，反向 MTL RX→解码→DRM | 高度模块化， donor chain | V4L2 DMA-BUF 采集、RGA 转换、MPP H.264/HEVC 编码、ST2110-22 TX、MTL sink 回调 |
| `rk3588_file_playout` | MP4 分发与多路接收、DRM 多平面合成、ALSA 播放 | RX 数据面 donor | ST2110-22/30 RX、码流重组、MPP 解码、播放缓冲/AV 同步、RGA 裁剪、DRM 合成、ALSA |
| `rk3588_common` | 基础无锁组件 | 已被三处复用 | `ring_buffer`、`slab_pool`、`timer`、`codec_plugin` 接口 |

三者都直接依赖 MTL、Rockchip MPP、libdrm、ALSA、FFmpeg。RK3588 平台只能软件 pacing，内核 AF_XDP native 模式受 YT6801 驱动限制，当前验证一律走 `kernel:` 后端。

### 2.2 DVS 应用层

| 应用 | 层级 | 状态 | 主要职责 |
|---|---|---|---|
| `dvs_media` | 节点媒体引擎（纯 C） | 单节点 MVP | 复用 file_playout RX 数据面；IPC 受控运行；白名单准入、布局 prepare/commit、音频仲裁、能力探测 |
| `dvs_agent` | 节点代理（C++17） | 单节点控制面 MVP | 六 scope 配置状态机、SQLite、注册/心跳、本机 HTTP、维护操作、媒体 IPC 转发 |
| `dvs_platform` | 平台服务（C++17） | 原型 | 通用 CRUD、墓碑、operation 屏障、布局像素拆分、串行升级入口 |
| `dvs_common` | 共享基础库 | 可用 | HTTP server/client、SHA256/PBKDF2、SQLite 封装、版本契约头 |

### 2.3 已确认的集成事实

- `dvs_media` 通过 meson in-tree 编译直接复用 `rk3588_file_playout` 的 `rx/*`、`shared/*`、`tx/mp4_demux` 与 `file_playout_config.c`，不是复制源码。
- `dvs_media` 的 RX 侧（output role）已经具备真实数据面；TX 侧（input role）仅参数校验与状态机，`dvs_tx_backend_fn` 为 NULL，启动后只报告 configured + backend_pending。
- `dvs_media` 与 `dvs_agent` 同包发布，版本契约在 `dvs_common/include/dvs_node_bundle.h`。
- `dvs_platform` 已具备布局拆分数学与 operation 屏障原型，但尚未真实下发 prepare/commit 到节点。

---

## 3. 目标架构

### 3.1 分层

```text
平台控制面 (dvs_platform)
  -> 节点网关 (WS/gRPC over TLS)
     -> 节点代理 (dvs_agent)
        -> IPC (Unix socket)
           -> 节点媒体引擎 (dvs_media)
              -> MTL (ST2110 收发/PTP/组播/pacing)
                 -> RK3588 MPP/RGA/DRM/ALSA/V4L2
```

职责边界：

- 平台：业务模型、快照、revision、operation、布局像素计算、审计。
- 节点代理：注册、心跳、配置状态机、网络/升级/本机页、媒体 IPC 转发。
- 媒体引擎：采集、编解码、组 multicast 收发、合成、音频、PTP 读取、DRM/ALSA 输出。
- MTL：ST2110 协议栈、PTP、组播、pacing、冗余。

### 3.2 进程隔离

节点代理与媒体引擎分进程。代理崩溃或升级时媒体继续出画；媒体崩溃时代理拉起并恢复最后布局；控制面与数据面线程分离，避免控制请求破坏 pacing。

### 3.3 单一 MTL 实例

一个进程只调用一次 `mtl_init`。媒体引擎的 RX session 与 TX session 必须共享同一 `mtl_handle`，分别作用于 RX port 与 TX port。这是 input/output 角色切换与双端口初始化的前置条件。

---

## 4. 复用与边界策略

### 4.1 in-tree 编译复用，不复制源码

继续沿用 `dvs_media` 的 in-tree 复用模式：meson 用相对路径把 donor 源码编进本工程静态库，再链接到可执行文件。好处是 donor 修复自动流入；代价是 include 路径与内部头强耦合， donor 重构会直接破坏 dvs_media。

约束：

- 不复制 donor 源码到 dvs_media 目录。
- donor 的内部头被 dvs_media 视为半公开 API，修改需同时编译 file_playout 与 dvs_media。
- `rx_compositor` 的真实来源是 `rk3588_file_playout/rx/rx_compositor.c`，不要与 `io/drm_renderer.c` 或 `shared/drm_renderer.c` 混淆。

### 4.2 三 donor 的分配

| donor | 引入 dvs_media 的部分 | 不引入的部分 |
|---|---|---|
| `rk3588_file_playout` | RX 数据面、合成器、解码器、音频、布局、PTP、配置 | TX 发流主流程、MP4 demux 仅 probe |
| `rk3588_live_ingest` | capture/encode/TX donor chain（规划中） | 其 `mpp_decoder.c`（避免与 file_playout 解码器冲突） |
| `rk3588_common` | `ring_buffer`、`slab_pool`、`timer` | 无 |

### 4.3 共享代码演进方向

短期接受 in-tree 强耦合；中期按 C/D 档把 `drm_mode`、`display_layout`、`ptp_time`、`e2e_stamp` 等稳定模块迁入 `rk3588_common`，让 dvs_media 与 file_playout 共享同一份。`drm_renderer`/`mpp_decoder` 的合并决策以 file_playout 接口为基准，end2end 做适配层。所有抽取必须保持两 app 编译与单测全绿、无行为 diff。

---

## 5. 数据面整合方案

### 5.1 Output 角色（已具备）

output 路径完全复用 file_playout RX 数据面：MTL RX → 码流重组 → MPP 解码 → 播放缓冲 → RGA 裁剪 → DRM 多平面合成 → HDMI/ALSA。dvs_media 在其上叠加受控层：白名单准入、两阶段布局、音频仲裁、能力上报。

### 5.2 Input 角色（待接入）

input 路径 donor 来自 `rk3588_live_ingest`：V4L2 采集 → RGA 转换 → MPP 编码 → MTL ST2110-22 TX。接入策略：

- 不重写 pipeline，复用 `tx_pipeline` 通过 `tx_mtl_sink_ops` 把编码结果喂给 MTL。
- `mtl_tx` 复用 dvs_media 已有的 `mtl_handle`，不新建 MTL 实例。
- `dvs_tx_backend_fn` 钩子在 backend 模块中实现 glue 逻辑，把 live_ingest 链路串起来。
- `tx_session` 保持纯参数校验 + 状态机，便于 x86 单测；backend 仅在单板 bring-up 注入。

### 5.3 角色互斥

`engine_ctx` 已实现 role 互斥：input 时停掉 RX compositor，output 时停掉 TX。两套数据面不会同时跑。角色切换时必须真实 teardown 旧链路（pipeline stop/free、mtl_tx stop/free），目前 teardown 为 TODO，是第一优先级补齐项。

### 5.4 MTL 双端口初始化

当前 `file_playout_mtl_init` 仅初始化 RX port。input 角色需要 TX port。方案：

1. 为 `file_playout_cfg` 增加 `tx_port`/`tx_lip` 字段。
2. `file_playout_mtl_init` 同时初始化 RX 与 TX port。
3. dvs_media 配置增加 tx_port/tx_lip。
4. `dvs_engine_tx_start` 确保 MTL 已初始化且含 TX port，再调用 backend。

---

## 6. 控制面与 IPC 方案

### 6.1 IPC 协议

Unix domain socket + JSON，消息带 `protocolVersion`、`messageId`、`nodeId`、`timestampTaiNs`。核心消息：`configure`、`prepare`、`commit`、`abort`、`clear`、`query_state`、`set_sources`，以及节点主动事件。

异步化要求：长连接、`messageId` 幂等、prepare 异步返回 202 + 事件、事件广播、write mutex。当前 state machine 已支持 stale revision、commit-before-prepare、并发拒绝、幂等 replay，但 IPC server 仍需重写为长连接模式。

### 6.2 节点代理

节点代理维护六 scope 配置状态机（SQLite），负责注册、心跳、本机 HTTP、维护操作、媒体 IPC 转发。媒体引擎不可达时必须透传结构化错误，不崩溃、不返回空状态。

### 6.3 平台 operation

平台基于 revision + operation 实现 prepare 屏障与 commit 广播：

1. 下发 prepare 到所有参与节点。
2. 收集 ready 回执，任一 blocked 则不 commit。
3. 计算 `apply_at = now + guard`（guard = max(2/fps, 50ms)）。
4. 广播 commit，节点在目标帧边界原子提交。
5. 聚合 applied 回执，任一失败则 rollback 到 revision-1。

平台只有全部 applied 后才更新当前场景。

---

## 7. 跨格与同帧方案

### 7.1 全局坐标拆分

建墙时固化格位矩阵 `cell[r][c] = {x,y,w,h}`。节点 N 绑定格位集合，本地画布原点取包围盒左上角。每个窗口按交集生成 plane 的 dst/src 矩形，使用半开区间避免缝隙，左/上取整向下、右/下取整向上，源裁剪限制在 `[0, source]`。

### 7.2 同帧机制

- 全网一个 PTP 域，节点读取 `CLOCK_TAI`。
- 定义 epoch 与 fps，全局帧号 `frame(t)=floor((t-epoch)*fps)`。
- prepare 阶段：加入组播、创建 RX 会话与解码器、等待首帧、预分配 DRM plane 与影子布局。
- commit 阶段：把 apply_at 映射到目标帧号，在帧边界原子提交，新布局在帧 N 生效。
- 时钟资格：PTP synced、偏差 ≤2ms、帧号一致、解码就绪、节点在线；不满足则 blocked。

### 7.3 影子副本与 rollback

commit 前保留旧布局影子副本。commit 后若任一节点未完成，平台下发 rollback 到 revision-1。提交路径必须控制在"切 plane 属性"，避免解码器重建导致不可预测延迟。

---

## 8. 构建、部署与内核约束

### 8.1 构建

- MTL、RK3588 apps、dvs_media 必须在编译服务器构建；dvs_agent/dvs_platform 可本地快速验证。
- dvs_media 是独立 meson 工程，MPP 为必需依赖。
- `rk3588_common` 的 MTL 依赖通过 `HAVE_MTL` 条件编译。

### 8.2 内核与驱动

- AF_XDP native 模式受 YT6801 驱动限制，当前走 `kernel:` 后端。
- 需确认 DMA32 堆可用，避免 RGA 大于 4G 地址告警。
- PTP/PHC 配置与 `linuxptp` 需在板端验证。
- MtlManager 与 `xdp-dispatcher.o` 需随镜像固化。

### 8.3 部署约束

- hugepages、MTL mempool ops、DPDK 插件需固化到镜像。
- 节点进程由 systemd 拉起，媒体崩溃 30 秒内恢复最后布局。
- 节点本机页 HTTPS，密码哈希，登录限速。

---

## 9. 分阶段实施计划

映射到现有任务 ID，避免重复造轮子：

| 阶段 | 目标 | 关键任务 | 退出标准 |
|---|---|---|---|
| P0 基座验证 | MTL 在 RK3588 可运行 | K1/K4、V1-V6、B1-B6 | 单路 30 分钟稳定，PTP 偏差达标 |
| P1 媒体引擎 MVP | 单节点 4 路热更新 | ME-1~ME-8、A1~A2、S0/S1 | 4 路 1080p60 同时显示，窗口可移动缩放 |
| P2 平台控制面 | 两节点 2×1 墙闭环 | PL-1~PL-5、AG-1~AG-7、S2/S3 | 建组→建墙→绑定→白名单→开窗→存场景 |
| P3 同帧与跨格 | 2×2 墙 1000 次切换 | ME-5/ME-7、PL-6/PL-7、S4 | 偏差 ≤1 帧，离线整墙 blocked，失败 rollback |
| P4 运维与规模化 | 128 节点/4×4 墙 | PL-8、S5、V9 | NFR 达标，24h 稳定 |

输入角色接入（live_ingest donor）插入 P1/P2 之间：先完成 MTL 双端口初始化与 meson 接入，再实现 backend glue 与 teardown，最后单板冒烟。

---

## 10. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 千兆带宽不足 | 高 | 先做码流实测，必要时降路数或升级网口 |
| H.264 GOP 导致切换超时 | 高 | 短 GOP、常驻订阅、全 intra 可选 |
| 跨屏同帧缺少实测 | 高 | PTP + 帧标记实测，必要时降级验收 |
| DRM plane 数量不足 | 中 | GPU/RGA 合成兜底，限制单节点窗口数 |
| MPP 并发解码/温升 | 中 | 实测解码路数，动态淘汰 |
| ARM 只能软件 pacing | 中 | 闭环局域网可接受，广播设备另评 |
| in-tree 强耦合蔓延 | 中 | 持续记录共享边界， donor 重构同步验证 |
| MTL 参考应用未产品化 | 中 | 抽公共库、补测试、固定上游版本 |
| AF_XDP/DPDK 配置复杂 | 中 | 镜像预配置，systemd capability |

---

## 11. 验收标准

- 单节点 4 路 1080p60 同时显示，窗口可移动、缩放、调层序，音频跟随指定窗口。
- 两节点 2×1 墙完整走通 FLOW-01，场景保存/切换/清屏符合 REQ。
- 2×2 墙 1000 次场景切换无可见撕裂，平台记录偏差 ≤1 帧。
- 任一节点离线时整墙阻止，画面不变；commit 失败自动 rollback。
- 媒体引擎重启 30 秒内恢复最后布局；断控制网络 30 分钟内画面不黑。
- input 角色可捕获→编码→MTL TX 出流，output 节点可接收并显示。
- 节点能力上报为真实枚举值，query_state 返回真实计数。

---

## 12. 待决策项

| 决策 | 建议 |
|---|---|
| 压缩或未压缩 | 当前选择压缩(ST22)，如果网络传输无瓶颈可以考虑改为不压缩(ST20) |
| 编码格式 | H.264 1080p，HEVC 4K；有 JPEG XS 硬件时优先 |
| GOP 策略 | 0.5–1s，可配置 |
| 组播模式 | SSM，指定源 |
| PTP 来源 | 外部 linuxptp + CLOCK_TAI |
| 合成方式 | DRM plane 优先，GPU/RGA 兜底 |
| 节点代理语言 | C++（已落地） |
| 控制通道 | WS + JSON（已落地） |
| MTL 维护方式 | fork + 固定 tag + 补丁 |
| 单节点多输出口 | V1.0 单口优先 |

---

## 13. 结论

DVS 的业务复杂度在控制面，技术风险在媒体面的跨节点同帧与跨格拼接。RK3588 apps 已提供 MTL 媒体基座与 donor 数据面；DVS apps 已提供受控运行与平台控制面原型。下一步应优先补齐 input 角色数据面接入与 MTL 双端口初始化，然后按 P1→P3 顺序推进，始终保持 in-tree 复用、不复制源码、不破坏 donor 编译。
