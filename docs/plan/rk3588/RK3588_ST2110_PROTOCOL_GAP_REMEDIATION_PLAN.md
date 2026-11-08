# RK3588 ST 2110 协议缺口补齐设计方案

依据 `docs/analysis/RK3588_ST2110_PROTOCOL_GAPS.md` 与
`docs/analysis/RK3588_ST2110_USAGE.md` 的结论制定。核心判断：**绝大多数缺口是 app 未打开
MTL 已有的开关，而非库缺失**。本方案按性价比分四个阶段，只展开 P0（立即做）与 P1（按需）的
设计；P2/P3 仅给出启动条件与过渡策略。

---

## 0. 范围与不做的事

| 协议 | 决策 | 理由 |
|---|---|---|
| ST 2110-21 pacing profile | **P0，补** | 广播互通刚需，app 层零库改动 |
| VRX 余量校验 | **P0，补** | pacing 显式化后必须可验证 |
| ST 2022-7 冗余（ST22） | **P2，暂缓** | MTL 无 ST22 冗余 API，库层工作量大 |
| NMOS IS-04/05 | **P3，暂缓** | 新子系统（mDNS+REST），自有编排已够用 |
| ST 2110-40 辅助数据 | 不做 | 场景用不到，时码已由 SEI/ST30 覆盖 |
| ST 2110-20 未压缩 | 不做 | 1/2.5 GbE 带宽放不下，ST22 是正确选型 |
| ST 2059/PTP | 无需补 | external/builtin/monotonic 三模式已贯通 |

---

## P0-1 显式 ST 2110-21 pacing profile

### 目标

把 TX pacing 从"隐性默认 AUTO→TSC"变为"显式声明、可配置、可校验"，使对接专业 ST 2110
接收机/交换机时 Narrow/Medium 合规可预期。

### 设计

**1. 配置项**

三个承载 MTL 的 app 统一新增一个配置字段（JSON 配置文件中与 `ptp_mode` 同级）：

```json
{ "st21_pacing": "tsc_narrow" }   // 取值: auto | tsc | tsc_narrow | rl
```

- `app/rk3588_playout_rx`（`file_playout_config.c`）：新增
  `file_playout_pacing_from_str()`，解析到 `enum file_playout_pacing`，默认 `tsc_narrow`。
- `app/rk3588_live_tx`（`live_tx_app.c`）：命令行参数 `--pacing <way>`，默认 `tsc_narrow`。
- `app/dvs_media`：经 IPC configure 下发的字段同步增加，复用 file_playout 的解析函数
  （in-tree 复用，勿拷贝）。

**2. 应用到 mtl_init_params**

在 `file_playout_mtl_init()` / `mtl_setup_single()` 中：

```c
mtp.pacing = pacing_cfg_to_st21_way(cfg->st21_pacing);
```

映射：`auto→ST21_TX_PACING_WAY_AUTO`，`tsc→ST21_TX_PACING_WAY_TSC`，
`tsc_narrow→ST21_TX_PACING_WAY_TSC_NARROW`。默认值取 `tsc_narrow` 的理由：
MTL 已有 Narrow VRX 计算（`st_tx_video_session.c`），RK3588 软件 pacing 下这是最接近
ST 2110-21 Narrow 线性行为的档位；AF_XDP 下无 RL root，`rl` 档仅在硬件限速 NIC 上有效，
保留用于未来 10 GbE 部署。

**3. ST22 会话强制覆盖的兼容处理**

`st_tx_video_session.c` 会在 ST22 frame-level 会话上将 RL 强制降为 TSC（每帧包数不固定）。
app 侧不做规避——在 README/配置注释中说明：ST22 + `rl` 实际运行在 TSC；`tsc_narrow` 不受
该覆盖影响，是 RK3588 的推荐档。

**4. 文档**

README 增补"pacing profile 选择"小节：Narrow（互通首选）/ Medium（宽松接收端）/
auto（调试回退）的取舍，以及与 ST 2110-21 发送端行为模型的对应关系。

### 验收标准

- 三个 app 启动日志打印最终生效的 pacing way（mtl_init 后回读）。
- JSON/命令行显式配置能覆盖默认值。
- dvs_media 经 IPC 下发 pacing 字段生效。

---

## P0-2 VRX 余量校验

### 目标

pacing 配置显式化后，用可观测指标确认接收端 VRX（virtual receiver buffer）余量达标，
防止对接第三方接收机时 VRX 溢出。

### 设计

**1. 校验脚本**（放 `app/rk3588_playout_rx/tests/`，与既有 Python log 校验脚本同目录）：

- 新增 `verify_vrx_margin.py`：解析 MTL 运行日志/统计中的 `vrx` 相关计数
  （参考 `tests/integration_tests/noctx` 的 `equal-vrx-margin` 用例思路），输出
  `min_vrx_margin`，低于阈值（默认配置，如 0）时非零退出。
- 环路验证流程：`live_ingest` TX → `file_playout` RX（或单板回环），跑 60 s，脚本判定
  VRX 余量 + 无 pool-full/ring-drops 增长。

**2. 单元测试**（x86 可跑的部分）：

- pacing 字符串→枚举解析的单测，覆盖合法值、未知值回退、缺省值。

### 验收标准

- 回环跑通后 `verify_vrx_margin.py` 在 `tsc_narrow` 下报告正余量。
- 切到 `tsc` 时脚本仍能运行（不要求余量达标，仅观测差异并记录到分析文档）。

---

## P1-1 多声道 / ST 2110-31 音频（按需）

### 触发条件

业务出现 5.1/7.1 声道或 AES3 载荷需求时启动。

### 设计要点

- `mtl_tx_audio.c` / `mtl_rx_audio.c` 的 `channel`、`fmt` 从硬编码改为配置：
  `fmt` 支持 `ST30_FMT_PCM16/PCM24`，`channel` 支持 `ST30_CHANNEL_*`（2/6/8），
  `ptime` 保持 1 ms。
- `framebuff_size` 继续用 `st30_calculate_framebuff_size()` 计算，不改手算。
- AV sync 与 SPSC 环形队列结构不变；ALSA 侧按声道数重配 hw params。
- 预估工作量：1–2 天（app 层参数化 + ALSA 适配 + 回环验证）。

---

## P2 ST 2022-7 for ST22（暂缓）

### 启动条件

`dvs_media` 视频墙明确要求双网口链路级容灾。

### 过渡策略（MTL 库补齐前的应用层方案）

- TX 侧：同一码流喂两个独立 ST22 TX 会话（双端口、不同组播地址）。
- RX 侧：两个 ST22 RX 会话 + 应用层"选流合并"——按 SEI 内嵌 `frame_seq`（live_ingest
  已有）或帧序号做跨口去重，先到先用。
- 局限（写入文档）：应用层切换非无缝（毫秒级），且软件 pacing 下双路时序精度有限；仅作
  容灾过渡，不等价于标准 ST 2022-7 Class B。
- 升级路径：待 MTL 提供 `st22_redundant_*` 公共 API 后，迁移到库层冗余会话，删应用层合并。

### 库层前置工作（启动时再做）

`lib` 内新增 ST22 双端口冗余会话与跨口 seq 去重，参照 `experimental/st20_redundant_api.h`
与 RX video session 的 `ST_RX_VIDEO_REDUNDANT_SLOT_NUM` 双槽模式。

---

## P3 NMOS 适配层（暂缓）

### 启动条件

`dvs_media` 节点需要被标准广播控制器 / JT-NM 生态（SDN 控制器、IP 网关）自动发现与连接。

### 设计方向

- **不改动三个媒体 app 的 MTL 数据面**。在 `dvs_platform` 现有注册/心跳模型上做 NMOS
  适配层：每个 `dvs_media` 节点映射为一个 NMOS Node，其 TX/RX 会话映射为 IS-04 Flow/Source
  与 IS-05 Sender/Receiver。
- IS-05 connection API 的实现落到 `dvs_agent`（已有 HTTP 服务栈 + json-c），把
  `PATCH /connection/<id>` 翻译为对 `dvs_media` 的 IPC configure（复用现有
  configure/prepare/commit 屏障语义，prepare↔激活、commit↔activation）。
- mDNS/DNS-SD 依赖（avahi 或自实现）单独评估，新增运行时依赖需在 `dvs_agent` 栈决策中确认。

---

## 实施顺序与工作量

| 阶段 | 内容 | 改动面 | 预估 |
|---|---|---|---|
| P0-1 | pacing 显式配置 + 三 app 接入 | app 层 | 0.5–1 天 |
| P0-2 | VRX 校验脚本 + 解析单测 | app tests | 0.5 天 |
| P1-1 | 多声道参数化（按需） | app 层 | 1–2 天 |
| P2 | ST22 冗余（暂缓） | lib + app | 数周，按需启动 |
| P3 | NMOS 适配层（暂缓） | dvs_platform/agent | 数周，按需启动 |

## 风险

- **tsc_narrow 在 RK3588 上被调度抖动打断**：软件 pacing 依赖 lcore 独占，验证时需确认
  `MTL_FLAG_RX_SEPARATE_VIDEO_LCORE` 等标志组合下的余量表现；不达标时文档化降级到
  `tsc` 并记录实测差异。
- **dvs_media in-tree 复用**：pacing 解析函数放 file_playout，dvs_media 经相对路径引用，
  不得复制代码（遵循 AGENTS.md 约定）。
- **P2/P3 的库层依赖**：均以 MTL 库能力或互通需求明确为前置，不在本轮实施。
