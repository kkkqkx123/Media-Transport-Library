# 解码 async 复核、延时缺口与小缓冲异步解码

日期：2026-11-11。承接 `mpp-async-ceiling-verification-design.md` §1.2
（生产解码保持同步、压测完再定）与 `codec-latency-semantics.md`（延时语义）。
本项目重点是延时不是 fps，以下逐条回答。

## 1 解码 async +30% 的结论：没变，反而更硬了

两次独立矩阵、两次启动、两个分辨率，ratio 几乎逐位复现：

| | 1080p sync→async | 4K sync→async |
|---|---|---|
| round1（boot -1） | 333.8→425.35（+27%） | 161→212.1（+32%） |
| round2 dmaheap2（boot 0） | 332.9→424.3（+27%） | 161.5→211.05（+31%） |

编码侧的随机多秒间隙在解码侧一次没出现（dec 探针无门控结构，
约 8 次 async run 全部干净 PASS）。结论维持：**单会话解码吞吐上限
在 async 口径下比同步高约三成**，置信度因复现而提高。

但必须澄清 "+30%" 的含义：它是流水吞吐（put/get 重叠 + MPP 内部
流水），不是单帧 service 变快。延时预算项用 sync 口径（service），
容量规划用 async 口径（上限），两者不可互换（§1.3 已有此戒律，
此处重申是因为下面的小缓冲设计正好踩在这条线上）。

## 2 延时覆盖盘点：有什么、缺什么

已有（`codec-latency-semantics.md`）：探针单帧 MPP 进出（enc sync
1080p ~4ms / 4K ~14.9ms；dec sync 1080p p50 4952/p99 7034us、
4K p50 6754/p99 13939us）、生产 TX `enc_cost` 分位（预算口径）、
生产 RX `dec_p50/dec_p99`（11-09 加上）、E2E-C（采集→flip 提交）、
Python 编排零扰动已验证。

按"延时是重点"衡量，缺四块，按优先级排：

1. **生产 RX `dec_p50/p99` 零实测。** 指标加上了（只编译通过），没跑过数。
   这是预算表 dec 段闭环的钥匙：跑一次 file_playout RX（含网络抖动 case），
   回填 dec 段，并观察 `put_fail` / `drain_pull` 是否堆积。
2. **E2E 预算表未闭环。** `E2E-C ≈ cap + rga + enc + net + dec + playout + flip`，
   其中 dec 段等第 1 项回填，net 段（MTL pacing + 网络）与 playout 段
   （AV sync 缓冲等待）无独立测量。做法：一次带齐全打点的 e2e run，
   用 `e2e_latency_budget.py` 把表填实，而不是各段各自为政。
3. **p50/p99 尾因未归因。** dec 4K p99/p50 = 2.06，enc 生产 p99 6.3ms vs
   探针单点 4ms。候选：IDR（GOP=60 周期尾）、RC、CBR 启始、MPP 批输出、
   调度。补充做法（只改探针，最便宜）：按 GOP 相位（seq % gop）分组
   打印分位，一次 run 即知尾是周期性（IDR/RC）还是随机（调度/饿死），
   再定要不要 IDR-aware 预算。enc async 的 gappy run 不在此列
   （那是门控 artifact，p99 恒等于超时，已有定论）。
4. **多流共享 HW 的 stretch 未测。** 单会话上限 ≠ 多会话并发时每流延时。
   做法：板上多进程并发跑 dec 探针（2/4 会话同跑），各记分位，
   看每流 p50/p99 相对单会话 stretch 多少。这是解码盒容量规划的直接输入。

第 4 项正是下一节小缓冲 async 的前置数据。

## 3 小缓冲异步解码：要，但按条件、分两步

### 3.1 "2 倍"的实测口径

`dec-async / enc-sync`：1080p 424.3/256.3 = **1.66×**，
4K 211.05/71.1 = **2.97×**。1080p 按 2× 规划偏乐观（实测 1.66），
4K 按 2× 规划偏保守。解码盒准入按分辨率取实测比 × 0.7 裕量
（留 IDR 突发 + 网络 case）：

- 允许接入流数 × 单流 fps ≤ 解码上限 × 0.7。

### 3.2 为什么是"小缓冲"async，而不是 sync 或深 async

- sync（现状，depth 1）：单帧延时最优（= service，1080p ~5ms），
  但 HW 输入易饿死——网络抖动导致 put 断流时 HW idle，吞吐掉且尾抖；
  多流时各会话串行 put/get 交替，抗抖动差。
- 深 async（探针现状 outstanding 16）：吞吐到顶，但排队上界
  16 × service（1080p ~80ms），直接吃掉延时预算，不可接受。
- 小缓冲 async（depth 2~4）：重叠增益只需要 ≥2 的深度（消除串行
  put→get 往返，MPP 内部流水持续有料），更深只加排队不加吞吐；
  排队上界 2~4 × service（1080p ~10~20ms），有界、可进预算表；
  网络抖动由 playout 既有 jitter buffer 吸收，两级解耦、各司其职。

单流延时关键路径保持 sync（depth 1 单帧延时最小，无可争议）；
解码盒（网络输入、多流）用小缓冲 async。这是"延时优先"下的最优折中，
不是"吞吐优先"的复刻。

### 3.3 两步走（仍不动生产代码，直到门控满足）

第一步（探针 only）：dec 探针 `DEC_ASYNC_MAX_OUTSTANDING` 参数化为
`--depth`，扫 N = 1/2/4/8/16 的 fps + p50/p99 + put_fail，
找到拐点（预期 N = 2~4 即达上限约 95%，再深只涨 p99 不涨 fps）。
同时跑第 2 节第 4 项的多会话并发，确认每流 stretch 可接受。

第二步（动生产，需门楣三者其一）：多流需求落地（N≥2 会话），或
生产 RX 出现 `put_fail` / `drain_pull` 堆积，或回填的 `dec_p50/p99`
显示 HW 饿死间隙。改动面：`shared/mpp_decoder.c` 加 shallow-async
模式（默认 sync 不变，file_playout/dvs_media 按需开），每流独立
session + depth 2~4，沿用现有 `dec_p50/p99` + 反压计数作可观测，
准入按 §3.1 算式。depth > 4 不做（无益，只加延时）。

在此之前，§1.2 "不动生产解码代码"继续有效。

### 3.4 验证结果（11-11，见 `docs/archive/perf/2026-11-11-decode-shallow-async-verify.md`）

第一步已完成：`--depth` 扫点确认**拐点 depth=4**（2→4 吞吐翻倍，4→16
持平 ±3%；depth=4 的 sojourn 分位反而最优——1080p p99 6581us vs
depth16 的 11531us）；depth=1 不可用（MPP 需 ≥2 输入才出首帧）。
并发批跑（@depth=4）：x2/x4/x8 stretch 0.65/0.38/0.20，aggregate 分别
577/668/715fps（1080p）与 268/314/341fps（4K），x2 即 rkvdec 99%+
见顶——单会话上限不是硬件上限，硬件真上限是 aggregate（约为单会话
1.6~1.7×）。准入更新为 aggregate 口径：1080p60 与 4K30 均约 8 路
（0.7 裕量）。x8 打满时每流仍超目标（88.6 > 60，42.1 > 30）。
生产 shallow-async 模式待门控后两者（反压堆积 / HW 饿死间隙）回填后定。
