# 解码盒验证：浅缓冲拐点 + 多会话 stretch

日期：2026-11-11。对应设计 `docs/tests/decode-latency-gaps-and-shallow-async.md`
§3.3 第一步（探针 only）。只改探针与脚本，生产代码未动。
板端结果：`~/code/mtl/perf/dec_shallow_verify/`、`~/code/mtl/perf/dec_x8/`，
overall 均为 PASS（sweep/concurrent 只记录 INFO，不翻转结论）。

## 1 方法

- 探针 `mpp_dec_frame_latency.c` 加 `--depth N`（async 专用，默认 16=
  现行值；sync 口径逐字节不变）：门控由"fed−framed"改为"帧 AU−framed"
  （`au_is_header` 按 NAL type 剔除 SPS/PPS/SEI/AUD，头 AU 永不产帧，
  旧门控在 depth ≤ 头数时必死）。输出行尾加 `depth=N`（下游前缀正则安全）。
- driver 加 `--depths`（单会话扫点）、`--concurrent`（同起 N 会话批跑，
  共用负载采样）、`--cdepth`（批跑深度，默认 4）。批跑每个会话独立进程/
  独立 MPP context（即生产多流形态），`stretch = min会话fps / 同depth单会话fps`。
- 矩阵：1080p/4K clock 流，`--depths 1,2,4,8,16 --concurrent 2,4,8 --cdepth 4`。

## 2 拐点：depth=4 到顶，且分位最优（单会话）

| depth | 1080p fps | 1080p p50/p99 | 4K fps | 4K p50/p99 |
|---|---|---|---|---|
| 1 | 不可用（rc=1，门控 10s 超时） | — | 不可用 | — |
| 2 | 214 | 6522/8633 | 117 | 12924/19279 |
| 4 | 420 | 5317/6581 | 212 | 10712/12908 |
| 8 | 407 | 9618/16158 | 212 | 18472/21364 |
| 16 | 424 | 9225/11531 | 212 | 18457/21645 |

三点结论：① depth=1 不可用（MPP 需 ≥2 输入才出首帧）→ 最小可用深度为 2；
② 2→4 吞吐翻倍（+96%/+82%），4→16 持平（±3%）→ **拐点 depth=4**，
达到单会话上限的约 99%；③ depth=4 的 sojourn 分位反而最好
（1080p p99 6581 vs depth16 的 11531；4K 12908 vs 21645）——更深只加
排队不加吞吐，与第 3 节理论预测逐字一致。depth=4 即"小缓冲"的定量定义。

附带：depth=16 的 fps（424/212）与 round1/round2 旧门控数（425/212）一致，
门控口径变更未移动上限，可比性成立。

## 3 并发：aggregate 见顶，单流 stretch 可接受（@depth=4）

| 会话数 | 1080p min/mean/agg | stretch | rkvdec | 4K min/mean/agg | stretch | rkvdec |
|---|---|---|---|---|---|---|
| 1（单） | 420 | 1.0 | 35% | 212 | 1.0 | 45% |
| x2 | 287/288/577 | 0.68 | 99% | 133/134/268 | 0.63 | 100% |
| x4 | 166/167/668 | 0.40 | 99% | 77/78/314 | 0.36 | 100% |
| x8 | 89/89/715 | 0.21 | 100% | 42/43/341 | 0.20 | 100% |

（agg = 各会话 fps 之和；min 即最慢会话，batch 内各会话差 <2%。）

三点结论：① 单会话 rkvdec 仅 35~45%（HW 没吃饱），x2 即 99%+ 见顶——
单会话上限（424/212）不是硬件上限，**硬件真上限是多会话 aggregate
（1080p ~715fps，4K ~341fps，约为单会话的 1.6~1.7×）**；
② 即使 x8 打满，每流仍超目标（1080p min 88.6 > 60，4K min 42.1 > 30）；
③ 争抢下分位（sojourn 含排队）：1080p x4 p50 ~18~20ms/p99 ~26~28ms、
x8 p50 ~35~42ms/p99 ~49~62ms；4K x4 p50 ~40~46ms/p99 ~50~56ms、
x8 p50 ~72~87ms/p99 ~103~122ms。进预算表用"depth4 排队上界 + 并发
stretch"两项分别列，不混算。

## 4 解码盒准入（更新 §3.1 算式）

按 aggregate 上限 × 0.7 裕量（留 IDR 突发 + 内容差异）：

- 1080p60：715 × 0.7 / 60 ≈ **8 路**；
- 4K30：341 × 0.7 / 30 ≈ **8 路**。

即单 RKVDEC（本配置 CBR H264）在额定帧率下可规划 **8 路**。
每流 depth=4，小缓冲排队上界 4 × service（1080p ~10ms/4K ~20ms），
网络抖动由 playout 既有 jitter buffer 吸收（两级解耦不变）。
并发 stretch 数据同时是第 2 节第 4 项（多流 stretch）的回填。

## 5 遗留

- 生产 `shared/mpp_decoder.c` 的 shallow-async 模式未动（门控三者其一：
  多流落地 / 反压堆积 / HW 饿死间隙；目前三个条件：多流需求已明确，
  后两者待生产 RX `dec_p50/p99` 实测回填——见第 2 节第 1 项）。
- depth=1 的 10s 门控超时是探针表征行为（min viable depth 数据点），
  不是缺陷；生产 depth 只会取 2~4，不触此线。
- 本轮探针二进制与 `~/code/mtl/src` 快照一致（`--rebuild` 全量重编验证过）。
