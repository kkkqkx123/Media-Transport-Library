# RK3588 应用对 ST 2110 系列及配套协议（ST-21 等）的覆盖分析与补实现建议

分析对象：本仓库面向 RK3588 的三个实时媒体 app（`rk3588_live_tx`、
`rk3588_playout_rx`、`dvs_media`）及其底层 MTL 库。执行日期：2026-11-05。

## 0. 一句话结论

三个 app 当前**只用了 ST 2110-22（视频码流）+ ST 2110-30（音频 PCM）**，且**完全没有配置**
ST 2110-21 流量整形 profile、ST 2022-7 冗余、ST 2110-40 辅助数据或 NMOS 发现。好在**这些能力
绝大多数已在 MTL 库里实现**，app 缺的是"显式开关/参数"而非"从零实现"。按"值不值"排序：

- **ST 2110-21（发送侧 pacing profile，Narrow/Medium/Wide）**：广播互通刚需，当前 app 隐性
  依赖 MTL 默认（AF_XDP 下 AUTO→TSC），**应当显式 pin 成 `TSC_NARROW` 并做 VRX 合规校验**。
  工作量小（app 层改配置），价值高。
- **ST 2022-7（无缝保护切换 / 双网口冗余）**：对 `dvs_media` 视频墙可靠性有价值，但 MTL 的
  冗余主要是 **ST20（实验性 `st20_redundant_api.h`）**，ST22 冗余不成熟 → **短期不建议补，
  除非确需双链路容灾且愿投入 MTL 库开发**。
- **NMOS（IS-04/05 发现与连接管理）**：对 `dvs_platform` 多节点编排互通有价值，但属于整套新
  子系统（mDNS/DNS-SD + REST），**中低优先级，按需引入**。
- **ST 2110-40（辅助数据）/ ST 2110-20（未压缩）/ ST 2110-31（多声道）**：当前 app 场景用不到，
  **不建议补**。

---

## 1. 范围与平台约束

"ST-21 等协议"这里理解为 **ST 2110 系列**（尤其 -21 流量整形）加其配套标准
（ST 2022-7、ST 2059/PTP、NMOS/AMWA IS-04/05）。RK3588 平台约束直接决定哪些值得做：

| 约束 | 对协议选型的影响 |
|---|---|
| ARM 软件 pacing，无 NIC LaunchTime / TSN（无 ICE/E810） | pacing 只能走软件（RL/TSC），无法做 TSN 精确发送；ST 2110-21 Narrow 只能近似 |
| 板载 1/2.5 GbE（单/双口） | 未压缩 ST 2110-20（1080p60 10bit ≈ 3 Gbps）放不下 → ST 2110-22 压缩是正确选择 |
| 双网口（README 场景 B 单板回环） | 为 ST 2022-7 双链路冗余提供物理条件，但是否需要看部署可靠性要求 |
| 部署形态（PC↔RK 双机、单板回环、RK 视频墙） | 双机/墙对接专业 ST 2110 设备时互通合规重要；纯内部 MTL 对 MTL 则宽松 |

---

## 2. ST 2110-21：发送侧流量整形（Narrow / Medium / Wide）

### 2.1 现状

- MTL 已实现完整 pacing 体系：`ST21_TX_PACING_WAY_AUTO / RL / TSC / TSC_NARROW / TSN / PTP / BE`
  （`lib/src/st2110/st_fmt.c:481-483`），并有 Narrow VRX 计算（`st_tx_video_session.c:707-709`）。
- **接口级默认**由 `mtl_init_params.pacing` 设定（`lib/src/dev/mt_dev.c:2564`
  `inf->tx_pacing_way = p->pacing;`）。
- 三个 app 在 `mtl_init_params mtp` 中**都没有设置 `pacing`**（搜 `rk3588_*` 全 0 命中），
  因此走 `AUTO`：AF_XDP 不支持 RL root（`MT_RL_TYPE_TM`）时回退为 **TSC**
  （`mt_dev.c:1600-1601`）。
- 进一步，**ST22 会话会被强制从 RL 改为 TSC**（`st_tx_video_session.c:3748-3752`：
  `if (st22_frame_ops && pacing_way==RL) pacing_way=TSC;`），因为 ST22 每帧包数不固定。

结论：app 当前 TX 实际是 **TSC 软件 pacing**，能近似线性，但**未显式声明/校验 ST 2110-21 profile，
也未做 VRX 余量验证**。对接专业 ST 2110 接收机/交换机（要求 Narrow/Medium 线性）时存在被判定
为非合规、接收端 VRX 溢出风险。

### 2.2 价值与建议

- **价值：高**（广播互通刚需；单板回环/内部 MTL 对 MTL 时价值低）。
- **是否补实现：是，且只在 app 层**——在 `file_playout_mtl_init` / `mtl_setup_single` 里
  显式 `mtp.pacing = ST21_TX_PACING_WAY_TSC_NARROW`（或按部署选 Medium 等价档），并把该值
  暴露为 JSON 配置项（`ptp_mode` 同级）。同时在测试/校验脚本里用 MTL 的 VRX 统计确认
  `vrx` 余量达标（参考 `tests/integration_tests/noctx` 的 `equal-vrx-margin` 思路）。
- 无需改 MTL 库：ST 2110-21 能力齐备，仅缺 app 开关。

---

## 3. ST 2022-7：无缝保护切换（冗余双链路）

### 3.1 现状

- MTL 有冗余能力，但**主线集中在 ST20**：`experimental/st20_redundant_api.h`（标题即
  "st2110-20 combined redundant transport, experimental"）、`CHANGELOG` "st20: Change ST2022-7
  stream class"、RX video session 有 `ST_RX_VIDEO_REDUNDANT_SLOT_NUM` 双槽
  （`st_rx_video_session.h:18`）。ST30/ST40 也有冗余/seq-gap 处理。
- **ST22 冗余在 MTL 中并不成熟**（无对应 `st22_redundant_*` 公共 API）。
- 三个 app 均未启用任何冗余（搜 `rk3588_*` 全 0 命中）。

### 3.2 价值与建议

- **价值：中**（仅当 `dvs_media` 视频墙要求双网口链路级容灾时有意义；双机/回环场景价值低）。
- **是否补实现：短期不建议**。难点在 MTL 库层——ST22 冗余 API 缺失，需在 `lib` 新增
  ST22 双端口冗余会话与跨口 seq 去重，工作量与风险都大；且 RK3588 软件 pacing 下冗余合并的
  时序精度有限。若确有容灾需求，建议**先用应用层双 TX/双 RX 会话 + 上层切换**过渡，待 MTL
  提供 ST22 冗余 API 再升级。

---

## 4. ST 2110-40：辅助数据（字幕/时码/SDI anc）

### 4.1 现状

- MTL 已支持 ST40（`st2110/st_pkt.h`、`st_tx_ancillary_session.c`、RX ancillary session 含
  跨口冗余槽与 bitmap 去重，`st_header.h:1193-1220`）。
- 三个 app 均未使用（搜 `rk3588_*` 全 0 命中）。`live_ingest` 的 SEI 时间戳是**塞进 H.264
  码流内部**的，不走独立 ST40 流。

### 4.2 价值与建议

- **价值：低**（当前场景无需独立传 SDI 辅助数据；时码/打点已用 SEI/ST30 时间戳解决）。
- **是否补实现：不建议**，除非要对接要求 ST40  carriage 的 SDI-IP 网关或字幕系统。如需要，
  MTL 库已具备，app 只需新增一路 `st40_tx/rx_create` 会话，难度中等。

---

## 5. ST 2110-20：未压缩视频

### 5.1 现状

- MTL 全面支持 ST20（`st20_tx/rx_create`、pipeline `st20p_*`）。仓库内 `app/v4l2_to_ip` 即
  用 `ST20_FMT_YUV_422_8BIT` 发未压缩流，但它**不在 RK3588 三个 app 之列**。
- 三个 app 一律选 ST 2110-22 压缩（`ST22_PACK_CODESTREAM`），与平台带宽匹配。

### 5.2 价值与建议

- **价值：负/不适用**（RK3588 的 1/2.5 GbE 装不下未压缩 1080p60 10bit）。选 ST22 是正确决策。
- **是否补实现：不建议**。若未来上 10 GbE 且需零延迟未压缩，可复用 `v4l2_to_ip` 的 ST20 路径，
  但当前无必要。

---

## 6. ST 2110-31 / 多声道音频

- 当前 ST30 仅 **PCM16 / 48k / 2ch / 1ms**（`mtl_tx_audio.c:123-141`）。若需 5.1/7.1 或
  AES3(ST 2110-31) 嵌入，需扩展 `channel` 与 `fmt`（如 `ST30_FMT_PCM24`、`ST30_CHANNEL_*`）。
- **价值：低**（当前场景双声道足够）。**是否补：按需**，MTL 已支持，app 仅扩参数。

---

## 7. NMOS（IS-04 发现 / IS-05 连接管理）

### 7.1 现状

- 三个 app 与 `dvs_platform` 均无 NMOS/mDNS/DNS-SD（搜 `rk3588_*` 全 0 命中）。
- `dvs_platform` 现用**自有注册/心跳/prepare-commit 屏障**做多节点编排（非标准）。

### 7.2 价值与建议

- **价值：中**（若 `dvs_media` 节点要被标准广播控制器/ JT-NM 生态自动发现与连接，NMOS 是门票；
  纯自有平台内互通则非必需）。
- **是否补实现：低优先级、按需**。NMOS 是独立子系统（mDNS-SD 服务发现 + IS-04 REST 注册 +
  IS-05 连接 API），工作量显著且需新增服务依赖。建议：短期维持自有编排；当进入标准 ST 2110
  基础设施（SDN 控制器、IP 网关）互通时再引入，可基于现有 `dvs_platform` 注册模型做适配层。

---

## 8. ST 2059 / PTP（已具备，列作对照）

- 三个 app 已实现 PTP：`external`（linuxptp→CLOCK_TAI）、`builtin`、`monotonic` 三模式，
  `mtl_ptp_read_time` 贯通（`file_playout_config.c:804-818`、live_ingest `time_now`）。
  这是 ST 2110 系统的时钟底座，已覆盖，无需补充。

---

## 9. 现状对照表

| 协议 | MTL 库支持 | app 当前使用 | RK3588 价值 | 是否补 / 在哪补 | 优先级 |
|---|---|---|---|---|---|
| ST 2110-21 pacing profile | 是（RL/TSC/TSC_NARROW/TSN） | 隐性默认 TSC，未 pin | 高（广播互通） | **补，app 层设 `mtp.pacing`+校验** | 高 |
| ST 2022-7 冗余 | 仅 ST20 实验性 | 无 | 中（视频墙容灾） | 暂缓，需 MTL ST22 冗余 | 低 |
| ST 2110-40 辅助数据 | 是 | 无 | 低 | 不建议 | — |
| ST 2110-20 未压缩 | 是 | 无（v4l2_to_ip 除外） | 负（带宽） | 不建议 | — |
| ST 2110-31 多声道 | 是 | 仅 2ch | 低 | 按需扩参数 | — |
| NMOS IS-04/05 | 否 | 无（自有编排） | 中（标准互通） | 按需、低优先 | 低 |
| ST 2059/PTP | 是 | 已三模式 | 已具备 | 无需补 | — |

---

## 10. 建议的"补实现"清单（按性价比）

1. **【立即，app 层】显式 pacing profile**
   - `file_playout_config.c` `file_playout_mtl_init()` 与 `live_ingest_app.c`
     `mtl_setup_single()` 中设置 `mtp.pacing = ST21_TX_PACING_WAY_TSC_NARROW`（或配置项），
     使 ST 2110-21 Narrow/Medium 合规可预期。
   - 新增 JSON 字段（如 `st21_pacing`）并在 README 说明 Narrow/Medium 取舍。
   - 在验证脚本中读取 MTL VRX 统计，确认 `vrx` 余量达标。

2. **【按需，app 层】多声道/ST 2110-31**
   - 仅当业务需要，扩展 ST30 `channel`/`fmt`。

3. **【暂缓，库层 + app 层】ST 2022-7 for ST22**
   - 仅在视频墙双链路容灾需求明确时启动；先在 MTL 补齐 ST22 冗余会话 API，再于 app 启用。

4. **【暂缓，新子系统】NMOS**
   - 进入标准 ST 2110 基础设施互通时，基于 `dvs_platform` 注册模型做 NMOS 适配层。

> 关键判断：**绝大多数"缺"是 app 未开 MTL 已有开关，而非 MTL 缺失**。最高性价比动作是
> 第 1 条——把 ST 2110-21 pacing 从"隐性默认"变成"显式声明 + 校验"，几乎零库改动即可显著提升
> 对专业 ST 2110 设备的互通性。
