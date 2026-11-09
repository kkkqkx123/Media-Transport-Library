# RK3588 采集+编码 / 收流+解码性能与延迟验证 · 第二轮实测（采集问题复验）

日期：2026-11-09。板上持久目录 `~/code/mtl`。本轮针对 round1 问题 1（HDMI 源抖动致 STREAMON 后停流）重新验证采集，并复跑 TX/RX/RGA/MPP 单点与画像。

## 0 采集复验结论

round1 问题 1 已解决：源端稳定后 `v4l2_stream_check.sh` 一次试采即 PASS，`v4l2_cap_latency` 采 300 帧无 timeout/empty，驱动协商 BGR3 1920x1080，HDMI 锁定 1080p60。

- `cap_interval_us` mean=16664.7 p50=16664 p90=16682 p99=16690 max=16701
- `full_events=0`，`ts_clamp=0`，`ts_fallback=0`。

采集链路可采，后续 TX 画像使用真实 HDMI 源。

## 1 TX 采集+编码画像（HDMI，30s）

`tx_capture_encode_perf.py`，kernel:enP2p33s0 后端，hugepages=1024。

- samples=26，cap_fps_mean=57.9，enc_fps_mean=57.7，enc_ratio=0.962
- enc_cost_us_mean=4452，p99_max=6321us
- cap_int_us_mean=16024
- verdict: PASS

## 2 RX 解码离线画像

- 1080p H264：au_fed=123，frames=119，no_output=4，put_fail=0，mean=3020.7us，p99=4828us，verdict PASS
- 4K H264：au_fed=64，frames=59，no_output=5，put_fail=0，mean=6245.0us，p99=11558us，verdict PASS

## 3 MPP 编码单点

| 口径 | 帧数 | mean | p99 |
|---|---|---|---|
| 1080p NV12 CBR8M | 119 | 3.97ms | 4.11ms |
| 1080p BGR888 CBR8M | 119 | 4.18ms | 4.36ms |
| 4K30 NV12 CBR40M | 59 | 14.23ms | 14.40ms |
| 4K30 BGR888 CBR40M | 59 | 15.20ms | 15.39ms |
| 4K60 NV12 CBR40M | 59 | 14.10ms | 14.20ms |

## 4 RGA 单点

| 口径 | loop | mean | p99 |
|---|---|---|---|
| 1080p NV12 自拷贝 | 200 | 0.925ms | 0.975ms |
| 1080p BGR3→NV12 | 200 | 1.326ms | 1.390ms |
| 4K NV12 自拷贝 | 200 | 3.045ms | 3.100ms |
| 4K→1080p 下缩放 | 200 | 7.102ms | 7.160ms |
| 4K BGR3→NV12 | 200 | 4.691ms | 4.720ms |

## 5 端到端回环

单板回环 TX+RX（kernel 后端）运行 12s，TX 典型预算 19.24ms（采集 15.0 + 编码 4.24），最坏 20.23ms。RX 侧未解析到 E2E-C 样本（SEI 戳/呈现路径待确认）， verdict WARN。TX/RX 各自数据面正常。

## 6 结论

- 采集问题已恢复，round1 问题 1 不再阻塞。
- 全部单点/画像 verdict PASS（E2E-C 除外）。
- 预算余量充足：1080p TX 约 19ms vs 70~170ms 包络。
