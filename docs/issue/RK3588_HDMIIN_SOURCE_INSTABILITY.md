# RK3588 HDMI 输入源抖动（采集验证阻塞）

日期：2026-11-07。板上实测（RK3588，`rk_hdmirx`，`/dev/video0`）。

## 1. 现象

- 同一源在约一小时内从“可用”变为“不可用”：早期 `v4l2-ctl --stream-mmap`
  可采 30 帧，后续自研探针与 `v4l2-ctl` 均零帧（`DQBUF: Invalid argument`）。
- dmesg 特征：`format_change: New format: 1920x1080p60` 与
  `signal lock ok` 约每 1~2 秒交替出现，STREAMON 时刻报 `timing is invalid`。
- 采集耗时预算因此暂缺；编码、解码、RGA 三项已用预置流完成（见性能归档）。

## 2. 关键教训：单次时序查询不可信

- `query-dv-timings` 在抖动期间仍返回缓存的 1080p60 值，具有误导性；
  驱动只在 STREAMON 时刻判定有效性。
- 预检要求：10 秒 dmesg 窗口内无 `format_change` 反复触发、
  无 `timing is invalid`，方可开始采集验证（已写入验证方案）。

## 3. 验证步骤与期望结果（源端预检，每次采集验证前必做）

```bash
# ① dmesg 静默窗口（期望：计数为 0）
dmesg -C; sleep 10; dmesg | grep -cE 'format_change|timing is invalid'

# ② 时序快照稳定性（期望：两次 pixelclock 完全一致，60.01fps）
v4l2-ctl -d /dev/video0 --query-dv-timings | grep Pixelclock; sleep 10
v4l2-ctl -d /dev/video0 --query-dv-timings | grep Pixelclock

# ③ 试采改用脚本（显式固定 BGR3，失败隔 10 秒重试，最多 3 次；退出码即结论）
bash debug/v4l2_stream_check.sh 30 3 ~/code/mtl/perf/check.log
```

期望结果汇总：

| 退出码 | 含义 | 下一步 |
|---|---|---|
| 0 | 试采成功 | 采 `cap_interval.csv` 并计入预算 |
| 2 | 流启动被拒 | 看 §4 分类后重试，重试不过转 §5 |
| 4 | 无信号 | 检查源供电/线缆/输入选择 |
| 其他 | 格式/设备异常 | 转格式文档排查 |

只有退出码 0 才能采数计入预算。

## 4. 试采失败分类（`DQBUF: Invalid argument` 有两种成因）

- 同一命令行内 `S_FMT` 已失败（如 NV12 测试）：队列协商根本没建对，
  后续 DQBUF 必然 EINVAL。这是格式不支持的附带现象，看 S_FMT 那行即可，
  不要误读为驱动停流。
- `S_FMT` 成功（或未指定格式）但 DQBUF 报 EINVAL：STREAMON 返回 0 之后
  源端抖动，驱动中途停流（dmesg 可见 `stream start stopping` 紧跟
  `start_streaming`，或直接报 `timing is invalid`）。阻塞中的 DQBUF 因此
  变 EINVAL。这是本 issue 的抖动现象，按 §3 重试与排查，不要改程序。

## 5. 最新进展（物理重联后仍未恢复）

- 物理重联线缆后复测：故障依旧，试采脚本退出码 2。
- 当时板子刚重启（dmesg 时间戳 100 秒内），驱动为干净状态，
  排除状态 wedging；extcon 显示 VIDEO-IN=1、控制器 connected，
  DV 锁存连续 6 次成功，仍报 `timing is invalid`。
- 观察：pixelclock 读数 148.52MHz（标准 1080p60 为 148.5MHz），
  怀疑源模式非标准 VIC 或深色/认证细节被流启动校验拒绝，
  需源端配合确认。

## 6. 遗留设置排查（回应“之前操作是否留下异常设置”）

逐项核对，所有操作均无跨重启残留：

| 操作 | 是否可能残留 | 依据 |
|---|---|---|
| v4l2-ctl S_FMT（含失败的 NV12） | 否 | 失败返回 EINVAL 不改状态；成功也只影响当次打开句柄 |
| 自研探针 S_FMT/G_FMT/REQBUFS/STREAMON | 否 | 同上，且板子已重启，驱动为全新 probe |
| `--set-dv-bt-timings query` | 否 | 当场 EINVAL 被拒，未生效 |
| 驱动 unbind/bind（中途中断） | 否 | 重启后重新 probe，`/dev/video0` 时间戳与启动时间一致 |
| EDID | 否 | 重启前后前 5 行逐字节一致；从未执行过 S_EDID |
| 板上新增文件 | 否 | 仅 `~/code/mtl` 下探针与脚本，无守护进程 |

板子 uptime 证实重启过，`Video Capture Multiplanar + Streaming` 能力正常，
因此“越弄越坏”不是操作残留。时间线还原：最初一次试采成功 → 源暂停 →
恢复后流启动校验持续拒绝（锁存稳定但 `timing is invalid`）。
最可能的是源暂停/恢复改变了输出细节（模式、深色、HDCP 会话），
而非接收端被改坏。

## 7. 待确认与需要的配合

- 源设备型号与“暂停/恢复”前后输出模式是否一致（分辨率、深色、HDCP 开关）；
- 源端彻底断电重启一次（排除源侧 HDCP/模式残留），再用 §3 脚本重测；
- 换一路标准 1080p60 源或换线后，用 §3 脚本重测；
- 在确认标准源可采之前，采集耗时预算保持空缺。
- 生产采集器对无信号/占位分辨率已有优雅失败与周期重试，
  本问题不影响该逻辑，只阻塞性能基线采集。
