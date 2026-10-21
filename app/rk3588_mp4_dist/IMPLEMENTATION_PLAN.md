# RK3588 MP4 分发 — 开发实施计划（融合版）

> **状态**: 待实施  
> **日期**: 2026-06-17  
> **范围**: `app/rk3588_mp4_dist/` RX 播放优化 + 流参数兼容性  
> **前置**: drain 修复、lcore pinning 已完成  
> **存档**: 详细背景见 `TMP_RX_PLAYOUT_OPTIMIZATION_PLAN.md`

---

## 1. 产品能力边界（全阶段适用）

| 支持 | 不支持 / 需手动对齐 |
|------|-------------------|
| H.264 Annex B（Baseline/Main/High，由 SPS 决定） | HEVC / AV1 / 其他编码 |
| CFR，MTL `st_fps` 枚举表内帧率（23.976/24/25/29.97/30/50/59.94/60…） | 任意 fps、VFR |
| 可配置 width/height/fps/codestream_size | playlist 内异构参数（fps/分辨率不同） |
| AAC → ST30 PCM16 48kHz 2ch | 44.1k / 5.1 / 非 AAC 源 |
| TX/RX 参数一致（fps 必须匹配） | 自动 SDP/元数据协商 |

---

## 2. 分阶段实施总表

| 阶段 | 目标 | 开发内容 | 兼容性要求 | 主要文件 | 验收 | 依赖 |
|:----:|------|----------|------------|----------|------|:----:|
| **P0** | 基线 + 能力边界 | ① 完善 `run_single_board_plan_a.sh`（时长/tee/摘要 grep）<br>② 跑后自动采：pkt/dec/render/drain、zero_pts、DRM 刷新率、CPU<br>③ 文档化能力边界（上表）<br>④ 固化 probe 工作流：`--probe-mp4` → 生成 TX/RX 建议命令 | probe 输出 width/height/fps；config 与 probe 不一致时 WARN | `tests/run_single_board_plan_a.sh`<br>`mp4_dist_config.c`<br>`README` / 本文档 | 同 config 指标可重复（dec ±1%）；probe 命令可用 | 无 |
| **P1** | AU 级 PTS 恢复 | ① `mtl_rx_mp4.c`：接受 `MEDIA_CLK`，不再非 TAI 置 0<br>② 扩展 `codec_packet`：`tfmt` / `media_clk` / `pts_ns`<br>③ 实现 media_clk unwrap + anchor → TAI（PTP 未连时用 monotonic 锚定）<br>④ bridge/pipeline 传递 pts<br>⑤ RX 启动校验：fps/width/height 与 config 一致则 WARN | **禁止硬编码 23.976**；`st10_media_clk_to_ns(..., 90000)` 与 fps 无关<br>TX/RX fps 必须 pre-config 一致 | `rx/mtl_rx_mp4.c`<br>`rx/mtl_rx_bridge.c`<br>`shared/pipeline_common.h`<br>`rx/media_clk.c`（新）<br>`rx/rx_pipeline.c` | `zero_pts` 0%；PTS 单调；dec 率不降 | P0 |
| **P2** | 按时钟渲染 | ① decode：`playout_buffer_push(fd, pts)`<br>② render：`target_pts` → `pop_at()` → DRM<br>③ 音频主时钟：`audio_first_tai + alsa_elapsed + playout_delay`<br>④ 无音频 fallback：`base_tai + frame_idx × frame_period_ns` | `frame_period_ns = 1e9 / cfg.fps`（运行时，非写死）<br>`playout_delay_ms` 可配 | `rx/rx_pipeline.c`<br>`rx/playout_buffer.c`<br>`rx/av_sync.c`<br>`config/rx_mp4_dist.json` | render 节奏稳定；overrun <1%；卡顿减轻 | **P1 必过** |
| **P3** | 显示帧级 PTS | ① 同一 AU 多帧输出：`pts += frame_period_ns` 递增<br>② 首帧用 AU 级 `au_tai_ns`；按 MPP 输出顺序分配<br>③ 统计 per-AU 多帧输出比例 | `frame_period_ns` 来自 `cfg.fps`<br>B 帧/GOP 长：方案 A 够用则不做 POC 解析 | `rx/rx_pipeline.c` | 多 B 帧场景 A/V 改善；drop/repeat 合理 | P1 |
| **P4** | 显示与内容匹配 | ① `drm_mode` 由 width/height/fps 生成或校验<br>② 逻辑：`"{W}x{H}@{round(fps)}"`，驱动无精确 mode 时选最接近<br>③ 启动 log + 跑后核对 `/sys/.../dri/0/summary` | 23.976→24、30→30、60→60（**非一律 24Hz**）<br>分辨率与 `cfg.width/height` 一致 | `mp4_dist_config.c`<br>`shared/drm_renderer.c`<br>`config/rx_mp4_dist.json` | DRM 实际刷新率与内容 fps 匹配；judder 减轻 | P0；可与 P1 并行 A/B |
| **P5** | 全局时钟对齐 | ① ST30 RX：media_clk → TAI（与视频同 anchor）<br>② 配置 PTP / MTL timesync<br>③ anchor 从 monotonic 切 `st10_get_tai()` / `mtl_ptp_read_time()` | 音频 TAI 非 0；与视频同一时钟域 | `rx/mtl_rx_audio.c`<br>`rx/av_sync.c`<br>MTL/系统 PTP 配置 | 600s A/V drift < 1 帧 | P1/P2 |
| **P6** | 缓冲与解码微调 | ① probe 后可选估 `codestream_size`（如 `max(w×h×2, 1MB)`）<br>② 按 overrun 调 `PLAYOUT_QUEUE_DEPTH` / `RENDER_Q_DEPTH`<br>③ **维持** legacy 8KB feed 默认；whole_au / aggressive_pull 仅 opt-in | 高码率/4K 需增大 `codestream_size`；config 可覆盖 probe 估值 | `mp4_dist_config.c`<br>`shared/mpp_decoder.c`<br>`rx/rx_pipeline.c` | dec/pkt ≥99%；高码率无 AU 截断 | P1–P4 后仍有 gap |
| **P7** | TX 时间戳增强 | **已完成（基础）**：`shared/tx_tai_ts.c` + ST22/ST30 `USER_TIMESTAMP`/`USER_PACING`，`stream_start_tai + epoch × period`<br>**待办 P7b**：见 §9 Feature backlog | 需 PTP（`ptp_mode≠monotonic`）；RTP 仍为 90k media_clk | `tx/mtl_tx_video.c`<br>`tx/mtl_tx_audio.c`<br>`shared/tx_tai_ts.c`<br>`tx/tx_scheduler.c` | 日志 `tai-ts` + `tx_tai: stream anchor`；多 RX RTP 轴一致 | P5 |
| **P8** | RX 画中画视口（Phase A） | **已完成**：JSON `display_viewport` + `display_layout` + `drm_renderer` overlay plane（Esmart NV12 硬件缩放）<br>默认全屏；`enabled=true` 时黑底 + PiP 矩形<br>**Phase B 预备**：`display_layer[]` / `DISPLAY_LAYOUT_MAX_LAYERS`<br>验证：`test_drm_plane` + `test_display_layout` | 缺省或 `enabled:false` = 原 SetCrtc 全屏<br>需 `DRM_CLIENT_CAP_UNIVERSAL_PLANES` | `shared/display_layout.c`<br>`shared/drm_renderer.c`<br>`mp4_dist_config.c`<br>`config/rx_pip_viewport.json` | `./build/test_drm_plane` PASS；RX viewport 目视 PiP | P4 |

---

## 3. 推荐实施顺序

```
P0（基线+边界）
  ├─ P1（PTS）─────────────┐
  ├─ P4（DRM，可并行 A/B）  │
  └─ P6-C1（probe 估 buffer，可选）
P2（渲染调度）← 依赖 P1
P3（帧级 PTS）← 依赖 P1，可与 P2 重叠
P5（PTP/音频 TAI）
P6（按需微调）
P7（TX TAI 基础）→ P7b（demux PTS，见 §9）
```

| 周期 | 阶段 | 说明 |
|:----:|------|------|
| Week 1 | P0 → P1 → P4 | 基线 + PTS 根因 + 显示 A/B |
| Week 2 | P2 → P3 | 渲染调度 + 帧级 PTS |
| Week 3 | P5 → P6 → P7b? | 时钟对齐 + 微调 + demux PTS 片头对齐 |

---

## 4. Go / No-Go 门槛

| 阶段 | Go | No-Go 动作 |
|:----:|-----|-----------|
| P1 | zero_pts=0%，PTS 单调 | 不进入 P2/P3 |
| P2 | render 间隔 stddev↓，无 underrun 风暴 | 调 delay/queue depth |
| P3 | A/V 可见改善 | 评估 H.264 POC 方案 B |
| P4 | DRM 刷新率≈内容 fps | 查 mode list，选手动 mode |
| P5 | 600s drift < 1 帧 | 查 ST30 PTS 链 |
| P6 | dec/pkt ≥99% | 增大 codestream_size |

---

## 5. 暂不处理项及影响

| 暂不处理 | 原因 | 影响 | 规避 / 使用约束 |
|----------|------|------|----------------|
| **HEVC / AV1 等非 H.264** | 链路写死 `MPP_VIDEO_CodingAVC` + ST22 codestream | 非 H.264 MP4 无法解码播放 | 仅使用 H.264 源；扩格式需独立项目 |
| **VFR / 非 MTL 枚举 fps** | ST22 session 假设 CFR；`st_frame_rate_to_st_fps` 离散表 | 22fps 等非常规帧率 session 创建失败或映射错误 | 使用 MTL 支持的标准 fps；probe 校验 |
| **TX/RX 自动参数协商** | 无 SDP/内嵌元数据通道 | fps/分辨率配错 → 丢包/花屏/组装失败 | `--probe-mp4` + 两边 config 一致；P0 WARN |
| **playlist 异构参数** | MTL session 启动时固定，切文件不重建 | playlist 内不同 fps/分辨率 → 第二文件起异常 | 短期：playlist 内文件同质；长期：切文件重建 session |
| **非 48kHz / 非 stereo 音频** | TX ST30 写死 48k PCM16 2ch | 44.1k/5.1 AAC 音画不同步或无声 | 仅 48k stereo AAC 源；或 TX 侧重采样（未规划） |
| **H.264 POC 级 PTS（P3 方案 B）** | 复杂度高；方案 A 先验证 | 长 GOP + 大量 B 帧时 A/V 可能仍有 ≤1 帧级偏差 | 先用方案 A；偏差可测再开 B |
| **PTP 硬依赖** | 当前环境 PTP 未连 | 无 PTP 时用 monotonic 锚定，无绝对墙钟 | P1 支持 monotonic fallback；P5 再上 PTP |
| **TX 转码** | 用户约束 | — | 禁止 |
| **默认 whole_au MPP feed** | 实测更差 | 解码率下降 | 保持 opt-in |
| **大改 MTL 库** | 根因在 app 层接线 | — | 不改 lib |
| **pts=0 时强开 av_sync drop** | 丢帧风暴 | 画面更卡 | P1 修 PTS 后再启用 sync 逻辑 |

---

## 6. 每阶段交付物速查

| 阶段 | 交付物 |
|:----:|--------|
| P0 | 可重复测试脚本 + 基线报告模板 + 能力边界文档 |
| P1 | AU 级非零 PTS + media_clk 转换模块 + config 一致性 WARN |
| P2 | playout_buffer 接线 + 音频/无音频双模式 render 调度 |
| P3 | 每 MPP 输出帧独立 PTS |
| P4 | DRM mode 与内容 fps/分辨率联动 |
| P5 | ST30 TAI + PTP 可选全局时钟 |
| P6 | codestream_size 估算 + queue 调优指南 |
| P7 | TX `USER_TIMESTAMP` + PTP 帧轴 TAI（已实现） |
| P7b | MP4 demux PTS → TAI 片头对齐（待开发，§9） |
| P8 | RX display_viewport + DRM overlay plane PiP（Phase A，已实现） |
| P8b | 多流 PiP（Phase B，§9 backlog） |

---

## 7. 关键测试命令

```bash
# Probe → 生成建议 config
./build/mp4_dist --probe-mp4 /path/to/file.mp4

# 基线跑（P0 后）
./tests/run_single_board_plan_a.sh

# PTS 零值统计（P1 验收）
awk '{if ($2==0) z++; n++} END{print "total="n, "zero="z}' /tmp/rx_capture.idx

# 显示模式（P4 验收）
cat /sys/kernel/debug/dri/0/summary

# 阶段自动检验（P0/P1 已落地）
./tests/verify_phase.sh P0 [mp4_path]
./tests/verify_phase.sh P1 [seconds] [mp4_path]
MP4_DIST_SKIP_L2=1 ./tests/verify_phase.sh P1   # 无板子时仅 L1
meson test -C build media_clk
```

---

## 8. 阶段自动检验

| 阶段 | 命令 | Pass 标准 |
|:----:|------|-----------|
| P0 | `verify_phase.sh P0` | 工具链 + probe 输出 |
| P1 | `verify_phase.sh P1` | `test_media_clk` + idx zero_pts=0% + dec≥90% |
| P2+ | 扩展 `verify_phase.sh` | 实施时追加 |

---

## 9. Feature backlog（后续开发）

| ID | 功能 | 说明 | 主要改动 | 依赖 | 优先级 |
|:--:|------|------|----------|------|:------:|
| **P7b** | **MP4 demux PTS 嵌入 TAI（片头对齐）** | 当前 P7 用 `stream_start_tai = mtl_ptp_read_time()` 作流起点；增强为以 **FFmpeg demux PTS** 为内容时间轴：首帧 `stream_start_tai = ptp_now - first_pts_ns`（或 `ptp_now` 对齐片头 PTS=0），每帧 `timestamp = stream_start_tai + demux_pts_ns`。视频在 `tx_get_next_frame` / `tx_tai_ts_fill` 传入 packet PTS；音频同理对齐 AAC packet 时间。RX 侧 RTP media_clk 与 MP4 时间轴一致，多终端/seek/playlist 切换更可控。 | `shared/tx_tai_ts.c`（扩展 API，区分 wall vs content anchor）<br>`tx/tx_scheduler.c`（demux → publish 传递 PTS）<br>`tx/mtl_tx_video.c` / `tx/mtl_tx_audio.c`（按帧 PTS 填 meta）<br>可选：`mp4_playlist.c` seek/切文件重置 anchor | P7 基础 + P5 PTP | 中 |
| **P8b** | **多流真·PiP（Phase B）** | 主画面 + 1–N 子画面，每路独立 ST22 session / decoder / DRM plane；扩展 `display_layout.layers[]` 与 `drm_renderer` 多 plane 管理 | `rx/rx_pipeline.c` 多实例<br>`config` 中 `pip_streams[]`<br>参考 `doc/rk3588_pip_analysis.md` | P8 Phase A | 低 |
| — | playlist 切文件重建 TAI anchor | 多文件 loop 时每段 MP4 独立 PTS 基准，避免第二段 A/V 跳变 | `tx_scheduler.c` + session 重建或 anchor 重置 | P7b | 低 |
| — | RX 弱锚定 / 免首帧 anchor | ST22 RX 仍只有 MEDIA_CLK；若 TX RTP 轴已绝对对齐，可评估缩短或共享 video/audio anchor | `rx/media_clk.c` | P7b + 双机实测 | 低 |

**P7b 验收建议**：同一 MP4 在 TX/RX 日志中 demux PTS 与 `meta.timestamp` 差值稳定；seek_ms 后首帧 TAI 与内容 PTS 一致；600s 无额外 A/V 漂移。

---

*实施完成后可将本文档合并进正式 README，并归档 `TMP_RX_PLAYOUT_OPTIMIZATION_PLAN.md`。*
