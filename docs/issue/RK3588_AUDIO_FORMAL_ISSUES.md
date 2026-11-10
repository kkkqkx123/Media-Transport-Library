# RK3588 音频链正式实现前问题清单

日期：2026-11-10。基准：当前 `feat/rk3588-apps` 工作树静态审计 + 板上实测结论（ALSA 可用）。
范围：`rk3588_live_tx`（采集/文件源/TX）、`rk3588_playout_rx`（RX/输出/同步）、`dvs_media`（仲裁/TX 后端）、`tests/` 验证脚本。

## A1 文件源无重采样，任意非 48k/立体声片源错位

- 位置：`app/rk3588_live_tx/file/file_source.c: aac_open/aac_decode_packet/audio_thread`。
- 现象：解码按片源自身声道数与采样率输出 PCM，`audio_thread` 按会话帧字节数（`48k/2ch`）硬切后 `publish`。单声道、`44100Hz`、浮点/多声道片源直接声道交错错位或采样率失配。
- 影响：文件源音频端到端仅对 `48k 立体声 S16` 片源成立，其余静默错音。
- 依据：`aac_ctx->channels/sample_rate/sample_fmt` 从未读出，也无 `libswresample` 依赖。

## A2 请求音频失败时静默降级为无声（掩盖故障）

- 位置：
  - `app/rk3588_live_tx/live_tx_app.c: tx_stack_create/file_stack_create`：`audio_tx/ALSA` 失败打印 `video-only` 后继续。
  - `app/rk3588_playout_rx/rx/rx_pipeline.c: rx_pipeline_create`、`rx/rx_compositor.c`：`ALSA open/mtl_rx_audio` 失败关闭音频后继续。
- 现象：明确请求音频（`--audio-source alsa`、`--audio-udp 非零`）时，音频链断裂不阻止启动，视频结论仍显示通过。
- 影响：音频故障被视频成功掩盖，回归时无法发现。
- 判定：请求音频即承诺音频，未请求（`none/0`）走无声属正常分支，不在此列。

## A3 直播与文件源声道数不一致未校验

- 位置：`live_tx_app.c: tx_audio_pcm` 累积器、`file_source.c: audio_thread`。
- 现象：`ALSA` 协商声道数与 `cfg.audio_channels`、文件解码声道数与会话声道数均未比对。`period_bytes` 与 `frame_bytes` 按不同声道数计算时累积器切帧漂移。
- 影响：`--audio_channels 1` 或单声道片源在 RX（固定立体声）侧必然错音。

## A4 音视频同步判决为死代码

- 位置：`app/rk3588_playout_rx/rx/av_sync.c: av_sync_decide[_for_pts]`、`av_sync.h`。
- 现象：全仓零调用，渲染线程（`rx_pipeline.c/render_thread`、`rx_compositor.c`）只用 `playout_target_ns + delay` 播视频，`note_audio_frame` 仅记录首包锚点做观测。
- 影响：阈值（`+40ms/-20ms`）、`±1s` 安全阀文字与实际行为脱节，阅读者误以为音频主钟在驱动视频。

## A5 格式全硬编码，无配置校验

- 位置：`mtl_tx_audio.c`、`mtl_rx_audio.c`、`alsa_audio.c`、`tx_backend_live_ingest.c`。
- 现象：`PCM16/48k/2ch/1ms/payload 98` 四处写死，`RX` 的 `frame_bytes` 用 `MP4_ST30_FRAME_TIME_NS` 常量，`TX dvs` 用 `10ms` 帧而 `live_tx` 用 `1ms` 帧，差异无文档。
- 影响：扩声道/位深时四处改漏；`TX/RX` 帧长差异本身合法但无人说明。

## A6 双音频 TX 实现并存，校验口径不一

- 位置：`live_tx/mtl_tx_audio.c（st30 1ms frame-level）` vs `dvs_media/engine/tx_backend_live_ingest.c（st30p 10ms pipeline）`。
- 现象：前者降级继续，后者整路失败；后者 `set_rate_near` 后不回读实际协商值，直接开采。
- 影响：同一声卡在 `live_ingest` 与 `dvs_media` 下行为不一致，非 `48k` 设备一边静默一边报错。

## A7 无音频端到端验证

- 位置：`app/rk3588_playout_rx/tests/` 全脚本、`app/rk3588_live_tx/tests/test_io.c`、`app/dvs_media/tests/`。
- 现象：
  - `run_tx_rx_e2e.sh/run_single_board_plan_a.sh/run_dualboard_interop.sh/run_dualoutput_sameframe.sh` 纯视频口径，不传 `--audio-udp`。
  - `run_multinode_composite.sh T1` 传 `--audio-udp` 但判定仅 `INFO` 打印音频是否建连，不断言帧数/`drops`。
  - `check_playout_log.py/check_drm_log.py/verify_phase.sh` 只解析视频汇总行。
  - `test_io alsa_test` 需手传设备且多 `SKIP`，`test_audio_owner/tx_session` 只测字符串与端口范围。
- 影响：音频断流、`drops>0`、`ALSA xrun` 在任何脚本中都不失败。

## A8 ALSA 严格 48k 但无统一说明

- 位置：`io/alsa_capturer.c: alsa_setup_hw` 拒绝非 `48k`，`tx_backend_live_ingest.c` 未拒绝。
- 现象：一处 `ENOTSUP`，一处放行，调用方处理又各不相同（见 A2/A6）。
- 影响：`44100Hz` 声卡在不同链路下表现随机。
