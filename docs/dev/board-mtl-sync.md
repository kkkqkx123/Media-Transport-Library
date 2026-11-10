# 板端 `~/code/mtl` 同步规范

> 范围：RK3588 调试工作区（`ssh board` 的 `~/code/mtl`）。正式部署位置
> （`/opt/mtl-rk3588`、系统库目录）见 `docs/dev/RK3588_DEPLOYMENT.md`，
> 两者不混用：`~/code/mtl` 只做手动调试与性能画像，不做发布。

## 1. 目录定义

板端顶层只保留五类目录，语义固定：

| 板端路径 | 性质 | 内容 | 是否进 git |
|---|---|---|---|
| `bin/` | 运行态 | 编译产物（二进制）+ 待跑 `.sh` 的可执行副本 | 否 |
| `src/debug/` | 源码镜像 | 仓库 `debug/` 全量（含 `tests/` 子目录），只读 | 是（以仓库为准） |
| `src/perf/` | 源码镜像 | 仓库 `tests/perf/` 全量（含 `common/`），只读 | 是（以仓库为准） |
| `src/playout-tests/` | 源码镜像 | `app/rk3588_playout_rx/tests/` 手动子集（见 §2），只读 | 是（以仓库为准） |
| `perf/`、`streams/` | 数据态 | 测量输出（csv/json/md/log）与输入流（nv12/h264/bgr3） | 否，长期保留 |

已废止：旧 `~/code/mtl/debug/`、`~/code/mtl/tests/`（路径与仓库不对齐的
手抄快照）、`src/` 顶层孤儿 `.c`、`bin/*.bak`、`__pycache__`，均已清理，
不再重建。

## 2. 三组映射（仓库 → 板端）

* `debug/*` → `src/debug/*`，含 `debug/tests/*` → `src/debug/tests/*`，
  共 14 文件（含 `board_env.sh`）。编译命令见 `src/debug/README.md`，产物落 `bin/`；
  其中 `board_env.sh`、`display_compare.sh` 另拷可执行副本进 `bin/`（e2e 门禁与一条龙入口）。
* `tests/perf/**/*` → `src/perf/**/*`，共 9 文件。运行命令见
  `src/perf/README.md`，输出落 `perf/`，输入流落 `streams/`。
* `app/rk3588_playout_rx/tests/` 手动子集 → `src/playout-tests/`：
  `run_tx_rx_e2e.sh`、`run_multinode_composite.sh`、
  `run_dualboard_interop.sh`、`run_dualoutput_sameframe.sh`、
  `manual_check_visual.sh`、`verify_phase.sh`、`plan_a_verify.sh`、
  `run_single_board_plan_a.sh`、`check_drm_log.py`、`check_playout_log.py`、
  `check_pts_idx.py`、`compare_au_trace.py`、`verify_vrx_margin.py`、
  `rx_minimal.c`。该目录下 meson 单元测试（`test_*.c`、`meson.build`）
  不同步。运行时把要跑的 `.sh` 拷一份到 `bin/` 后执行，
  `BIN_DIR` 默认为 `$HOME/code/mtl/bin`。

`bin/` 中 `.sh` 必须是 `src/` 对应文件的可执行副本，不得独立演进；
`md5sum` 不一致时以 `src/` 为准重拷。

## 3. 同步操作

约定：单向推送，仓库永远是源。板上永不改 `src/`，发现问题回仓库改完再推。

```bash
# 推送（仓库根执行，按需取子集）
scp debug/<file> board:~/code/mtl/src/debug/
scp debug/tests/<file> board:~/code/mtl/src/debug/tests/
scp tests/perf/<file> board:~/code/mtl/src/perf/
scp tests/perf/common/<file> board:~/code/mtl/src/perf/common/
scp app/rk3588_playout_rx/tests/<file> board:~/code/mtl/src/playout-tests/
cp ~/code/mtl/src/playout-tests/<run_*.sh> ~/code/mtl/bin/   # 板上执行

# 校验（抽查 md5，全 SAME 方为对齐）
ssh board "md5sum ~/code/mtl/src/debug/mpp_dec_frame_latency.c"
md5sum debug/mpp_dec_frame_latency.c

# 探针重编（板上执行，mpp_dec 带 --wait-ms 语义，见 docs/tests 下清单文档）
gcc ~/code/mtl/src/debug/mpp_dec_frame_latency.c \
  -o ~/code/mtl/bin/mpp_dec_latency -lrockchip_mpp
```

二进制说明：`bin/` 下 `live_ingest`、`file_playout_rx` 为交叉编译产物，
只在编译服务器重编后同步，不在板上编译；`*_latency`、`drm_smoke`、
`hdmi_passthrough`、`alsa_tone`、`v4l2_cap_latency` 为板上 `gcc` 直编产物，
源码更新后必须重编（本次已因 `--wait-ms` 修复重编 `mpp_dec_latency`）。

## 4. 禁止事项

* 禁止在板上直接编辑 `src/` 下任何文件。
* 禁止往 `src/` 放输出（csv/json/log/h264 一律进 `perf/` 或 `streams/`）。
* 禁止用 `/tmp` 存需长期保留的二进制与数据（重启即失）；`bin/perf/streams`
  即持久位置。
* 禁止在 `bin/` 留 `.bak`；禁止同步 `__pycache__`、编译中间产物。
* 数据回流方向只有一个：板 `perf/` 输出归档到仓库 `docs/archive/perf/`
 （按日期命名），命令示例见 `tests/perf/README.md`。
