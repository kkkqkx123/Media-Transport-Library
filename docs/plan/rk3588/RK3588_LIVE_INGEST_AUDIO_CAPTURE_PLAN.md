# RK3588 live_ingest 实时音频采集补充方案

状态：**阶段 1–5 已实施**（设计→代码→单测）。剩余：RK3588 交叉编译 +
`test_io <v4l2> <alsa_dev>` 回环验收，提交前用 clang-format-14 复跑
`format-coding.sh`（本机只有 clang-format 23，输出可能与 CI 不完全一致）。

实施记录（2026-11）：

- 阶段1 `io/alsa_capturer.{c,h}`：块池 + poll 非阻塞 + XRUN 就地恢复 +
  连续错误升级停机 + `snd_pcm_htimestamp` PTS（回退计时不计错误）+
  `HAVE_ALSA` 编译期开关（无 ALSA 时 stub，create 返回 ENOTSUP）；
- 阶段2 `live_tx_config`：`streams[0].audio {source, device, sample_rate,
  channels, period_frames}` + argv `--audio-source/--audio-device`；
- 阶段3 `live_tx_app`：抽公共 `audio_tx_create`（文件/实时共用）；
  实时路径 `tx_stack` 接入 capturer + 1ms ST30 帧累积（`acc` 缓冲，
  节奏同 file_source audio_thread）；音频创建置于 pipeline 之后，
  保证 main 的 ENODATA 重试循环无需清理音频资源；
- 阶段4 `meson_options.txt`（app 本地）`enable_alsa` + `dependency('alsa')`；
- 阶段5 `tests/test_io.c` 增加 ALSA 自测（argv[2] 传入设备才运行）；
  `test_io` 可执行目标补入 meson。

## 1. 背景与现状结论

`app/rk3588_live_tx`（live_ingest，编码发送端）当前音频能力分布：

| 位置 | 现状 |
|---|---|
| `io/v4l2_capturer.c` | 仅视频采集（BGR3/NV12），无音频 |
| `pipeline/` | 实时链路（V4L2→RGA→MPP→MTL TX）纯视频，无音频挂点 |
| `mtl_tx_audio.{c,h}` | ST30 发送会话封装**已完整**（1ms PCM 帧、framebuffer acquire/publish/abort、TAI 重置），可复用 |
| `file/file_source.c` | 音频仅在文件源路径存在：MP4→AAC 软解→喂 `mtl_tx_audio` |
| `live_tx_config` | 仅有 `audio_udp_port`；无音频源配置 |
| `meson.build` | 无 alsa 依赖 |

**结论：ST30 发送端就绪，实时采集路径缺一个"实时 PCM 源"（ALSA 采集）。**
`file_source.c` 的 `audio_thread`（填满 `mtl_tx_audio_frame_bytes` 才 publish 的节奏）
就是文件路径的 PCM 生产者模板；实时路径需要等价的 ALSA 版本。

## 2. 参考实现（ZLMediaKit HdmiIn）

参考 `~/code/zlmediakit/src/HdmiIn/`（音频采集→编码链）：

- `capture/HdmiInAudioCapture.{h,cpp}` — ALSA 采集核心，采纳的机制：
  - 非阻塞打开 + `poll(100ms)` 等待可读；独立采集线程，Stop 置位后 join，有界退出；
  - PCM 块池（period_bytes × block_count），采集线程认领、消费侧 release 归还，
    回调内禁止耗时操作，防 XRUN；
  - PTS 用 `snd_pcm_htimestamp`（首采样硬件时刻修正），与视频同单调时钟域；
  - XRUN/suspend 就地恢复（`snd_pcm_prepare` + 显式 `snd_pcm_start`），
    连续错误超阈值触发 `on_error` 上层重建；
  - 统计：drop / xrun / recover 计数。
- 明确**不采纳**（本场景不需要）：
  - 编码/封装环节 —— ST30 直传 PCM16，无需 AAC/G711 编码器
    （ZLM 编码层 `HdmiInAudioEncoder` 服务于 muxer，ST30 无此需求）；
  - 静音补帧、物理源率（sysfs ACR）注入 —— HDMI-in I2S 特化，
    直连 mic/line-in 场景无此问题，留作后续扩展点。
- ZLM 桩模式借鉴：编译期无 ALSA 时提供"Open 始终失败"的降级行为，
  本方案对应为 meson option `enable_alsa`（默认 true），关闭时 live 分支仅视频。

## 3. 设计

### 3.1 新增 `io/alsa_capturer.{c,h}`（阶段1，核心）

C 风格与 `v4l2_capturer` 对齐（不透明句柄 + 回调 + 生命周期三段式）：

```c
struct alsa_capturer_params {
    const char* device;        /* "hw:0,0" / "default" / "pulse" */
    int sample_rate;           /* 期望值，实际以 ALSA 协商回读为准 */
    int channels;              /* 期望值，同上 */
    int sample_bit;            /* 仅支持 16（匹配 ST30_FMT_PCM16） */
    int period_frames;         /* 每次读取采样帧数，默认 1024 ≈ 21ms@48k */
    int block_count;           /* PCM 块池数量，默认 8，抖动大可加大 */
};

/* 回调在采集线程调用：data 指向一块完整 period PCM（S16LE 交错），
 * pts_ns 为本块首采样时刻（CLOCK_MONOTONIC，snd_pcm_htimestamp 修正），
 * release 必须且仅能调用一次归还块。 */
typedef void (*alsa_pcm_cb)(void* user_data, const void* data, size_t len,
                            uint64_t pts_ns,
                            void (*release)(void* user_data, void* data));

struct alsa_capturer*
alsa_capturer_create(const struct alsa_capturer_params* p,
                     alsa_pcm_cb cb, void* user_data);
void alsa_capturer_free(struct alsa_capturer* c);
int  alsa_capturer_start(struct alsa_capturer* c);   /* 启动采集线程 */
void alsa_capturer_stop(struct alsa_capturer* c);
int  alsa_capturer_sample_rate(const struct alsa_capturer* c);  /* 协商回读 */
int  alsa_capturer_channels(const struct alsa_capturer* c);
void alsa_capturer_set_error_cb(struct alsa_capturer* c,
                                void (*cb)(void* user_data), void* user_data);
/* 观测：丢块（块池耗尽）/ XRUN 恢复 / 连续错误停机 计数 */
struct alsa_capturer_stats { uint64_t drops, xruns, recovers, errors; };
void alsa_capturer_stats(const struct alsa_capturer* c,
                         struct alsa_capturer_stats* out);
```

内部要点：

- 块池：`posix_memalign` 预分配 period_bytes × block_count，缓存行对齐占用标志
  （atomic CAS 认领 / release 归还）；池耗尽时用 scratch 缓冲排空 ALSA 硬件并计 drop；
- 采集循环：`poll` 非阻塞等可读 → `snd_pcm_readi` 填块 → 回调投递 → release 归还；
- XRUN/STRPYE：`snd_pcm_prepare` + `snd_pcm_start` 就地恢复，计 xrun；
  连续错误 ≥10 触发 error_cb 并停止线程（与 v4l2 的错误→上层重建模式一致）；
- Stop：置位 `running=false` → `poll` 唤醒（100ms 超时兜底）→ join；
- PTS：`snd_pcm_htimestamp` 读首采样单调时刻；读取失败回退"读完成时刻 −
  period 时长"，并计 fallback（对齐 v4l2 的 ts_fallback 思路）。

### 3.2 配置（阶段2）

`live_tx_config` 新增字段（JSON 键在 `streams[0].audio` 下）：

```json
"audio": { "source": "alsa", "device": "hw:0,0",
           "sample_rate": 48000, "channels": 2, "ptime_ms": 1 }
```

- `source`：`none`（默认，仅视频，保持现行为）| `alsa`；
- `sample_rate/channels` 映射 `st30_sampling`（仅 48k）/`channel`；
  `ptime_ms` 映射 `st30_ptime`（1ms，匹配 mtl_tx_audio 的 1ms wire pacing）；
- `use_tai`、`audio_udp_port` 沿用现有顶层键；
- 命令行补 `--audio-source`、`--audio-device`。

### 3.3 装配（阶段3，live_tx_app）

- 音频 TX 通用化：`mtl_tx_audio_create/start/free` 从 `file_stack` 提取为独立的
  `audio_stack`，文件路径与实时路径共用；
- 实时路径（`tx_stack`）：`audio_source=alsa` 时
  创建 capturer → 回调内 `mtl_tx_audio_acquire_fb` → 按需缓冲拼满 frame_bytes →
  `publish_fb`（节奏与 file_source audio_thread 相同：period 21ms vs 帧 1ms，
  需一个小的 PCM 累积缓冲把 ALSA 周期切分成 1ms ST30 帧）；
- ALSA 协商回读与配置不符（如设备仅 44.1k）时报错退出，不做重采样
  （ST30 仅 48k 族，重采样引入额外依赖，超出本阶段范围）；
- error_cb → 打印告警并停音频 TX（视频不中断，与 file 路径"音频不可用则
  video-only"语义一致）。

### 3.4 构建（阶段4）

- `meson.build`：`enable_alsa = get_option('enable_alsa')` +
  `dependency('alsa', required: false)`；开启时把 `io/alsa_capturer.c` 加进
  `live_ingest_io`，`common_c_args` 加 `-DHAVE_ALSA=1`；
- 交叉 sysroot 需安装 `libasound2-dev`（aarch64），见
  `docs/dev/compile/rk3588-apps-build.md`。

### 3.5 测试（阶段5）

`tests/test_io.c` 增加 ALSA 自测（沿用现有"设备自测"风格，默认设备可通过 argv 覆盖，
无设备时打印 SKIP）：

1. create/start → 回调帧计数 + len==period_bytes 校验；
2. pts 单调性检查（允许少量回退由 fallback 路径产生，计数并报告）；
3. stop 后线程退出（join 有界）、stats 计数打印。

x86/无 ALSA 环境：`meson configure -Denable_alsa=false` 可整体裁掉，不阻塞其它测试。

## 4. 实施阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| 1 | `io/alsa_capturer.{c,h}` | RK3588 上 `test_io <alsa_dev>` 出帧、pts 单调 |
| 2 | 配置项（struct + JSON + argv） | JSON/argv 均可解析，`none` 默认不加载 alsa |
| 3 | live_tx_app 装配 | `--source live --audio-source alsa` 与 file_playout_rx 回环收音 |
| 4 | meson alsa 依赖 + option | aarch64 交叉编译通过；`-Denable_alsa=false` 亦通过 |
| 5 | test_io ALSA 自测 | 单测运行无泄漏（bsan/valgrind 视条件） |
| 6 | format-coding.sh + 文档回写 | clang-format-14 通过，README 更新目录表 |

## 5. 风险与后续

- sysroot 缺 libasound：阶段4 前置条件，需在编译服务器同步安装；
- r8168 单队列带宽：ST30 48k/2ch/16bit ≈ 1.5 Mbps，远小于视频，无争用风险；
- 后续扩展：静音补帧、物理源率修正（接入 HDMI-in I2S 时）、重采样兜底。
