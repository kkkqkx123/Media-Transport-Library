# 编解码性能测试逻辑分析（perf）

> 整理日期：2026-11-09。范围：仓库内两套互相独立的编解码性能测试——
> ① x86 上游遗留的 `app/perf` 帧格式转换/DMA 基准（C，DPDK 环境跑）；
> ② 本 fork RK3588 板端的 `tests/perf` 采集/编解码画像编排（Python，标准库直跑）。
> 设计方案原文见 `docs/tests/rk3588-capture-encode-decode-perf-design.md`，板端使用说明见 `tests/perf/README.md`。
> 延时指标的精确语义与正确性分析见 `docs/tests/codec-latency-semantics.md`。

## 一、总体结构

```
app/perf/                        # x86: 像素格式转换 SIMD/DMA 微基准（不涉及编解码器）
├── perf_test.sh                 # 逐个 Perf* 可执行文件批量驱动
├── rfc4175_422be10_to_le.c      # 每个转换方向一个 main，编译成 Perf<方向>
├── ...（共 14 个转换 + 1 个 PerfDma）
└── perf_dma.c                   # mtl_udma 拷贝 vs memcpy vs mtl_memcpy

tests/perf/                      # RK3588 板端画像编排（无 meson，python3 直跑）
├── common/
│   ├── env_probe.py             # 环境基线（板型/内核/MPP/RGA/V4L2/调频策略）
│   ├── hw_load_sample.py        # rkvenc/rkvdec/rkrga/CPU 负载采样
│   ├── report.py                # JSON + Markdown 报告落盘
│   └── thresholds.json          # 全部判定阈值（单一事实来源）
├── tx_capture_encode_perf.py    # TX 侧：采集+编码画像与预算判定
├── rx_decode_perf.py            # RX 侧：解码吞吐画像（offline / mtl 两模式）
├── e2e_latency_budget.py        # 端到端：单板回环三段延迟预算
├── gen_test_stream.sh           # ffmpeg 合成预置流（NV12 + H264）
└── README.md
```

两套互不复用：`app/perf` 测的是传输层内像素搬运/转换原语的上限（x86 SIMD + DMA），`tests/perf` 测的是 RK3588 硬件编解码链路（V4L2 采集 → RGA → MPP 编码 → MTL TX / MTL RX → MPP 解码 → 呈现）的管线画像与延迟预算。

## 二、x86 侧：`app/perf` 转换与 DMA 基准

### 2.1 运行方式

`app/perf/perf_test.sh` 固定端口/地址（`ST_PORT=0000:af:01.0`、`ST_SIP=192.168.89.89`、`DMA_PORT=0000:80:04.0`），对 14 个转换方向 + `PerfDma` 逐个执行：

```bash
${TEST_BIN_PATH}/${perf_prog} --log_level error --p_port ... --p_sip ... \
  --dma_port ... --perf_frames 120 --perf_fb_cnt 3
```

每个方向是独立 main（`app/sample/sample_util.h` 提供参数解析与 `mtl_init`），线程绑定到 MTL 分配的 lcore 后跑基准。**无 PASS/FAIL 判定，只打日志供人工对比**。

### 2.2 单个转换基准的测量逻辑（以 `rfc4175_422be10_to_le.c` 为例）

1. `mtl_udma_create(st, 128, MTL_PORT_P)` 建 user DMA 通道；大页分配输入（`mtl_hp_malloc` + `mtl_hp_virt2iova`），malloc 分配输出；预填充 `fb_cnt` 个随机帧。
2. 对固定分辨率序列 `640x480 → 720p → 1080p → 4K → 8K`，每组分别用 `clock()` 环测 `perf_frames` 帧：
   - **scalar**（`MTL_SIMD_LEVEL_NONE`）作基准；
   - **AVX2 / AVX512 / AVX512_VBMI2**：仅当 `mtl_get_simd_level()` 支持时逐级跑，打 `%fx performance to scalar`；
   - **dma+SIMD**：仅 AVX512 及以上分支里，若 DMA 通道可用，走 `st20_..._simd_dma(dma, in, in_iova, out, ...)` 异步路径再对比。
3. 每轮输出 `time: %f secs with %d frames(%dx%d@%d buffers)` 与相对标量的倍数。

### 2.3 `PerfDma`（perf_dma.c）

纯拷贝对比：对 1080p/4K/8K × 三种块尺寸（128B / 1200B / 4096B），同一帧大小分别用
`memcpy` → `mtl_memcpy`（SIMD 拷贝）→ `mtl_udma_copy`+`submit`+`completed` 轮询，
三种方式各环测 `perf_frames` 帧，输出相对 CPU 拷贝的倍数。观察点是块尺寸与 DMA 描述符深度对吞吐的影响。

## 三、RK3588 板端：`tests/perf` 画像编排

### 3.1 分层模型（沿用 zlmediakit tests/perf 的思路）

| 层 | 工具 | 回答的问题 |
|---|---|---|
| 环境基线 | `env_probe.py` | 这块板/这版内核/MPP/RGA 是什么？版本不同则上限不可比 |
| 硬件上限 | `gen_test_stream.sh` + `rx_decode_perf.py --mode offline` | 解码器本身吞吐多少（隔离网络/收流） |
| 管线画像 | `tx_capture_encode_perf.py` / `rx_decode_perf.py --mode mtl` | 挂上采集/收流后预算被谁吃掉 |
| 端到端 | `e2e_latency_budget.py` | 三段预算拼起来落在包络内吗 |

### 3.2 数据来源：全部依赖应用自带日志行（无侵入探针）

| 日志行 | 来源 | 携带字段 |
|---|---|---|
| `tx_pipe[1s]: cap=N/s enc=N/s ... cap_fps cap_int enc_fps enc_cost lat(p50/p90/p99) qp bps rga ok/fail/pool_full/cpu_fb ema` | `live_ingest` 周期统计 | 采集/编码帧率、编码耗时分位、RGA 队列健康 |
| `RX[..]: mtl_rx=+n bridge=+n dec=+n render=+n mtl_drop br_drop corrupt incomplete` 及 `RX[..] total: pkt dec render pb_skip pb_underrun` | `file_playout_rx` 周期与汇总统计 | 各级增量与累计、playout 缓冲欠载 |
| `E2E-C: n min avg max skew_reject` | RX 侧基于 SEI 时间戳的端到端延迟统计 | 采集→呈现实测延迟与主时钟偏斜拒绝数 |
| `au_fed framed no_output put_fail mean_us p50 p90 p99` | `mpp_dec_latency` 探针（可选，`--dec-binary` 指定） | 解码送入到取出的耗时分布 |
| `decode N frames ... fps X` | `mpi_dec_test`（探针不可用时的兜底） | 解码 fps |
| `/proc/mpp_service/load`、`/sys/kernel/debug/rkrga/load`、`/proc/stat` | `hw_load_sample.py` 后台线程每 0.5s 采样 | rkvenc/rkvdec 逐核负载、RGA 负载、整机 CPU |

关键实现点：TX 画像用一个后台线程在跑被测进程的**同时**采样硬件负载（`tx_capture_encode_perf.py` `_bg()`），避免分两次跑引入源抖动差异；`hw_load_sample.sample_load()` 会先写 `/proc/mpp_service/load_interval` 设定采样窗口。

### 3.3 TX 侧判定逻辑（`tx_capture_encode_perf.py`）

1. 启动 `live_ingest <duration> --udp <port> --dip <dip>`（默认 HDMI 真实源；`--mp4` 切 `--source file --mp4 ... --loop` 文件帧源做无信号回归）。
2. 逐行正则解析 `tx_pipe[1s]`，收满 duration 后逐字段求均值/最大值，与 `thresholds.json` 的 `tx_capture_encode` 段比对：
   - `enc_fps / 目标fps < encode_fps_ratio(0.95)` → **FAIL**（编码没跟上帧率）；
   - `enc_p99 > enc_p99_max_us(33333，两帧间隔)` → **WARN**（FAIL 时保持 FAIL）；
   - rkvenc 负载 max ≥ 90% → 只记 note「触顶（上限形态，非预算形态）」，不判 FAIL——区分"硬件已到上限"与"预算超标"两种形态。
3. 输出 JSON（`tx_capture_encode.json`）+ Markdown 表（cap_fps / cap_int / enc_fps / enc_cost / p50 / p90 / p99 / rga_ema / rkvenc_max），退出码 0/1 对应 PASS/FAIL。
4. 4K 有独立阈值段 `tx_encode_4k`（p99 按 30/60fps 分 66666/33333us）。

### 3.4 RX 侧判定逻辑（`rx_decode_perf.py`）

两种模式：

**offline（默认）——隔离解码器本身：**
- 优先路径：`--dec-binary` 指向 `mpp_dec_latency` 探针，直接测"送入 AU → 取出帧"的 mean/p50/p90/p99；
- 兜底路径：`mpi_dec_test -t 7 -i <h264>` 回放，只取 fps，无阈值判定（恒 PASS，作记录用）。
- 探针路径的判定（4K 输入按文件名/宽高自动切 `rx_decode_4k` 阈值，p99 按 fps≤32 选 66666 否则 33333）：
  - `put_fail > 0` → FAIL（解码反压，消费侧跟不上）；
  - `no_output > 12` → FAIL。**口径说明写在代码注释里**：探针按 start code 切 AU，SPS/PPS/SEI 等非帧 AU 永无输出，加上解码器 priming，前 ~10 个 no_output 属正常，所以允许 12 个而不是按管线产出比判；
  - `p99` 超阈值 → WARN。
- 产出比字段 `yield = frames / au_fed` 仅记录，不作判据。

**mtl——验证收流链路等速：**
- 启动 `file_playout_rx <duration> --udp --sip --width --height --fps`，解析 RX 日志的 total 行与 E2E-C 行；
- `dec/pkt < yield_ratio(0.99)` → FAIL；`pb_underrun > 0` → WARN（playout_delay 偏小）；`skew_reject > 0` → **FAIL**（时钟偏斜使本轮 E2E 数据作废）。

### 3.5 E2E 预算逻辑（`e2e_latency_budget.py`）

1. 先起 RX（`file_playout_rx --sip 127.0.0.1`），睡 2s 再起 TX（`live_ingest --dip 127.0.0.1`）——单板回环，单调时钟可比；双板对比要求 PTP 就绪且 skew=0。
2. 复用 `parse_tx_line` / `parse_rx_log`（直接 import 两个画像脚本的函数，保证口径一致）。
3. 预算表 = TX 侧均值拆段 + RX 侧 E2E-C 实测：
   - `tx_typical = cap_int_mean + rga_ema_mean + enc_cost_mean`；
   - `tx_worst  = cap_int_mean + rga_ema_mean + enc_p99_max`（编码取最坏，采集/RGA 只有均值口径）；
   - `e2e_measured` 取最后一条 E2E-C（min/avg/max/skew）。
4. 判定：skew>0 → FAIL 作废；`budget_ms` 包络 `[70, 170]` 之外 → WARN；无 E2E-C 样本 → WARN（提示检查 SEI 戳与呈现路径）。

### 3.6 阈值汇总（`common/thresholds.json`）

| 段 | 键 | 值 | 含义 |
|---|---|---|---|
| tx_capture_encode | encode_fps_ratio / enc_p99_max_us / rkvenc_saturation_pct / full_events_allowed | 0.95 / 33333 / 90.0 / 0 | 编码帧率比、p99 两帧间隔、触顶线 |
| tx_encode_4k | enc_p99_max_us_30fps / _60fps | 66666 / 33333 | 4K 按帧率分档 |
| rx_decode | yield_ratio / put_fail_allowed / pb_underrun_allowed / dec_p99_max_us | 0.99 / 0 / 0 / 33333 | 产出比、反压、欠载 |
| rx_decode_4k | dec_p99_max_us_30fps / _60fps | 66666 / 33333 | 4K 按帧率分档 |
| rga_4k | self/scale/bgr_convert p99 | 12000/12000/15000 | RGA 三类操作上限 |
| e2e | budget_ms / skew_reject_allowed | [70,170] / 0 | 端到端包络 |

判定语义统一为三级：超"质量"线（帧率比、反压、偏斜）→ FAIL（exit 1）；超"延迟"线（p99、包络）→ WARN 保留；硬件负载触顶只记 note。

### 3.7 预置流生成（`gen_test_stream.sh`）

ffmpeg lavfi 合成，每种源同时产出 NV12 rawvideo（供编码上限测试）与 libx264 H264（供解码测试）：
- `clock`（testsrc 时钟条纹，自带时间码，最接近真实场景——默认基准）；
- `bars`（smptehdbars 静态色条，零帧间差异 → 编码器空闲开销）;
- `motion`（testsrc2 运动块 → 码控行为观察）；
- `clock720`（720p 档位做预算对比）；
- `--4k` opt-in：3840x2160 时钟条纹 60 帧 @30fps（NV12 约 746MB，默认不生成）。

### 3.8 执行与归档

```bash
python3 tests/perf/common/env_probe.py --out ~/code/mtl/perf/env.json     # 每轮必跑
bash tests/perf/gen_test_stream.sh --out ~/code/mtl/streams [--4k]
python3 tests/perf/tx_capture_encode_perf.py --duration 30 --out ~/code/mtl/perf/tx
python3 tests/perf/rx_decode_perf.py --mode offline --input .../clock_1920x1080.h264 \
  [--dec-binary ~/code/mtl/bin/mpp_dec_latency] --out ~/code/mtl/perf/rx
python3 tests/perf/rx_decode_perf.py --mode mtl --out ~/code/mtl/perf/rx   # 接真实组播
python3 tests/perf/e2e_latency_budget.py --duration 60 --out ~/code/mtl/perf/e2e
```

JSON 原始结果 + Markdown 报告写入 `--out`；`tests/perf` 不参与 meson 构建（根 `tests/meson.build` 只收 `integration_tests`）。压测与在线业务会争用硬件，对比采集指标前需停业务或记录负载基线。

## 四、两套测试的对比

| 维度 | app/perf（x86） | tests/perf（RK3588） |
|---|---|---|
| 被测对象 | RFC4175 像素转换 SIMD/DMA 原语 | 完整采集/编码/解码/呈现链路 |
| 测量方法 | `clock()` 环测固定帧数，人工看日志倍数 | 解析应用周期日志 + 驱动负载计数，自动判定 |
| 判定 | 无 | thresholds.json 三级判定（PASS/WARN/FAIL + exit code） |
| 产出 | stdout 日志 | JSON + Markdown 报告 |
| 运行前提 | DPDK E810 VF + hugepages | RK3588 板端二进制（live_ingest / file_playout_rx / mpp_dec_latency） |
| 与 CI 关系 | 不在 CI（依赖真实 NIC） | 不在 CI（依赖板卡），结论归档 `docs/archive/perf/` |
