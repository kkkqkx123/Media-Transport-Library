# MP4 Distribution — 实测验证方案

> 测试文件: `app/InTheGrey.mp4` (H.264 High@L4, 1920×1080, 23.976fps, AAC 48kHz stereo, ~97min, 2.6GB)

## 验证策略: 从无依赖到全链路，逐步推进

每个 Phase 验证一个独立组件，通过后进入下一 Phase。Phase 6 是全链路验证目标。

---

## Phase 1: MP4 Demux 基础验证 🔴 无 MTL/DNIC/DRM 依赖

**目标**: 验证 FFmpeg demux 正确打开 MP4 并读取 Annex B 码流

**测试内容**:
1. 打开 InTheGrey.mp4，打印流信息 (分辨率/帧率/编码格式/时长)
2. 读取前 30 帧视频 packet，验证: 非空、Annex B start code (0x00000001)、NAL unit type
3. 读取前 10 帧音频 packet，验证: 非空、AAC 编码
4. 读取最后一个视频 packet，验证 seek 和 duration 正确
5. 统计 IDR 帧数量，计算 GOP 长度

**成功标准**:
- 视频流: 1920×1080, 23.976fps, H.264 High Profile
- 音频流: AAC LC, 48kHz, stereo
- 读出的 AVPacket 非空, data 有 Annex B start code
- IDR 帧间隔统计合理

---

## Phase 2: Extradata Handler 验证 🔴 无 MTL/DNIC 依赖

**目标**: 验证 extradata 提取和 IDR 拼接逻辑

**测试内容**:
1. 从 MP4 提取 extradata (SPS+PPS), 验证 size > 0
2. 验证 extradata 内容包含 SPS NAL (type=7) 和 PPS NAL (type=8)
3. 验证 IDR 检测: 确认 IDR 帧被正确识别 (NAL type=5)
4. 验证 splice 结果: extradata + IDR data = 正确拼接
5. 用 MPP decoder 验证 IDR+splice 数据可解码 (如果 MPP 可用)

**成功标准**:
- extradata_size > 0, 包含 SPS(7) 和 PPS(8)
- 首帧为 IDR (type=5)，被正确识别
- 拼接后数据大小 = extradata_size + frame_size

---

## Phase 3: TX Scheduler Dry-Run 🔴 无 MTL/DNIC 依赖

**目标**: 验证 PTS→TAI 时间戳映射、AAC→PCM 解码、帧节奏

**测试内容**:
1. 创建 mock MTL TX (输出到 /dev/null 或内存 buffer)
2. 运行 scheduler 处理前 300 帧 (~10 秒)
3. 验证: PTS→TAI 映射单调递增
4. 验证: AAC→PCM 解码输出非空 (PCM S16_LE 格式)
5. 验证: 视频帧和音频帧 PTS 在同一时间轴上
6. 测量处理速率: 帧/秒 vs 实时速率

**成功标准**:
- PTS→TAI 映射正确 (单调递增, 间隔 ~41.7ms @ 23.976fps)
- AAC 解码成功, PCM 数据非空
- 处理速率 ≥ 实时速率 (23.976fps)

---

## Phase 4: ALSA Audio Output 验证 🔴 需要 ALSA 设备

**目标**: 验证 ALSA PCM 播放功能

**测试内容**:
1. 枚举 ALSA 播放设备 (`snd_device_name_hint`)
2. 打开默认设备, 配置 S16_LE/2ch/48kHz
3. 生成 1kHz 正弦波, 播放 1 秒
4. 验证 audio_clock_get() 返回值单调递增

**成功标准**:
- ALSA 设备打开成功
- 正弦波可听见
- audio_clock 单调递增

---

## Phase 5: MPP Decoder + DRM Display 验证 🔴 需要 MPP + DRM

**目标**: 验证 MPP 硬解码和 DRM 显示链路

**测试内容**:
1. 从 MP4 提取第一个 IDR 帧 (含 extradata)
2. MPP decoder 创建 (MPP_VIDEO_CodingAVC, split_parse=1)
3. Feed IDR 帧 + 后续 5 帧, 验证解码输出 dma-buf fd
4. DRM renderer 创建, 连接 HDMI 输出
5. 连续解码+显示 60 帧 (~2.5 秒), 目测画面是否正常

**成功标准**:
- MPP decoder 创建成功
- 解码输出 dma-buf fd 非负
- DRM 显示正常画面
- 无解码错误 (errinfo/discard 标志)

---

## Phase 6: Full Pipeline Loopback 🔴 需要 MTL + DNIC

**目标**: TX→RX 完整链路验证

**前置条件**:
```bash
sudo MtlManager &
# 等待 NIC 就绪
```

**测试内容**:
1. 启动 RX: `./mp4_dist rx 30 --port native_af_xdp:eth0`
2. 启动 TX: `./mp4_dist tx 30 --mp4 InTheGrey.mp4 --port native_af_xdp:enP3p49s0`
3. 或 both 模式: `./mp4_dist both 30 --mp4 InTheGrey.mp4`

**成功标准**:
- TX 发送帧数 = 视频帧率 × 测试时长 (约 720 帧 @ 30s)
- RX 接收帧数 ≥ TX 发送帧数 × 95%
- DRM 显示流畅无卡顿
- 音频播放正常
- 端到端延迟 < 100ms

---

## 实施说明

Phase 1-3 可通过独立测试程序 `test_mp4_dist` 执行 (已写入 `tests/test_mp4_dist.c`)。
Phase 4-5 在有 MPP/DRM/ALSA 硬件时单独测试。
Phase 6 是需要完整 MTL 网络栈的最终目标。

```bash
# 构建测试
meson setup build_tests
ninja -C build_tests

# Phase 1: Demux 验证
./build_tests/test_mp4_dist phase1 ../InTheGrey.mp4

# Phase 2: Extradata 验证
./build_tests/test_mp4_dist phase2 ../InTheGrey.mp4

# Phase 3: Scheduler Dry-Run
./build_tests/test_mp4_dist phase3 ../InTheGrey.mp4 300
```
