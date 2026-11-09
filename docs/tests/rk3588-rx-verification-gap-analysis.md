# RK3588 接收侧（rx）测试缺口分析与实现方案

## 1 现有覆盖

- 单元测试：`test_drm_mode`、`test_display_layout`、`test_media_clk`、`test_pacing_config`
  （纯逻辑，x86 可跑）。
- 硬件冒烟：`debug/tests/drm_smoke.c`（DRM 枚举 + 单 overlay）。
- 集成检查：`check_playout_log.py`、`check_drm_log.py`、`check_pts_idx.py`。
- 性能：`tests/perf/rx_decode_perf.py`（离线 MPP 解码分位）。
- 端到端：`file_playout_rx` 运行日志中的 `RX stats` / `rx_pipe` / `E2E-C`。

## 2 主要缺口

### A. MPP 解码鲁棒性

- 缺少损坏 NAL、缺失 SPS/PPS、截断帧、分辨率切换、多 slice 的回归。
- 缺少解码器 reset/reopen 路径验证。
- 缺少长时间运行下的内存/FD 泄漏检查。

### B. 音频路径

- AAC 解码（extradata 变化、ADTS/ASC）未覆盖。
- ALSA 设备枚举、格式协商、underrun、时钟单调性未验证。
- AV 同步（audio clock vs video PTS）未量化。

### C. Playout 缓冲与 AV 同步

- 缓冲欠载/溢出策略（pb_skip、pb_overrun、pb_repeat、pb_underrun）缺少边界测试。
- 时钟漂移、PTS 乱序、重复帧的处理未覆盖。
- playout_delay 调参与欠载关系未建立曲线。

### D. 合成器与布局热更新

- 多流（PiP/视频墙）布局切换、z-order、裁剪/缩放未自动化。
- 布局更新期间的帧一致性（无撕裂）未验证。
- 多分辨率混排未覆盖。

### E. 网络损伤

- 丢包、乱序、抖动、重复包对 MTL RX 组包的影响未量化。
- 组播/单播切换、MTU 边界未覆盖。

### F. PTP 与端到端时延

- PTP 锁定、skew_reject、TAI 注入的自动化验证缺失。
- E2E-C 采集戳到呈现的全链路时延未稳定复现（当前无样本）。

### G. 长稳与恢复

- 无 soak 测试（>30min）与资源监控。
- 解码/DRM/ALSA 异常后的自动恢复未验证。

## 3 实现方案

### 3.1 单元测试（x86 可跑）

- 扩展 `tests/` 下 C unit tests：extradata 解析、NAL 类型识别、PTS unwrap、布局解析、
  playout 缓冲策略（若逻辑可抽离）。
- 使用合成输入（固定字节流）避免硬件依赖。

### 3.2 硬件探针（板端 debug/tests）

- MPP 解码探针：扩展 `mpp_dec_frame_latency` 支持 `--corrupt`、`--repeat`、
  `--max-au`、多分辨率；输出 no_output/put_fail/分位。
- ALSA 探针：新增 `debug/tests/alsa_tone.c`（枚举设备、播放正弦波、读取 clock）。
- AV 同步探针：在 `file_playout_rx` 中增加 `--dump-pts`，输出视频/音频 PTS 对齐 CSV。

### 3.3 集成测试（板端 Python）

- 扩展 `tests/perf/rx_decode_perf.py`：支持多输入、损伤注入（通过预生成损坏流）。
- 新增 `tests/perf/rx_avsync_perf.py`：解析 `rx_pipe` 与 E2E-C，计算音视频偏差。
- 新增 `tests/perf/rx_soak.py`：长时间运行并采样 CPU/内存/FD/underrun。

### 3.4 网络损伤

- 使用 `tc netem` 在回环/测试接口注入丢包/抖动/乱序。
- 提供脚本 `debug/tests/netem_inject.sh` 设置/清除规则。
- 记录 RX 丢包率、重建帧率、E2E-C 变化。

### 3.5 PTP 验证

- 使用 `ptp4l`/`phc2sys` 同步板卡时钟，运行 `file_playout_rx` 注入 TAI。
- 解析 `skew_reject` 与 E2E-C count，要求 skew_reject=0 且 count>0。

## 4 数据提供方式

### 4.1 测试码流

- 复用 `tests/perf/gen_test_stream.sh` 生成 clock/motion/noise 三种图案。
- 新增损坏变体：截断、比特翻转、缺失 SPS/PPS、分辨率切换序列。
- 多流场景：准备 2-4 路不同分辨率/帧率的 H264 与 MP4。

### 4.2 网络数据

- `tc netem` 参数记录（loss/delay/reorder）。
- 可选 pcap 抓包用于离线分析。

### 4.3 运行数据

- `rx_pipe` 单行统计（pkt/dec/render/skip/overrun/repeat/underrun）。
- `E2E-C` 行（min/avg/max/count/skew_reject）。
- MPP 解码分位 CSV（`mpp_dec_latency --csv`）。
- DRM 日志（mode/ready）。
- 系统负载采样（CPU/内存/FD）。

### 4.4 判定阈值

- 扩展 `tests/perf/common/thresholds.json` 的 `rx_decode` / `e2e` 段：
  - yield ≥ 0.99（MTL 链路）；离线解码 no_output ≤ 12、put_fail=0。
  - pb_underrun=0（playout_delay 充足时）。
  - E2E-C skew_reject=0。
  - soak 期间内存/FD 无单调增长。

## 5 优先级

1. P0：E2E-C 样本复现（SEI 戳 + 呈现路径），PTP 验证。
2. P1：音频路径与 AV 同步量化。
3. P2：网络损伤矩阵。
4. P3：多流合成与布局热更新。
5. P4：长稳与恢复。

## 6 最小落地步骤

1. 在 `file_playout_rx` 增加 `--dump-pts` 与 `--dump-e2e` 开关，输出 CSV。
2. 新增 `debug/tests/alsa_tone.c` 与 `debug/tests/netem_inject.sh`。
3. 扩展 `rx_decode_perf.py` 支持多文件与损坏流。
4. 在板端跑一轮 PTP + E2E，记录 skew_reject 与 E2E-C。
