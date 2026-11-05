# RK3588 应用与 DVS 应用融合技术方案

> 位置：`docs/analysis/RK3588_DVS_INTEGRATION_TECHNICAL_SOLUTION.md`
> 基准：仓库当前工作树（`feat/rk3588-apps` 之后的 DVS 阶段），MTL `26.09.0.DEV`
> 输入：`app/rk3588_live_tx`、`app/rk3588_playout_rx`、`app/rk3588_common`、`app/dvs_media`、`app/dvs_agent`、`app/dvs_platform`、`app/dvs_common`，以及 `docs/req`、`docs/plan`、`docs/analysis`
> 目的：把 RK3588 媒体基座、DVS 节点媒体引擎、节点代理、平台服务四层的职责、生产架构、实施顺序与验收标准收敛成一份可执行的技术方案

---

## 1. 目标与范围

本方案回答四个问题：

1. RK3588 上的 MTL 媒体基座如何支撑 DVS 节点媒体引擎。
2. 节点媒体引擎如何同时具备 input（采集/编码/发送）与 output（接收/解码/显示）两类角色，并支持运行时切换。
3. 节点代理与平台服务如何通过 IPC 与 operation 机制驱动媒体引擎，形成端到端闭环。
4. PTP 时钟、内核/驱动、共享代码等外部约束如何纳入实施顺序。

范围限定为 V1.0：单节点多窗口、两节点到四节点墙、prepare/commit 同帧、底图与音频归属。SIP、A/B 固件激活、多输出口等 P3/P4 任务保留扩展点，不作为本方案主体。

---

## 2. 现状（开发与过渡形态）

当前仓库存在三个 RK3588 相关应用：

- `rk3588_live_tx`：HDMI 实时采集、RGA 转换、MPP 编码、MTL TX 的编码发送端（TX-only）；亦支持文件帧源插件（MP4 解复用 → extradata → SEI → 同一发送宿接口 + ST30 音频）。不收流，回环验证由 `rk3588_playout_rx` 进程经网络完成。
- `rk3588_playout_rx`：MTL 接收、MPP 硬解码、播放缓冲、AV 同步、DRM 多平面合成、ALSA 播放的解码接收端（RX-only）；发送端由 `rk3588_live_tx` 经网络供给。
- `dvs_media`：由 `rk3588_playout_rx` RX 侧演进的节点媒体引擎，受 IPC 控制；output 角色基本可用，input 角色已接入 `rk3588_live_tx` 的 TX 数据面，但 teardown、能力门控与切换验证仍不完整。

当前 `dvs_media` 通过 in-tree 编译复用 `rk3588_playout_rx` 的 RX 数据面与 `rk3588_live_tx` 的 TX 数据面，output 角色复用 file_playout RX 数据面，input 角色通过 `rk3588_live_tx` 提供 capture/encode/TX 能力。这种复用方式属于开发与过渡形态，用于加速 bring-up 与验证。

平台与节点代理当前通过 HTTP REST over TCP 交互；WS/gRPC 是目标架构，不是当前实现。单板回环通过分别启动发送/接收进程完成，不作为生产交付；`both` 自环模式已移除。

---

## 3. 最终生产架构

### 3.1 分层

```text
平台控制面 (dvs_platform)
  -> 节点网关 (目标: WS/gRPC over TLS；当前: HTTP REST over TCP)
     -> 节点代理 (dvs_agent)
        -> IPC (Unix socket)
           -> 节点媒体引擎 (dvs_media)
              -> MTL (ST2110 收发/PTP/组播/pacing)
                 -> MPP / RGA / DRM / ALSA / V4L2
```

职责边界：

- 平台：业务模型、快照、revision、operation、布局像素计算、审计。
- 节点代理：注册、心跳、配置状态机、网络/升级/本机页、媒体控制转发。
- 媒体引擎：采集、编解码、组播收发、合成、音频、PTP 读取、DRM/ALSA 输出。
- MTL：ST2110 协议栈、PTP、组播、pacing、冗余。

### 3.2 进程隔离

节点代理与媒体引擎分进程。代理崩溃或升级时媒体继续出画；媒体崩溃时代理拉起并恢复最后布局；控制面与数据面线程分离，避免控制请求破坏 pacing。

### 3.3 单一媒体包与运行时角色

最终节点使用单一 `dvs_media` 包，运行时选择 input 或 output 角色。角色互斥：一个节点同一时刻只运行采集/编码/TX 或接收/解码/合成/显示中的一组数据面。MTL 在进程内初始化一次，同时持有 RX 与 TX 端口，供角色切换复用。

共享基础设施通过正式库依赖提供。生产构建不采用源码目录级 in-tree 复用作为最终方式。rk3588 app 保留为验证与回归入口，不作为生产交付物。

单屏可合成显示的路数由 MPP 解码能力、DRM plane 数量与内存决定；物理 HDMI 输出口数量只决定多屏输出能力，不等同于单屏合成路数。

---

## 4. 数据面

### 4.1 Output 角色

MTL RX → 码流重组 → MPP 解码 → 播放缓冲 → RGA 裁剪 → DRM 多平面合成 → HDMI/ALSA。媒体引擎在数据面之上提供白名单准入、两阶段布局、音频仲裁、能力上报。

### 4.2 Input 角色

V4L2 采集 → RGA 转换 → MPP 编码 → MTL ST2110-22 TX。生产实现应作为媒体引擎内部数据面模块，复用共享采集/编码/传输组件，而不是依赖独立 rk3588 app。

### 4.3 角色切换

角色切换必须完整释放旧数据面资源，再启动新数据面。MTL 实例保持可用，避免重复初始化。能力不足时拒绝切换并返回明确错误。

---

## 5. 控制面与 IPC

### 5.1 IPC 协议

Unix domain socket + JSON，消息带 `protocolVersion`、`messageId`、`nodeId`、`timestampTaiNs`。核心消息：`configure`、`prepare`、`commit`、`abort`、`clear`、`query_state`、`set_sources`，以及节点主动事件。

要求长连接、`messageId` 幂等、prepare 异步返回 202 + 事件、事件广播、write mutex。状态机支持 stale revision、commit-before-prepare、并发拒绝、幂等 replay。

### 5.2 平台 operation

1. 下发 prepare 到所有参与节点。
2. 收集 ready，任一 blocked 则不 commit。
3. 计算 `apply_at = now + guard`（guard = max(2/fps, 50ms)）。
4. 广播 commit，节点在目标帧边界原子提交。
5. 聚合 applied，任一失败则 rollback 到 revision-1。

平台仅在全部 applied 后更新当前场景。

---

## 6. 跨格与同帧

### 6.1 全局坐标拆分

建墙时固化格位矩阵 `cell[r][c] = {x,y,w,h}`。节点绑定格位集合，本地画布原点取包围盒左上角。窗口按交集生成 plane 的 dst/src 矩形，使用半开区间，左/上向下取整、右/下向上取整，源裁剪限制在 `[0, source]`。

### 6.2 同帧机制

- 全网一个 PTP 域，节点读取 `CLOCK_TAI`。
- 定义 epoch 与 fps，全局帧号 `frame(t)=floor((t-epoch)*fps)`。
- prepare 阶段：加入组播、创建 RX 会话与解码器、等待首帧、预分配 DRM plane 与影子布局。
- commit 阶段：把 apply_at 映射到目标帧号，在帧边界原子提交，新布局在帧 N 生效。
- 时钟资格：PTP synced、偏差 ≤2ms、帧号一致、解码就绪、节点在线；不满足则 blocked。

### 6.3 影子副本与 rollback

commit 前保留旧布局影子副本。commit 后若任一节点未完成，平台下发 rollback 到 revision-1。提交路径控制在切 plane 属性，避免解码器重建导致不可预测延迟。

---

## 7. 构建、部署与内核约束

这些约束来自 RK3588 平台的硬件、驱动与实时性要求，直接影响部署可行性与长期稳定性。

### 7.1 构建

媒体引擎、代理、平台服务按各自工程构建。共享组件通过正式依赖提供。RK3588 目标依赖 MTL、Rockchip MPP、libdrm、ALSA、FFmpeg、json-c。

### 7.2 内核与驱动

- AF_XDP native 模式受 YT6801 驱动限制时走内核后端。
- 确认 DMA32 堆可用，避免 RGA 大于 4G 地址告警。
- PTP/PHC 与 `linuxptp` 需在板端验证。
- MtlManager 与 `xdp-dispatcher.o` 需随镜像固化。

### 7.3 部署

- 镜像按节点能力与配置包含统一媒体包。
- hugepages、MTL mempool ops、DPDK 插件固化到镜像。
- 节点进程由 systemd 拉起，媒体崩溃 30 秒内恢复最后布局。
- 节点本机页 HTTPS，密码哈希存储，登录限速。

---

## 8. 实施计划

实施计划按依赖关系推进：基座验证优先，媒体引擎次之，平台控制面与同帧能力最后落地。这样可以尽早暴露硬件与 MTL 风险。

| 阶段 | 目标 | 退出标准 |
|---|---|---|
| P0 基座验证 | MTL 在 RK3588 可运行 | 单路 30 分钟稳定，PTP 偏差达标 |
| P1 媒体引擎 MVP | 单节点 4 路热更新 | 单屏 4 路 1080p60 合成显示，窗口可移动缩放 |
| P2 平台控制面 | 两节点 2×1 墙闭环 | 建组→建墙→绑定→白名单→开窗→存场景 |
| P3 同帧与跨格 | 2×2 墙 1000 次切换 | 偏差 ≤1 帧，离线整墙 blocked，失败 rollback |
| P4 运维与规模化 | 128 节点/4×4 墙 | NFR 达标，24h 稳定 |

输入角色接入在 P1/P2 之间完成：先 MTL 双端口初始化与数据面模块接入，再 backend glue 与 teardown，最后单板冒烟。

---

## 9. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 千兆带宽不足 | 高 | 先做码流实测，必要时降路数或升级网口 |
| H.264 GOP 导致切换超时 | 高 | 短 GOP、常驻订阅、全 intra 可选 |
| 跨屏同帧缺少实测 | 高 | PTP + 帧标记实测，必要时降级验收 |
| DRM plane 数量不足 | 中 | GPU/RGA 合成兜底，限制单节点窗口数 |
| MPP 并发解码/温升 | 中 | 实测解码路数，动态淘汰 |
| ARM 只能软件 pacing | 中 | 闭环局域网可接受，广播设备另评 |
| 共享组件边界漂移 | 中 | 稳定模块迁入共享库，保持编译与测试全绿 |
| MTL 参考应用未产品化 | 中 | 抽公共库、补测试、固定上游版本 |
| AF_XDP/DPDK 配置复杂 | 中 | 镜像预配置，systemd capability |

---

## 10. 验收标准

验收标准覆盖功能、时延、时钟、稳定性与恢复能力，用于判断系统是否达到 V1.0 可用状态。

- 单屏 4 路 1080p60 合成显示（物理 HDMI 输出口数量以实际硬件为准），窗口可移动/缩放/调层序，音频跟随指定窗口。
- 两节点 2×1 墙走通 FLOW-01，场景保存/切换/清屏符合 REQ。
- 2×2 墙 1000 次切换无可见撕裂，平台记录偏差 ≤1 帧。
- 任一节点离线整墙阻止、画面不变；commit 失败自动 rollback。
- 媒体引擎重启 30 秒内恢复最后布局；断控制网络 30 分钟内画面不黑。
- 输入与输出角色均可运行，角色切换具备资源释放与能力门控。
- 能力上报为真实枚举值，`query_state` 返回真实计数。

---

## 11. 待决策项

| 决策 | 概念说明 | 建议 |
|---|---|---|
| 压缩或未压缩 | ST2110-22 压缩视频带宽低但受编码/GOP 影响；ST2110-20 未压缩时延最低但带宽高。 | V1.0 压缩，万兆可选未压缩 |
| 编码格式 | H.264 兼容性好；HEVC 压缩率更高；JPEG XS 时延低但依赖硬件支持。 | H.264 1080p，HEVC 4K；有 JPEG XS 硬件时优先 |
| GOP 策略 | 关键帧间隔影响切换等待时间与带宽。 | 0.5–1s，可配置 |
| 组播模式 | SSM 可过滤非法源，安全性更高；ASM 配置更简单。 | SSM，指定源 |
| PTP 来源 | 统一时基来源，影响同帧精度与部署复杂度。 | 外部 linuxptp + CLOCK_TAI |
| 合成方式 | DRM plane 高效但受硬件数量限制；GPU/RGA 可兜底更多窗口。 | DRM plane 优先，GPU/RGA 兜底 |
| 节点代理语言 | 影响开发效率、镜像体积与团队技能要求。 | C++（已落地） |
| 控制通道 | HTTP REST 易调试；WS/gRPC 支持长连接与推送。 | WS + JSON（目标）/ HTTP REST（当前） |
| MTL 维护方式 | fork + 固定 tag + 补丁可控但需自行维护；跟踪上游升级快但风险高。 | fork + 固定 tag + 补丁 |
| 单节点多输出口 | 多输出口支持多显示，但增加绑定与能力校验复杂度。 | V1.0 单口优先 |

---

## 12. 结论

- 当前阶段：`dvs_media` 已具备 output 角色并接入 input TX 数据面；rk3588 app 独立存在，用于验证与回归。
- 最终架构：单一 `dvs_media` 包运行时选角色，MTL 一次初始化长期持有双端口；共享组件通过正式库依赖提供；rk3588 app 不合并，仅作验证入口。
- 下一步：优先补齐角色切换 teardown、能力门控与切换验证，按 P1→P3 推进，保持数据面与控制面进程分离。
