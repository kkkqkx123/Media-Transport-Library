# 编解码延时测量语义分析 — "测的到底是什么"

> 整理日期：2026-11-09。回答一个问题：当前 RK3588 测试里各个"编解码延时"指标
> 的精确语义是什么，是否正确，能否反映 **MPP 进出时间间隔**（帧送入 MPP → 帧从 MPP 取出）。
> 依赖前篇：`docs/tests/codec-perf-test-logic.md`。探针源码在 `debug/`
> （`mpp_enc_frame_latency.c` / `mpp_dec_frame_latency.c`），板端二进制 `~/code/mtl/bin/`。
> 文中实测数据来自 ssh board（2026-11-09，clock 流）。

## 一、指标语义总表

| 指标 | 采样点 | 精确语义 | 是"纯 MPP 进出"吗 |
|---|---|---|---|
| 探针 `mpp_enc_latency` mean/p50/p99 | `t0`=encode_put_frame 前；`t1`=encode_get_packet 取到包后 | 单帧送入空闲编码器 → 码流包取出，串行、无流水 | **是**（干净，含 ≤1ms 轮询量化） |
| 探针 `mpp_dec_latency` mean/p50/p99 | `t0`=decode_put_packet 前；`t1`=轮询取出可显示帧后 | 单 AU 送入空闲解码器 → 首个可显示帧取出，串行 | **是，但有 ~25ms 测量上限** |
| TX `enc_cost`（tx_pipe[1s]） | `submit_push` 于 encode_put_frame 前；`done_us` 于收集线程收齐码流并入 ring 后 | 帧**提交编码器 → 码流收齐入环形队列**，含收集/拷贝/排队，EMA 平滑 | **否**（预算口径，大于纯 MPP 进出） |
| TX `enc_p50/p90/p99` | 同上 cost 的滑动 300 帧窗口 | 同上口径的分位数 | 同上 |
| RX `dec=`（RX[..] 行） | `stat_decoded++` 于解码器拉出帧时 | **帧计数，不是时长** | —（无逐帧解码延时指标） |
| RX `E2E-C` | TX SEI 戳（V4L2 回调时刻）→ RX `drm_renderer_display` 返回后取 `time_now` | 采集 → 显示提交（flip 提交，非实际扫描输出） | 全链路口径 |
| `mpi_dec_test` fps | 工具自身 | 稳态吞吐（帧率），非单帧延时 | 吞吐口径 |

结论先行：**只有两个 debug 探针真正测"MPP 进出时间间隔"，且都是"空流水线单帧"口径；
生产管线里的 enc_cost 是更宽的预算口径；RX 生产路径没有任何逐帧解码时长指标。**

## 二、逐项分析

### 2.1 编码探针（debug/mpp_enc_frame_latency.c）

流程：memcpy 进 MPP buffer → `t0` → `encode_put_frame` → 轮询 `encode_get_packet`
（1ms×200 次，上限 200ms）→ `t1`。串行喂帧，每帧等取到包才喂下一帧。

实测（1080p，300 帧）：

```
frames=300 1920x1080@60fps level=42 mean_us=4555.2 p50=4561 p90=4757 p99=4825 out_kbps=7889
```

**语义正确性**：
- ✅ 时间戳位置干净：memcpy 在 `t0` 之前，不含拷贝；确为 MPP 进→出。
- ✅ 串行测得的正是"空流水线单帧延时"——做单帧预算分解时这是对的口径。
- ⚠️ **注释与代码不符**：注释说 "prime the pipe with one frame so steady-state cost
  excludes startup"，但代码**没有丢弃首个样本**，`cost[0]` 含首帧编码路径
  （SPS/PPS 生成等），对 mean/p99 有小幅高估，p50 不受影响。要么真跳过首样本，要么改注释。
- ⚠️ 1ms 轮询量化：测量值最多被抬高 ~1ms（1080p 均值 4.5ms 时约 20% 量级）。
  若要更准，可在 `encode_get_packet` 里用 MPP 的阻塞/超时语义代替 sleep 轮询。

### 2.2 解码探针（debug/mpp_dec_frame_latency.c）

流程：按 start code 切 AU → `t0` → `decode_put_packet` → 轮询
`decode_get_frame`（50 轮 × 0.5ms sleep，**总上限 ~25ms**）→ `t1`。
非帧 AU（SPS/PPS/SEI）必然轮空计入 `no_output`；`put_fail` 为解码器反压。

实测：

```
1080p: au_fed=123 framed=119 no_output=4 put_fail=0 mean=4968.5us p50=5409 p90=5619 p99=7311
4K:    au_fed=64  framed=59  no_output=5 put_fail=0 mean=7215.3us p50=6754 p90=7782 p99=13939
```

（no_output 4~5 正好等于参数集 AU 数，`rx_decode_perf.py` 允许 12 个的口径与之吻合。）

**语义正确性**：
- ✅ 对"单 AU 进解码器 → 首帧出"的间隔测量本身正确；且是串行空流水口径。
- ⚠️ **~25ms 隐形测量上限**：轮询循环 50×(get_frame+0.5ms) ≈ 25ms。解码耗时超过
  ~25ms 的帧不会被记为慢样本，而是被**误分类为 `no_output`**——在 4K 高码率/复杂
  内容下，一旦真实帧耗时越过该线，统计会从"延时偏大"翻转成"无输出 AU 过多"，
  触发 `rx_decode_perf.py` 的 FAIL 分支，结论性质完全不同。本轮实测 4K p99=13.9ms
  未触线，但这块天花板必须写进口径文档。修法：轮询轮数按 `--max-au`/fps 参数化，
  或用阻塞式 `decode_get_frame`（MPP 支持超时语义）。
- ⚠️ 串行口径 ≠ 稳态吞吐：RKVDEC 有流水深度，连续喂流下单帧吞吐低于串行延时
  推算值。预算用本探针，容量评估需另测 fps（但见 2.4，fps 兜底路径在本板不可用）。
- ⚠️ `put_fail` 时该 AU 被直接丢弃：持续反压下样本集偏向轻负载帧，mean/p99 偏乐观。

### 2.3 生产 TX：`enc_cost` / `enc_p50/p90/p99`（codec/mpp_encoder.c）

- `submit_push(now_us())` 在 `encode_put_frame` 之前（`enc_send` 尾部）；
  `done_us = now_us()` 在**收集线程**收齐该帧全部 partition 包（EOI）、memcpy 进 slab、
  `ring_push` 成功之后。`enc_cost` = 两者之差的 EMA（×9/10 整数平滑）；
  分位由最近 300 帧（60fps 下约 5s）滑动窗口排序得到。
- **这不是纯 MPP 进出**：额外包含收集线程等待/搬运、以及 MPP 内部队列中
  前序帧造成的排队。定位是"这帧的数据什么时候可以交给发送"的**预算口径**——
  对 E2E 预算表（e2e_latency_budget.py 的 encode 段）这是正确的口径；
  但拿它跟编码探针对比时会"偏大"，二者不可互换。
- ⚠️ **配对错位缺陷**：`submit_q` 只在输出成功（`ring_push==0`）时被消费。
  两处提前 continue 的丢帧路径（码流超 slab `total==0`、ring 满丢弃）会**遗留
  过期的 submit 时间戳**：下一个成功帧的 cost 从被丢帧的提交时刻起算，系统性偏大，
  直到 `ENC_SUBMIT_Q` 环绕覆盖头部才恢复对齐。丢帧罕见时无感，但丢帧恰恰是
  最需要看延时的时刻——建议丢帧路径同步 `submit_pop` 消费一个时间戳。

### 2.4 生产 RX：没有逐帧解码延时，`dec=` 只是计数

- `rx_pipeline.c` 的 `dec=+N` / total `dec=N` 是 `stat_decoded++`——**帧数**。
  `rx_decode_perf.py --mode mtl` 对"解码延时"的全部判定只有产出比
  `dec/pkt ≥ 0.99` 和 playout 欠载，**不含任何时长**。逐帧解码延时长只能靠
  offline 模式的外置探针离线推断。
- 若要在生产路径补上这个指标：在 `rx_drain_decoder` 的 `mpp_dec_pull` 成功处
  对 (feed 时刻 → pull 时刻) 记窗口分位即可，结构与 mpp_encoder 的
  submit/latency 窗口同构，改动面小。
- ⚠️ 实测发现：**`mpi_dec_test` 在本板 rc=0 但输出 0 字节**，`rx_decode_perf.py`
  offline 兜底路径（无 `--dec-binary` 时）必然报「mpi_dec_test 输出未解析」FAIL。
  该兜底在这块板上实际不可用，文档/脚本应标注。

### 2.5 E2E-C（shared/rx_e2e_stats.c）

- TX 戳：`tx_encode_on_frame` 在 **V4L2 回调**取 `time_now`（TAI）作 `cap_ts`，
  编码完成后 `sei_timestamp_build` 以该值内嵌 SEI NAL——**戳是采集时刻，不是编码
  完成时刻**，语义正确。
- RX 侧：`rx_render_frame` 在 `drm_renderer_display` 返回 0 后取 `time_now` 记录。
  即 E2E-C = **采集 → 显示提交（page flip 提交）**，比"玻璃到玻璃"少一帧
  flip→扫描输出的增量（≤1 个帧周期），方向上略偏小但口径自洽、可比。
- 反向样本（present < capture）计入 `skew_reject` 而非负延时——时钟偏斜防护正确；
  `rx_decode_perf.py`/`e2e_latency_budget.py` 以 `skew>0 → FAIL 作废` 处理一致。

## 三、回答核心问题

**"现有计算能否准确反映 MPP 进出时间间隔？"**

1. **能，但仅限两个 debug 探针**，且都是"空流水线、逐帧串行"的单帧口径：
   编码探针干净（量化 ≤1ms + 首样本未剔除）；解码探针正确但有 ~25ms 隐形上限，
   超限帧会被误归类为 `no_output` 而不是慢样本。
2. **生产 TX 的 `enc_cost`/分位不是 MPP 进出**，是"提交→码流可发送"的预算口径，
   另有丢帧配对错位缺陷会使其虚高。
3. **生产 RX 完全没有逐帧解码时长指标**（`dec=` 是计数），解码延时的判断
   目前依赖离线探针外推 + E2E-C 间接覆盖。
4. 各口径的换算关系：`E2E-C ≈ cap_int + rga + enc_cost(预算口径) + 网络 + 解码(未测) +
   playout + flip`。预算表用 tx_worst 时，encode 段用 `enc_p99`（预算口径）与
   用编码探针 p99（硬件口径）会差出一个收集/排队增量，对照时应注明口径。

## 五、Python 编排层是否引入额外延迟（2026-11-09 实测）

**结论：不引入可测量的延迟。** 依据：

1. **时间戳全部在 C 进程内部打点**：两个探针用 `CLOCK_MONOTONIC`（`mpp_*_frame_latency.c`
   的 `now_ns`），生产 enc_cost 用 `mpp_encoder.c` 的 `now_ns`。Python
   （`tx_capture_encode_perf.py` / `rx_decode_perf.py`）只负责 `subprocess` 拉起二进制、
   逐行读 stdout 做正则解析、0.5s 周期读 `/proc` 采样——全部发生在测量点之外，
   测量期间 Python 阻塞在 `subprocess.run`/`Popen` 读管道上，几乎不占 CPU。
2. **实测扰动对照**：4K 编码探针裸跑 mean=14891us / p99=15656us；模拟编排层负载
   （0.5s 周期并发读 `/proc/mpp_service/load` + `/proc/stat`）后 mean=14829us /
   p99=15389us——差异在噪声内（<0.5%），无系统性偏移。
3. 唯一的理论风险是 stdout 管道反压（被测进程写满 64KB 管道缓冲被阻塞），实际
   `tx_pipe[1s]` 行 1Hz 一条，编排层实时逐行读取，不构成风险。

**4K 编码实测 vs 10ms 理论值**：

```
frames=120 3840x2160@30fps level=51 mean_us=14891 p50=14838 p99=15656（裸跑）
frames=120 3840x2160@30fps level=51 mean_us=14829 p99=15389（带编排层负载）
```

实测 **~14.9ms，未达到 <10ms 理论值**，且与测量环境无关（调频 ondemand、
结温 33°C，非降频/过热因素）。解释与后续验证方向：
- 本探针是**空流水线串行口径**（喂一帧等一帧），含完整的硬件流水延迟；
  连续喂流时生产 `enc_cost` 口径只会更大（含收集/排队），不会更小；
- 10ms 理论值可能基于更低延时配置（更低 level/码率、质量档位）或非 CBR 的
  内部参考数据；当前生产配置 CBR 8Mbps、h264 level 51、rc 平滑系数默认；
- 建议用 `mpp_enc_frame_latency` 扫 `--bps`/level/qp 上限组合找 10ms 的成立条件，
  并在 TX 生产路径跑 4K mp4 源对照 `enc_cost`（预算口径）确认差距。
- 注意 14.9ms < 30fps 帧预算 33.3ms，单帧延时本身不构成 4K30 的吞吐瓶颈。

## 四、建议修正清单（按影响排序）

> 全部已修复（2026-11-09），实测验证记录见各条括注。

| # | 问题 | 位置 | 修法 | 状态 |
|---|---|---|---|---|
| 1 | 解码探针 25ms 轮询上限会把慢帧误分类为 no_output | `debug/mpp_dec_frame_latency.c` | 轮询改为 `--wait-ms` 截止期（默认 50ms，覆盖 4K30 预算），采样粒度 500us→100us | ✅ 已修；板端复测 1080p p50 4952/p99 7034us，与修复前口径一致 |
| 2 | enc 探针未丢弃首样本，与注释不符 | `debug/mpp_enc_frame_latency.c` | 首帧仍编码但计入 priming，不进样本集 | ✅ 已修；板端复测 1080p mean 4503us（n=119） |
| 3 | 生产 enc_cost 丢帧路径 submit 时间戳错位 | `codec/mpp_encoder.c` 收集线程 | `total==0` 与 `ring_push` 失败两条丢帧路径均补 `submit_pop` 消费（并修复 ring 满时 slab 块泄漏） | ✅ 已修，三 app 交叉编译通过 |
| 4 | RX 生产路径无逐帧解码延时 | `rx/rx_pipeline.c` | feed→首帧输出滑动 300 帧窗口，周期统计行新增 `dec_p50=xxus dec_p99=xxus` | ✅ 已修，playout_rx 编译通过 |
| 5 | mpi_dec_test 兜底在本板零输出 | `tests/perf/rx_decode_perf.py` + README | 自动探测 `~/code/mtl/bin/mpp_dec_latency` 默认路径；mpi_dec_test 解析失败时报错给出改用探针的指引 | ✅ 已修 |

## 六、11-11 补测：GOP 相位尾因与编码重测（本节新增）

- **p99 尾无 GOP 周期性。** `--csv` 逐帧成本按 `seq % 60` 分组：enc
  1080p/4K 的 top 成本样本相位随机（6,5,26,11,23,…），带宽仅
  ~55~70us；dec 的大值全是启动前几帧（首帧解码器初始化），其后随机。
  结论：尾 = 启动瞬态 + 系统噪声，无 IDR/RC 周期项；warmup 跳过已覆盖
  最坏部分，无需 IDR-aware 预算。
- **编码 sync 三轮复现，async-enc 不可复现。** sync（file/dmaheap）：
  1080p 246.0/256.2、4K 70.4/71.1fps，与 round1/round2 逐位一致；
  持续 2000 帧 `mean 3969 p50 3963 p99 4117`，零漂移（无降频/过热）。
  async 同配置重测 14~20fps（p99 = 秒级门控超时），round1 的
  192/51fps 为 MPP 状态相关的幸运值，不再引用；编码上限以 sync 为准。
- **dec 并发对照（plan 决策依据）。** x4 sync aggregate 776~800、
  stretch 0.58，x4 async 659~668、stretch 0.40（交错复测两轮确认）；
  x2 sync stretch 0.98~1.0。并发 sync 已打满 HW（rkvdec 99%），
  生产吞吐侧不替换，详见 `docs/plan/rk3588/RK3588_DECODE_ASYNC_PLAN.md`。
