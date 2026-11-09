# RK3588 接收侧（RX）验证结果 · 2026-11-09

## 1 ALSA 音频路径

- 测试程序：`debug/tests/alsa_tone.c`（板端编译 `-lasound -lm`）。
- 结果：`PASS: opened default @ 48000 Hz 2 ch S16_LE`，播放 1s 1kHz 正弦波 48000 帧成功。
- 枚举到 HDMI0/HDMI1/DP1/ES8388 等播放设备；默认 PulseAudio 可用。

## 2 MPP 解码鲁棒性

输入：`clock_1920x1080.h264`（123 AU）的损坏变体，探针 `mpp_dec_latency`。

| 变体 | au_fed | frames | no_output | put_fail | mean (us) | p99 (us) |
|---|---|---|---|---|---|---|
| 原始 | 123 | 119 | 4 | 0 | 3028.8 | 4852 |
| 截断 50% | 57 | 53 | 4 | 0 | 3083.9 | 4434 |
| 0.1% 位翻转 | 123 | 23 | 100 | 0 | 6143.7 | 20334 |
| 尾部垃圾 4KB | 123 | 119 | 4 | 0 | 3013.0 | 4827 |

结论：截断与尾部垃圾不崩溃；位翻转导致大量 no_output 但无 put_fail/crash，探针鲁棒。

## 3 网络损伤注入

- 脚本：`debug/tests/netem_inject.sh`（tc netem）。
- 板端内核未启用 netem（`RTNETLINK answers: Operation not supported`），无法在本板注入损伤；需更换内核/模块或在外部交换机注入。

## 4 端到端回环（loopback, kernel:lo）

- TX 595 帧全部编码发送；RX mtl_rx=595, drops=0, corrupt=0, incomplete=0。
- 解码：dec=594, put_fail=0；render=500, pb_skip=94, pb_overrun=0, pb_repeat=0, pb_underrun=2493。
- E2E-C：avg≈37.1ms，min≈37.1ms，max≈37.14ms，skew_reject=0，count≈499。
- 结论：链路打通，无丢包/损坏；playout_delay=80ms 下存在欠载（underrun 2493），需增大 playout_delay 或降低帧率压力；E2E-C 稳定 ~37ms。

## 5 DRM 冒烟

- `drm_smoke --list-planes-only`：8 planes，4 个 NV12 overlay，PASS。
- 显示冒烟 3s：SetCrtc/SetPlane 成功，PASS。

## 6 长稳

- 本次 loopback 运行 ~30s，无 crash；内存/FD 未专项采样（后续补充）。

## 7 遗留

- netem 不可用，损伤矩阵待外部环境。
- playout_delay 调参与欠载曲线待补。
- 多流合成与布局热更新未自动化。
