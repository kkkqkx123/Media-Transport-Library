# RK3588 应用是否应该合并的分析

> 分析对象：`app/rk3588_live_tx`、`app/rk3588_playout_rx`、`app/dvs_media`、`app/rk3588_common`
> 结论先行：**不建议把三个应用合并为一个可执行文件或一个源码目录；建议保持按职责分离的应用边界，把共同数据面模块收敛到共享库，并由 `dvs_media` 作为生产集成点按角色接入 donor 链路。**

---

## 1. 问题定义

当前存在三个 RK3588 相关应用：

- `rk3588_live_tx`：HDMI 实时采集 → RGA → MPP 编码 → MTL TX，反向 MTL RX → MPP 解码 → DRM 显示，强调低时延回环验证。
- `rk3588_playout_rx`：MP4 文件分发与多路接收、播放缓冲、AV 同步、DRM 多平面合成、ALSA 播放，强调文件源与多窗口输出。
- `dvs_media`：由 file_playout RX 侧演进的节点媒体引擎，受 IPC 控制，当前 output 角色可用，input 角色仅占位。

问题：完成测试后，是否应该把 `media`、`ingest`、`playout` 合并为一个应用？

这里的"合并"至少有三种含义，结论不同：

1. **源码目录合并**：把三个 app 的源文件混到一个目录。
2. **可执行文件合并**：编译成一个二进制，通过模式/角色切换执行不同链路。
3. **能力合并到产品**：让 `dvs_media` 具备 input 与 output 两种角色，同时保留 ingest/playout 作为验证与 donor。

本分析的结论是：只接受第三种；前两种不建议。

---

## 2. 三者职责并不相同

| 应用 | 主要职责 | 数据面方向 | 依赖倾向 | 生产定位 |
|---|---|---|---|---|
| `rk3588_live_tx` | 实时 HDMI 采集与低时延传输 | capture → encode → TX；RX 仅回环验证 | V4L2、RGA、MPP encode、MTL TX | 输入盒 donor 与低时延基座验证 |
| `rk3588_playout_rx` | MP4 文件分发与多路接收播放 | file demux → TX；RX → decode → compose → ALSA | FFmpeg、MPP decode、DRM、ALSA、MTL RX | 输出盒 donor 与文件源验证 |
| `dvs_media` | 受控节点媒体引擎 | output RX 数据面已具备；input TX donor 待接入 | 复用 file_playout RX + live_ingest TX donor | 生产节点媒体引擎 |

三者不是同一职责的重复实现，而是不同链路的参考与验证应用。把它们合并成一个目录或一个二进制，会把"实时采集"、"文件分发"、"受控节点"三类不同运行形态混在一起，增加模式分支与故障定位成本。

---

## 3. 为什么不建议合并为一个可执行文件

### 3.1 生产节点角色互斥

生产节点分为输入盒与输出盒，角色互斥。一个节点不会同时采集编码和解码显示。把所有链路编译进同一个二进制，并通过运行时模式切换，会扩大部署包体积、依赖集合和启动路径，却没有生产收益。

### 3.2 依赖差异

- `live_ingest` 强依赖 V4L2 采集与 RGA/MPP 编码。
- `file_playout` RX 强依赖 MPP 解码、DRM、ALSA；TX 依赖 FFmpeg。
- `dvs_media` output 角色继承 file_playout RX；input 角色只应接入 live_ingest 的 capture/encode/TX donor。

合并为一个二进制会迫使所有部署节点携带全部依赖，违背按角色拆分部署包的建议。

### 3.3 测试归因

`ingest` 与 `playout` 是端到端验证入口。若合并，链路失败时难以判断是采集、编码、MTL、解码、DRM 还是 ALSA 出现问题。保留独立 app 可以把验证范围收敛到单条链路。

### 3.4 与架构规则一致

项目架构规则强调按职责拆分，而不是按"更好/更简单/更干净"合并。三者职责不同，合并会破坏边界，而不是简化系统。

### 3.5 `both` 模式已满足开发回环

两个 app 已经提供 `both`/回环模式用于开发与联调。生产不需要 `both`，开发也不需要把所有链路塞进产品二进制。

---

## 4. 推荐方案：能力收敛，应用保持分离

### 4.1 应用层

保持三个独立应用：

- `rk3588_live_tx`：低时延实时采集与回环验证， donor chain 来源。
- `rk3588_playout_rx`：文件源分发与多窗口输出验证，RX donor 来源。
- `dvs_media`：生产节点媒体引擎，按角色接入 donor 链路。

### 4.2 共享层

把共同数据面模块收敛到共享库，而不是合并 app：

- `rk3588_common`：无锁环、slab、定时器、SPS/VUI、SEI、布局、PTP 时间、DRM 模式。
- 后续 C/D 档把 `drm_mode`、`display_layout`、`ptp_time`、`e2e_stamp` 等稳定模块从 file_playout `shared/` 迁入 common。
- `drm_renderer` / `mpp_decoder` 以 file_playout 接口为基准统一，end2end 做适配层。

目标是消除三处实现漂移，而不是消除三个 app。

### 4.3 生产集成

`dvs_media` 通过 in-tree 编译复用 donor 源码，不复制：

- output 角色：复用 file_playout RX 数据面。
- input 角色：通过 `dvs_tx_set_backend()` 接入 live_ingest 的 capture/encode/TX donor。

角色互斥与 teardown 必须真实实现；单一 `mtl_handle` 同时承载 RX/TX port。

### 4.4 部署层

按角色拆分部署包：

- 输出盒：`dvs_agent` + `dvs_media`（RX-only）。
- 输入盒：`dvs_agent` + TX-only 媒体引擎，从 live_ingest donor 或 file_playout TX 演进。
- 开发/回环镜像可包含 `both` 模式与测试 app。

---

## 5. 什么情况下可以精简或移除 donor app

只有在以下条件全部满足时，才考虑把 donor app 降级为测试工具或移除：

1. `dvs_media` 的 input 与 output 角色均已真实跑通，包括采集、编码、MTL TX、MTL RX、解码、合成、音频。
2. donor 链路所需模块已稳定收敛到共享库或 dvs_media 的 backend 模块。
3. 对应的端到端验证已迁移到 dvs_media 的测试矩阵。
4. 移除不会丢失唯一可独立运行的链路验证入口。

在这些条件满足前，保留 donor app 作为独立验证与回归入口，避免产品化过程中失去可定位问题的最小链路。

---

## 6. 对现有计划的影响

- `docs/plan/DVS_MEDIA_INTEGRATE_LIVE_INGEST.md` 的方向正确：它不是合并 app，而是把 live_ingest 的 donor chain 接入 `dvs_media` 的 backend 钩子。
- `docs/plan/rk3588/RK3588_APP_ROLE_PACKAGING_ANALYSIS.md` 的结论仍然有效：生产按角色拆分包，`both` 仅用于开发回环。
- `docs/analysis/DVS_MEDIA_RK3588_INTEGRATION.md` 的 in-tree 复用策略应继续执行，不改为复制源码或合并目录。

---

## 7. 决策建议

建议明确采用以下策略：

1. **不合并应用目录，不合并可执行文件。**
2. **合并能力到 `dvs_media`**，通过角色与 backend 钩子支持 input/output。
3. **收敛共享代码到 `rk3588_common` 与稳定 donor 模块**，消除实现漂移。
4. **保留 ingest/playout 作为 donor、参考与端到端验证应用**，直到 dvs_media 完成等价验证。
5. **生产部署按角色拆分包**，不交付 `both` 模式。

一句话总结：**合并的是能力与共享模块，不是应用边界。**
