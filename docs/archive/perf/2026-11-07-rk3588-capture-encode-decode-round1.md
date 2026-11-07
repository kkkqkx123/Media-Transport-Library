# RK3588 采集+编码 / 收流+解码性能与延迟验证 · 第一轮实测

日期：2026-11-07。板上持久目录 `~/code/mtl`（`bin/` 探针、`streams/` 预置流、`perf/` 原始结果）。
设计方案见 `docs/tests/rk3588-capture-encode-decode-perf-design.md`，本报告只归档结论与遗留问题。

## 1 环境基线（`perf/env.json`）

- SoC `rockchip,rk3588`，8 核，8GB；内核 5.10.226；CPU governor ondemand（最大 1.8GHz）。
- MPP 为 2026-02-04 SDK 版本；rga_api 1.10.1；`mpi_enc_test` / `mpi_dec_test` 可用。
- HDMI 输入锁定 1080p60（2200x1125，60.01fps，polarities 正常，input status ok）。
- V4L2 当前格式 BGR3 1920x1080，驱动枚举支持 BGR3/NV24/NV16/NV12。

## 2 本轮结论（单次测量，待三轮收敛）

| 环节 | 输入 | 均值 | p50/p90/p99 | 状态 |
|---|---|---|---|---|
| MPP 编码 1080p CBR8M | 合成时钟流 120 帧 | 3.97ms | 4.00/4.08/4.25ms | 通过，远低于预算行 10~30ms |
| MPP 解码 1080p H264 | 同上转码 123AU，119 帧产出，0 反压 | 3.35ms | 3.55/3.57/5.31ms | 通过，低于预算行 10~30ms；首尾 4AU 无输出为 priming 现象 |
| RGA 自拷贝 1080p | 200 次 | 0.92ms | 917/937/988us | 通过 |
| RGA 缩放至 720p | 200 次 | 1.64ms | 1632/1652/1705us | 通过 |
| 空闲硬件负载 | rkvenc/rkvdec 0%，rkrga 仅自带测试负载 | — | — | 基线干净 |

预算拼表（发送侧典型值，采集段待补）：采集 ? ＋ RGA 约 1ms ＋ 编码约 4ms。
对照既有 70~170ms 端到端包络，编码与解码两段合计约 7~8ms（均值），余量充足。

## 3 未完成项与问题清单

1. V4L2 采集间隔暂缺（已定位到信号源，非程序问题）。过程：首版探针 S_FMT 未填
   `num_planes`（对照实现要求置 1），NV12 被拒曾被误判为驱动行为；修正并对齐
   参考实现后复测，NV12/NV16/NV24 仍被拒绝（驱动真实行为），BGR3 协商成功、
   REQBUFS 获批 6 个、STREAMON 返回 0，但 poll 无帧。随后对照发现 `v4l2-ctl
   --stream-mmap` 同样失败（`DQBUF: Invalid argument`，dmesg 报 `timing is
   invalid`），而 dmesg 显示 `format_change` 与 `signal lock ok` 约每 1~2 秒交替
   出现：源端抖动导致驱动在 STREAMON 时刻判定失效。单次 `query-dv-timings`
   快照会返回缓存的 1080p60 值，具有误导性。待源端稳定后重采。
2. NV12/NV16/NV24 的 S_FMT 被拒绝为驱动真实行为（已排除程序因素）；BGR3 可协商，
   生产采集器应走 BGR3＋RGA 转换支路（与参考实现的格式决策一致）。
3. TX/RX 应用二进制板上缺失，管线画像与单板回环 E2E 暂不可跑，需交叉编译产物；
   解码侧已按预案用预置数据完成离线覆盖。
4. `mpi_enc_test` / `mpi_dec_test` 交叉对比输出为空，需带显式参数重跑一次互验。
5. 本轮为单次测量，阈值收敛需要两轮复测后再定。

## 4 原始结果位置

板上 `~/code/mtl/perf/`：`env.json`、`cap_interval.csv`（空，待问题 1 解决后重采）、
`enc_cost.csv`、`dec_cost.csv`、`rga_cost.csv`、`round1_*.log`。CSV 保留在板上，
不进仓库。

## 5 下一步

按序处理：等源端稳定后重采（问题 1，需先做 §6 新增的预检）→ 两轮复测收敛阈值 →
交叉编译产物到板后跑管线画像与 E2E 回环。
