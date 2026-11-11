# 编解码延时真相源（RK3588 MPP）

> 日期：2026-11-11 晚。**本文件是编解码延时数字的唯一口径依据。**
> 替代并删除 `codec-latency-semantics.md`（11-09，其解码慢数已被同条件复测推翻）
> 与 `decode-latency-gaps-and-shallow-async.md`（11-11，小缓冲门控结论并入本文 §5）。
> 凡与本文数字矛盾的旧文档、旧归档引用，一律以本文为准，不做兼容解释。
> 探针源码：`debug/mpp_enc_frame_latency.c` / `debug/mpp_dec_frame_latency.c`，
> 板端二进制 `~/code/mtl/bin/`。本轮原始数据：板 `~/code/mtl/perf/dec_gt_enc_20261111_085704/`。

## 1 一句话结论

1080p 同条件下**解码短于编码**：repo clock 流 `dec mean 3.0ms vs enc mean 4.0ms`
（三轮复现）；内容配对（MPP 自产 H264 回喂）`dec mean 1.9ms vs enc 4.0ms`。
"解码慢于编码"是旧数加错比造成的假象，理论（编码做运动估计 + 率失真 +
熵编码，解码只做熵解 + 运动补偿 + 环路滤波）成立。4K 下编码约 14.9ms、
解码约 6.4~6.7ms，从未反转。

## 2 口径定义（旧文档中仍然有效的部分）

| 指标 | 采样点 | 精确语义 |
|---|---|---|
| 探针 enc sync mean/p50/p99 | `t0`=put 前；`t1`=取到包后 | 单帧送入空闲编码器 → 码流包取出，串行，无流水。首帧计入 priming 不进样本，1ms 轮询量化 |
| 探针 dec sync mean/p50/p99 | `t0`=put 前；`t1`=轮询取出可显示帧后 | 单 AU 送入空闲解码器 → 首个可显示帧取出，串行。**全收录（含启动帧）**，`--wait-ms` 截止期默认 50ms，100us 轮询步进 |
| 探针 async（enc/dec） | feeder/collector 双线程 | 提交到输出的 sojourn（含排队）+ wall 吞吐 fps。**不可与 sync service 对比，不可套帧间隔阈值** |
| TX `enc_cost` / `enc_p50/p90/p99` | put 前 push 戳 → 收集线程收齐 EOI 入环后记耗时 | "提交 → 码流可发送"**预算口径**，含收集等待搬运与 MPP 队列排队，恒大于探针 service。丢帧配对错位已修（丢帧路径同步消费 submit 戳） |
| RX `dec_p50/dec_p99` | feed 时刻 → 首帧 pull 时刻，300 帧滑动窗口 | 生产解码预算口径。代码已加（`shared/mpp_decoder.c`），**零实测**，见预算 T1 |
| `E2E-C` | TX V4L2 回调戳（SEI 内嵌）→ RX flip 提交返回后 | 采集 → 显示提交，不等 vblank。`skew_reject>0` 则本轮作废 |
| `rx_decode_perf.py --mode mtl` | 产出比 + 欠载 | 只有 `dec/pkt ≥ 0.99` 与 `pb_underrun` 判定，**不含时长** |

Python 编排层零扰动已验证（裸跑 vs 编排负载差异 <0.5%）：时间戳全在 C 进程内打点，
编排只做拉起与管道读取。`mpi_dec_test` 在本板零输出，offline 兜底不可用，
必须用 `--dec-binary` 指探针。

## 3 当前有效数字（11-11 晚，governor ondemand，clock 流）

编码 sync（CBR8M，level 42）：round2 `3.97/4.11ms`；11-11 持续 2000 帧
`mean 3969 p50 3963 p99 4117us` 零漂移；本轮 `mean 3999 p50 4010 p99 4180us`。
三轮一致，**有效值：mean 约 4.0ms，p99 约 4.1~4.2ms**。

解码 sync（本轮同条件，三轮复现，`au_fed=123 framed=119 no_output=4 put_fail=0`）：

| 输入 | mean | p50 | p99 | skip10 后 p99 |
|---|---|---|---|---|
| repo `clock_1920x1080.h264`（外部合成流） | ~3.0ms | ~3.2ms | ~4.6ms | ~3.4ms |
| fresh（本轮 enc 现产 H264 回喂，内容配对） | 1.99ms | 1.92ms | 3.34ms | ~2.1ms |
| bars 静态流 | ~3.1ms | ~3.3ms | ~5.5ms | ~3.4ms |

**有效值（预算代理）：mean/p50 约 3.0ms，p99 约 4.6ms；配对下限 1.9/2.1ms
仅用于机理论证，不进预算。** 首帧 5.3~5.5ms、次帧 ~3.3ms 均为解码器初始化，
skip10 后 p99 跌 ~1.2ms；enc skip10 后 p99 几乎不动（4180→4131us）。
4K：enc CBR40M `mean ~14.9ms`；dec `mean ~6.4~6.7ms / p99 ~13.9ms`。

## 4 三条对比戒律（违反任何一条，对比结论作废）

- **R1 同内容。** 解码成本由输入流特性决定，不由码率决定：fresh 流 6 倍于
  repo 流的码流反而快 1.1ms。repo 流是外部 High profile / CABAC 合成，
  MPP 自产是 Baseline（`h264:profile 66`）/ CAVLC——熵编码制式差约 1ms。
  跨"编码器输入（NV12）"和"解码器输入（H264）"直接比数字，量的不是同一个东西。
- **R2 同 warmup。** enc 探针剔首帧，dec sync 全收录。比 p99 必须 skip≥10
  或明确注明含启动，否则系统性冤枉解码约 1ms。
- **R3 同版本。** 以下旧数作废，禁止引用：dec `mean 4968 / p50 5409 / p99 7311`
  与 `p50 4952 / p99 7034`（11-09，同一文件后三轮复测均为 3.0ms，系旧二进制 /
  异常板状态）；enc `mean 4555 / p50 4561`（同批旧数）。旧 500us 步进 / 25ms
  上限版探针的任何数字不再有效。

## 5 async 与小缓冲门控现状（旧 gaps 文档结论继承，数字见 §3 修正）

- 单会话 dec async 吞吐比 sync 高约三成（1080p +27%，4K +31~32%，跨启动复现）。
  这是流水重叠，不是 service 变快；**预算用 sync，容量用 async**，不可互换。
- 多会话并发下 sync 全面反超 async（1080p x4 aggregate 高约两成，
  stretch 0.58 对 0.40；rkvdec x2 即 99%+ 见顶）。硬件真上限是 aggregate
  口径（1080p ~715fps，4K ~341fps，约为单会话 1.6~1.7×），准入按
  aggregate × 0.7（1080p60 与 4K30 均约 8 路）。
- 拐点 `depth=4`（2→4 吞吐翻倍，4→16 持平 ±3%；depth=4 的 sojourn 分位反而最优；
  depth=1 不可用）。排队上界 `4 × service`。
- sync service 按 §3 已修正为 1080p ~3ms（旧文"约 5ms"作废），小缓冲排队上界
  相应为 1080p ~12ms / 4K ~20ms。
- 门控（三者任一）：多流落地需求 / 生产 `put_fail` 反压堆积 / HW 饿死间隙。
  文件输入下 `put_fail` 全零，门控未满足 → **不动生产解码代码**。
  旧文引用的 `RK3588_DECODE_ASYNC_PLAN.md` 不存在，门控状态以本节为准。

## 6 已修复问题（11-09 五项，继承）

解码探针 25ms 上限 → `--wait-ms`（默认 50ms）+ 100us 步进；enc 探针首帧
priming 不进样本；`enc_cost` 丢帧路径 submit 错位 + slab 泄漏；RX 生产
`dec_p50/dec_p99` 打点；`rx_decode_perf.py` 自动探测探针路径。
p99 尾无 GOP 周期性（top 样本相位随机，带宽 ~55~70us），无需 IDR-aware 预算。

## 7 预算引用指南

预算表编码段用生产口径 `enc_cost 4.5 / p99 6.3ms`，解码段用探针代理
`p50 3.0 / p99 4.6ms`；配对下限（1.9/2.1ms）只证明机理，不进预算。
生产 RX `dec_p50/p99` 回填（预算 T1）后若显著大于探针，解码段改用生产值。
