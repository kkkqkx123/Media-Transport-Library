# RK3588 HDMI 音频无声：ALSA 起播阈值过大且缺 EBADFD 恢复【已修复已复测】

日期：2026-11-10。板上实测（RK3588，`rockchip-hdmi0/card0`，`file_playout_rx` + `live_ingest --source file`，`T1 30s`）。

## 1. 现象

- 显示器有画面但扬声器无声，旧 `T1` 全绿（只查帧数不断流）。
- 补静音断言后 `T1` 明确失败，数字域与驱动消费分叉：
  - `TX audio exit peak_max=4912 silent=0`，`RX mtl_rx_audio peak_max=4912 silent=0`，网络链路正常，片源 `volumedetect mean -31.9dB max -16.5dB` 非静音。
  - `ALSA` 侧 `write_err=26585/26957`，`close written=17856 exp~1440000`，`/proc/asound/card0/pcm0p/sub0/status` 全程 `PREPARED→SETUP`，`hw_ptr=0` 从未 `RUNNING`。
  - 日志刷屏 `alsa: write error: File descriptor in bad state`，首错即 `EBADFD`，无 `EPIPE` 前兆。
- 单独 `alsa_tone default` 可播，`48帧×500` 同参数 `set_params` 紧循环可播，视频独播加并行小包写也可播，排除声卡硬件与全局 `HDMI` 独占。

## 2. 根因链

- 起播阈值等于缓冲大小。`snd_pcm_set_params latency 500ms` 落为 `buffer 24000 period 6000 start_threshold 24000 avail_min 6000`，硬件需攒满 `500ms` 才 `START`，与视频 `playout_delay 80ms` 和 `1ms` 流式写天然不匹配，`hw_ptr` 长时间不动。
- 恢复只处理 `EPIPE`。`writei` 在 `SETUP/XRUN/SUSPENDED` 等态回 `EBADFD`，旧码不做 `prepare/recover`，一次进 `SETUP` 即永久刷错，`total_written` 停在 `17856`，缓冲打满后全败。
- 计数掩盖故障。`mtl_rx_audio` 无论写成败都 `stat_frames++`，旧 `T1` 只查帧数与 `open failed`，静音必漏过（已由本次补断言堵住）。

## 3. 验证步骤与期望

- 预检片源非静音：`ffmpeg -af volumedetect` 要求 `max_volume` 非 `-inf` 且高于 `-60dB`。
- 跑 `board_env.sh run -- ./run_multinode_composite.sh T1 30`，期望新增断言全过：`TX/RX peak_max>=500`、`silent<50%`、`write_err==0`、无 `alsa: write error`、`close written>=0.7*48000*sec`、探针日志见过 `RUNNING`。
- 板上 minima：`/tmp/paced_test`（`48帧+usleep 1ms×3000`）在无视频时应全过，在视频独播（`--audio-udp 0`）并行时也应全过，用于区分全局干扰与本模块问题。

## 4. 修复（已实施，已复测通过）

- `alsa_audio` 存设备名，`set_params` 后经 `sw_params` 把起播阈值降到约 `100ms`、唤醒粒度降到一帧，使 `1ms` 流早起播早消费，避免攒满才起。
- 写路径统一恢复：`EPIPE/ESTRPIPE` 用 `snd_pcm_recover` 重试，`EBADFD` 用 `prepare` 重试，仍败则关查重开一次再试，仍败才计数并限频打印，不再无条件继续假装成功。
- 不改公有接口与线程模型，`dvs_media` in-tree 复用同文件自动生效。
- 旧二进制缺 `peak_max/close written` 字段时新 `T1` 明确失败并提示重编，不做兼容静默放过。

## 5. 复测结论（2026-11-10，`T1 30s`）

- 全绿退出 `0`：`TX peak 4912 silent 0`，`RX peak 4912 silent 0 write_err 0`，无 `alsa: write error`，`close written=1291680 exp~1440000`，探针全程 `RUNNING hw_ptr` 递进。
- 对比修复前：`write_err 26585`、`written 17856`、`SETUP hw_ptr 0`，确认本修复即无声根因，非片源与网络问题。
