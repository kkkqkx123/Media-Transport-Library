# tests/perf — RK3588 采集/编解码性能与延迟编排

板端 Python 脚本，只依赖标准库。设计方案见
`docs/tests/rk3588-capture-encode-decode-perf-design.md`。
思路沿用 zlmediakit 的 `tests/perf` 分层（环境基线、硬件上限、管线画像、端到端），
测量口径换成 MTL 两端应用自带统计。

## 目录

```
tests/perf/
├── README.md
├── common/
│   ├── env_probe.py       # 环境基线（板型/内核/MPP/RGA/V4L2）
│   ├── hw_load_sample.py  # rkvenc/rkvdec/rkrga/CPU 采样
│   ├── report.py           # Markdown 报告生成
│   └── thresholds.json    # 判定阈值（初值，三轮收敛后调整）
├── tx_capture_encode_perf.py  # 发送侧：采集+编码画像与预算
├── rx_decode_perf.py           # 接收侧：收流+解码画像与预算
├── e2e_latency_budget.py       # 端到端：单板回环预算表
└── gen_test_stream.sh         # 预置 NV12/H264 合成流生成
```

`tests/perf` 不参与任何 meson 构建（根 `tests/meson.build` 只收录
`integration_tests`），板端直接 `python3` 运行。

## 执行（板端，持久目录 `~/code/mtl`）

```bash
mkdir -p ~/code/mtl/perf ~/code/mtl/streams ~/code/mtl/bin

# 环境基线（每轮必跑）
python3 tests/perf/common/env_probe.py --out ~/code/mtl/perf/env.json

# 预置流（无 HDMI 或回归对比时；--4k 按需追加 4K 时钟流 60 帧 @30fps）
bash tests/perf/gen_test_stream.sh --out ~/code/mtl/streams
bash tests/perf/gen_test_stream.sh --out ~/code/mtl/streams --4k

# 发送侧画像（HDMI 真实源，需 live_ingest 已编译）
python3 tests/perf/tx_capture_encode_perf.py --duration 30 --out ~/code/mtl/perf/tx

# 接收侧画像（离线预置流；或 --mode mtl 接真实组播）
python3 tests/perf/rx_decode_perf.py --mode offline \
  --input ~/code/mtl/streams/clock_1920x1080.h264 --out ~/code/mtl/perf/rx

# 接收侧 4K 画像（优先走 mpp_dec_latency 探针，含分位与阈值判定；
# --dec-binary 省略时自动探测 ~/code/mtl/bin/mpp_dec_latency，
# mpi_dec_test 兜底在部分板卡无 stdout，不可用时会给出改用探针的提示）
python3 tests/perf/rx_decode_perf.py --mode offline \
  --input ~/code/mtl/streams/clock_3840x2160.h264 \
  --dec-binary ~/code/mtl/bin/mpp_dec_latency \
  --width 3840 --height 2160 --fps 30 --out ~/code/mtl/perf/rx4k

# 端到端预算（单板回环）
python3 tests/perf/e2e_latency_budget.py --duration 60 --out ~/code/mtl/perf/e2e
```

每轮输出 JSON 原始结果与 Markdown 报告到 `--out`（板上 `~/code/mtl/perf`），结论汇总后归档到 `docs/archive/perf/`（按日期命名）。
压测与运行中业务会争用硬件，对比采集指标前需停业务或记录负载基线。
