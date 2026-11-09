# RK3588 V4L2 HDMI RX NV12 格式协商分析

## 背景

`rk3588_live_tx` 的 V4L2 采集链路使用 `/dev/video0`（RK HDMI RX，`rk_hdmirx`）。
需要确认该 V4L2 设备是否支持协商 `NV12`（`V4L2_PIX_FMT_NV12`）格式，以便在
采集侧直接输出 NV12，减少后续转换开销。

## 测试环境

- 开发板：RK3588，内核 `5.10.226-g40733b7b2aa6-dirty`，aarch64
- 连接方式：`ssh board`（`192.168.80.202`，用户 `root`）
- V4L2 设备：`/dev/video0`
  - 驱动：`rk_hdmirx`
  - 类型：`Video Capture Multiplanar`
  - 当前输入时序：`1920x1080p60`（`148520000 Hz`）
  - 当前像素格式：`BGR3`

## 枚举结果

`VIDIOC_ENUM_FMT` 显示设备支持以下输出格式：

- `BGR3`：24-bit BGR 8-8-8
- `NV24`：Y/CbCr 4:4:4
- `NV16`：Y/CbCr 4:2:2
- `NV12`：Y/CbCr 4:2:0

因此，从枚举角度看，`NV12` 是支持的格式之一。

## 协商测试

对 `/dev/video0` 执行 `VIDIOC_TRY_FMT` / `VIDIOC_S_FMT`，请求：

- `pixelformat = NV12`
- `width = 1920`
- `height = 1080`

结果：两者均返回 `EINVAL`。

内核日志：

```text
hdmirx_s_fmt_vid_cap_mplane: err, set_fmt:0x3231564e, cur_fmt:0x33524742!
```

其中：

- `0x3231564e` = `NV12`
- `0x33524742` = `BGR3`

## 驱动逻辑分析

RK HDMI RX 驱动的 `cur_fmt_fourcc` 并非由用户通过 `S_FMT` 选择，而是由 HDMI
输入信号的颜色空间决定：

- `HDMIRX_RGB888` → `V4L2_PIX_FMT_BGR24`
- `HDMIRX_YUV422` → `V4L2_PIX_FMT_NV16`
- `HDMIRX_YUV444` → `V4L2_PIX_FMT_NV24`
- `HDMIRX_YUV420` → `V4L2_PIX_FMT_NV12`

`hdmirx_s_fmt_vid_cap_mplane()` 中的校验逻辑要求请求格式必须等于当前
`cur_fmt_fourcc`，否则直接返回 `-EINVAL`。因此：

- 当 HDMI 源输出 RGB 时，当前格式为 `BGR3`，请求 `NV12` 协商失败；
- 当 HDMI 源输出 YUV420 时，当前格式为 `NV12`，`NV12` 协商才能成功。

## 结论

1. 开发板 V4L2 的 `/dev/video0` **枚举支持 NV12**。
2. 但 **NV12 的协商不是无条件的**：仅在 HDMI 输入信号为 YUV420 时，驱动才会
   将当前格式设为 `NV12`，此时 `VIDIOC_S_FMT` 才能成功。
3. 当前开发板所接 HDMI 源为 RGB，因此 `NV12` 协商失败，返回 `EINVAL`。
4. 若要验证 NV12 协商成功，需要将 HDMI 源输出改为 YUV420 色彩空间。

## 对采集链路的影响

在当前 RGB 输入条件下，V4L2 采集侧不能直接协商为 NV12，仍需要后续转换步骤
（例如 RGA）将采集帧转换为 NV12 后供 MPP 编码器使用。若未来输入源可稳定
输出 YUV420，则可考虑在采集侧直接使用 NV12，减少转换开销。
