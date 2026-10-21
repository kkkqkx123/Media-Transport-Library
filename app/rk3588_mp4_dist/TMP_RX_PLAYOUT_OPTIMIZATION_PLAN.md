# RK3588 MP4 分发 — RX 播放优化方案（临时存档）

> **状态**: 规划稿，待实施（**实施以 `IMPLEMENTATION_PLAN.md` 融合版为准**）  
> **日期**: 2026-06-16  
> **范围**: `app/rk3588_mp4_dist/` RX 路径（ST22 视频 + ST30 音频 + MPP 解码 + DRM 渲染）  
> **测试文件**: `InTheGrey.mp4` (1920×1080, 23.976fps, H.264 High+B, AAC 48kHz)

本文档汇总多轮排查与讨论结论，供后续实施时回顾。按优先级递进，每阶段有独立验收标准。

---

## 1. 问题背景

600s 单板测试（TX/RX 分进程）现象：

- MTL 传输健康：`0 drops/corrupt/incomplete`；TX wire ↔ RX bridge **100%** 字节匹配
- 解码率经 drain 修复后约 **97.7%**（`14057/14290`），render 与 dec 对齐
- 主观仍有 **卡顿 / judder**
- `/tmp/rx_capture.idx`：**100% `pts=0`**

已排除：传输层、H.264 profile/BSF、ST22 码流损坏、whole_au 默认化。

---

## 2. 根因归纳

| 层级 | 结论 | 证据 |
|------|------|------|
| MTL ST22 传输 | ✅ 健康 | wire=bridge 100%；MTL stats 0 异常 |
| MPP backpressure drain | ✅ 已修 | drain 帧进 `drain_pending[]`，`mpp_dec_pull()` 优先取出 |
| PTS 传递 | ❌ 断裂 | capture 100% zero；`mtl_rx_mp4.c` 仅接受 TAI |
| 渲染调度 | ❌ 未启用 | `playout_buffer` 已 init，热路径未 push/pop |
| A/V sync | ⚠️ 旁路 | `pts==0` → `av_sync_decide_for_pts` 立即 render |
| 显示刷新率 | ⚠️ 不匹配 | 23.976fps 内容 @ 1080p60 → 3:2 judder |

---

## 3. 时间戳语义（MTL vs 应用）

### 3.1 MTL ST22 RX 回调

库在帧完成时设置（`lib/src/st2110/st_rx_video_session.c`）：

```c
meta->tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
meta->timestamp = slot->tmstamp;      // 90k RTP media clock
meta->rtp_timestamp = slot->tmstamp;
```

- **收到的是 media_clock（90k）**，不是 TAI ns。
- `timestamp_first_pkt`（TAI）存在于 ST20 slot 路径，**不是** `st22_rx_frame_meta` 公开字段。

### 3.2 当前应用链路

```
mtl_rx_mp4:  pkt->tai_ns = (tfmt==TAI) ? timestamp : 0   // 非 TAI → 恒 0
mtl_rx_bridge: pkt->pts = tai_ns
rx_pipeline: rx_feed_packet(..., pkt->pts)
             rx_drain_decoder(..., rx_timestamp)        // 同一 AU 所有 MPP 输出共用
av_sync:     pts==0 → AV_SYNC_RENDER（立即渲染）
```

### 3.3 三层时间戳粒度

| 粒度 | 来源 | 当前状态 |
|------|------|----------|
| RTP 包级 | RTP header timestamp | MTL 内部组装进 slot |
| ST22 AU 级 | `meta->timestamp` | 应用层被置 0 |
| MPP 显示帧级 | 应有独立 PTS | 未实现，共用 AU pts |

---

## 4. 目标架构

```
ST22 meta (MEDIA_CLK)
    → unwrap + anchor → AU 级 TAI ns          [P1]
    → per MPP output frame PTS                [P3]
decode thread: playout_buffer_push(fd, pts)   [P2]
render thread: target_pts → pop_at() → DRM    [P2]
audio: ST30 TAI anchor + alsa elapsed         [P5]
display: 1080p24 mode                         [P4]
```

---

## 5. 分阶段实施计划

### P0 — 基线与可观测性（1 天，无功能风险）

**目标**: 固定对比基准，后续改动可量化。

**步骤**:

1. 固化 `tests/run_single_board_plan_a.sh`（120s/600s，tee 日志，grep 摘要）
2. 每次跑完采集：
   - `pkt / dec / render / drain / discard / err`
   - `/tmp/rx_capture.idx` 的 `zero_pts` 比例
   - `cat /sys/kernel/debug/dri/0/summary`（实际刷新率）
   - CPU 分布（MTL lcore vs app 线程）
3. 保留 `AU_TRACE_*` 用于回归

**验收**: 同一 MP4、同一 config，指标可重复（dec 率 ±1%）。

**依赖**: 无（drain fix、lcore pinning 已完成）。

---

### P1 — 修复 PTS 传递（最高优先级，阻塞 P2/P3）

**目标**: `codec_packet.pts` 不再恒为 0，恢复 AU 级单调时间轴。

**根因**: `rx/mtl_rx_mp4.c` 仅当 `tfmt == ST10_TIMESTAMP_FMT_TAI` 才赋值，ST22 固定为 `MEDIA_CLK`。

**改动范围（最小）**:

| 文件 | 改动 |
|------|------|
| `rx/mtl_rx_mp4.c` | 保留 `tfmt`、`raw_timestamp`、`rtp_timestamp` |
| `rx/mtl_rx_bridge.c` | 传递 media_clk 或已转换 TAI |
| `shared/pipeline_common.h` | 扩展 `codec_packet` 时间戳字段 |
| `rx/media_clk.c`（可选） | unwrap 32bit + anchor 转 TAI |

**推荐转换**:

```
首 AU:
  base_media_clk = meta->timestamp
  base_tai_ns    = mtl_ptp_read_time(st)  // 或 st10_get_tai()；PTP 未连时用 monotonic 锚定

后续 AU:
  delta_clk = unwrap(meta->timestamp) - base_media_clk
  au_tai_ns = base_tai_ns + st10_media_clk_to_ns(delta_clk, 90000)
```

**注意**: PTP 当前 `not connected` 时，可先用 **monotonic + 首帧锚定** 做相对时间轴。

**验收**:

- `zero_pts` 从 100% → 0%
- PTS 单调递增；media_clk 间隔约 `90000/23.976 ≈ 3753` ticks
- dec/render 率不下降

**风险**: 32bit wrap、首帧乱序 → 需 unwrap 状态机。

---

### P2 — 启用 playout_buffer 渲染调度（依赖 P1）

**目标**: 「来帧即渲」→「按时钟取帧」，消除 burst 渲染抖动。

**现状**:

- decode: `av_sync_decide_for_pts()` → 直接 `render_q`
- `playout_buffer_push/pop_at` 已实现（`rx/playout_buffer.c`），热路径未接线
- `av_sync_decide()`（peek PTS）已写好，render 线程未用

**目标数据流**:

| 线程 | 现在 | 目标 |
|------|------|------|
| decode | dispatch → render_q | `playout_buffer_push(fd, pts_ns)` |
| render | 阻塞 pop render_q | 周期 tick → `target_pts` → `pop_at(target_pts)` |

**target_pts**:

```
audio_master_tai = audio_first_tai + alsa_elapsed_ns
target_pts       = audio_master_tai + playout_delay_ms
```

无音频 fallback: `target_pts = base_tai + frame_index * frame_period_ns`

**配置**: `config/rx_mp4_dist.json` → `playout_delay_ms: 120`

**验收**:

- render 节奏接近 23.976fps（间隔 stddev 下降）
- 主观卡顿减轻
- playout overrun < 1%

**风险**: delay 过小 → underrun 重复帧；过大 → 延迟感明显。

---

### P3 — 解码输出帧级 PTS（依赖 P1，可与 P2 并行）

**目标**: 一个 ST22 AU 经 MPP 可能输出 0/1/N 个显示帧，每帧独立 PTS。

**现状** (`rx/rx_pipeline.c`):

```c
// 同一 AU 的所有 mpp_dec_pull 输出共用 rx_timestamp
rx_dispatch_frame(rp, ..., rx_timestamp);
```

**方案 A（推荐，改动小）**:

- AU 级 `au_tai_ns` 作为该 AU **第一幅显示帧** PTS
- 同一 AU 后续输出: `pts += frame_period_ns`（`1e9/23.976`）
- 按 MPP 输出顺序（display order）递增

**方案 B（更准，改动大）**:

- 解析 H.264 POC / slice header，按显示序号赋 PTS
- 仅当方案 A 仍有明显 A/V 漂移时再做

**验收**: 多 B 帧场景 A/V 改善；av_sync drop/repeat 非 0 且非风暴。

---

### P4 — 显示刷新率匹配（低成本，可与 P1 并行 A/B）

**目标**: 减少 23.976fps @ 60Hz 的 3:2 pulldown judder。

**步骤**:

1. `config/rx_mp4_dist.json` → `drm_mode: "1920x1080@24"`
2. 跑后确认: `/sys/kernel/debug/dri/0/summary` 为 24Hz
3. 驱动不支持精确 24 时试最接近 mode

**验收**: 平移画面 judder 减轻（与 P2 叠加更佳）。

---

### P5 — 音频时钟与 PTP 对齐（增强项）

**目标**: 稳定全局时间基准。

**步骤**:

1. ST30 RX 同样 media_clk → TAI（与视频同一 anchor）
2. 配置 PTP / MTL timesync，日志 `PTP connected`
3. anchor 从 monotonic 切到 `st10_get_tai()` / `mtl_ptp_read_time()`

**验收**: 600s A/V drift < 1 帧。

---

### P6 — MPP / 解码侧微调（证据驱动，非默认）

**原则**: P1–P4 完成后仍有 gap 才动。

| 项 | 结论 | 建议 |
|----|------|------|
| `MP4_DIST_MPP_WHOLE_AU` | 离线更差 | 保持 opt-in |
| `MP4_DIST_MPP_AGGRESSIVE_PULL` | EOS 多捞帧 | 保留 opt-in |
| legacy 8KB feed | 当前最优 | 维持默认 |
| `RENDER_Q_DEPTH` / `PLAYOUT_QUEUE_DEPTH` | P2 后按需调 | 按 overrun 调整 |

**验收**: `dec/pkt ≥ 99%` 且主观流畅。

---

### P7 — TX 侧增强（可选，非阻塞）

**目标**: TX 显式携带时间戳，简化 RX anchor。

- `tx/mtl_tx_video.c`: `get_next_frame` 设置 `meta->tfmt` / `meta->timestamp`（demux PTS）
- 需确认 MTL TX API 是否透传到 ST22 RTP header

**优先级低**: RX media_clk 已足够；不改变「TX 不转码」约束。

---

## 6. 推荐实施顺序

```
Week 1
  P0  基线固化
  P1  PTS 修复              ← 关键路径
  P4  24Hz 显示 A/B         ← 可并行

Week 2
  P2  playout_buffer 接线
  P3  帧级 PTS（方案 A）

Week 3
  P5  音频 TAI + PTP
  P6  按需 MPP 微调
  600s 长稳 + 主观评估
```

---

## 7. Go / No-Go 门槛

| 阶段 | Go 条件 | No-Go 则 |
|------|---------|----------|
| P1 | `zero_pts=0%`，PTS 单调 | 不进入 P2/P3 |
| P2 | render 间隔 stddev ↓，无 underrun 风暴 | 调 delay / queue depth |
| P3 | A/V 漂移可见改善 | 评估方案 B（POC） |
| P4 | DRM 确认 24Hz | 查驱动 mode list |
| P5 | 600s drift < 1 frame | 检查 ST30 PTS 链 |

---

## 8. 关键文件索引

| 文件 | 角色 |
|------|------|
| `rx/mtl_rx_mp4.c` | ST22 回调；**TAI-only 过滤（P1 根因）** |
| `rx/mtl_rx_bridge.c` | `pkt->pts = tai_ns` |
| `rx/rx_pipeline.c` | decode/render 线程；AU 级 pts 下发 |
| `rx/playout_buffer.c` | push/pop_at（P2 接线） |
| `rx/av_sync.c` | 音频主时钟；`pts==0` 旁路 |
| `shared/mpp_decoder.c` | drain_pending；backpressure 修复 |
| `tx/mtl_tx_video.c` | TX 未设 meta timestamp（P7） |
| `lib/src/st2110/st_rx_video_session.c` | MEDIA_CLK + slot 组装 |
| `include/st_api.h` | `st10_get_tai()`, `st10_media_clk_to_ns()` |
| `config/rx_mp4_dist.json` | drm_mode, playout_delay_ms, mtl_lcores |
| `tests/run_single_board_plan_a.sh` | A/B 测试脚本 |

---

## 9. 已完成改动（实施本方案前）

1. **MTL lcore pinning** — `mp4_dist_config.c`, `mp4_dist_app.h`；both 模式 RX/TX 共享 worker list
2. **Backpressure drain 修复** — `shared/mpp_decoder.c`：`count_output=true` + `drain_pending[]`
3. **Aggressive pull（opt-in）** — `MP4_DIST_MPP_AGGRESSIVE_PULL=1`；`rx_drain_decoder()`
4. **测试脚本** — `tests/run_single_board_plan_a.sh` 默认 120s + 摘要 grep

---

## 10. 不建议现在做的

1. **TX 转码** — 违反约束
2. **默认 `MP4_DIST_MPP_WHOLE_AU`** — 证据表明更差
3. **pts=0 时强开 av_sync drop** — 丢帧风暴
4. **大改 MTL 库** — 问题在 app 层时间戳接线

---

## 11. 参考测试命令

```bash
# RX（600s）
./build/mp4_dist rx 600 --config config/rx_mp4_dist.json --fps 23.976

# TX（另开进程）
./build/mp4_dist tx 600 --config config/tx_mp4_dist.json

# PTS 零值统计
awk '{if ($2==0) z++; n++} END{print "total="n, "zero_pts="z, "("z/n*100"%)"}' /tmp/rx_capture.idx

# 显示模式
cat /sys/kernel/debug/dri/0/summary
```

---

## 12. 一句话总结

**P1 修 PTS → P2 接 playout_buffer 控渲染节奏 → P3 帧级 PTS → P4 24Hz 显示** 为主路径；P0/P5/P6/P7 为度量与增强。每步独立验收，可逐步深入而不推翻前序工作。

---

*本文档为临时规划存档，实施完成后可归档或合并进正式 README / 设计文档。*
