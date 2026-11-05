# rk3588_common — RK3588 各应用共享基础库

`rk3588_live_tx`、`rk3588_playout_rx`、`dvs_media` 共用的静态源码池，只放跨应用复用、行为稳定、无应用策略的基础组件（additive-only，不做兼容垫片）。是否硬件相关不作为判断条件——消费者本来就全是 RK3588 硬件应用；禁区是应用策略（线程模型、布局/平面/准入/会话编排），这类代码留在各自 donor。可选依赖 MTL：找到时以 `-DHAVE_MTL=1` 编译，找不到也能单独构建。消费者在各自 meson 工程内直接编译 `src/*.c` 引入（`rk3588_common_dep` 别名保留备用）。

## 结构

```
include/rk3588_common/
  pipeline_common.h   # 跨应用数据结构：dmabuf_frame、codec_packet、
                      # file_playout_rx_pkt、decoded_frame、file_playout_mode
  codec_plugin.h      # 编解码插件接口（MPP 封装实现该接口）
  ring_buffer.h       # 无锁环形缓冲
  slab_pool.h         # slab 内存池（预分配，避免数据面 malloc）
  timer.h             # 单调定时器
  sps_vui.h           # SPS/VUI 解析（第一批由 live_ingest 上移）
  sei_timestamp.h     # SEI 时间戳打包/解析（第一批由 live_ingest 上移）
  display_layout.h    # 布局矩形数学与 pip_stream 结构（第一批由 file_playout 上移）
  drm_mode.h          # 显示模式字符串/分辨率数学（第一批由 file_playout 上移）
  ptp_time.h          # PTP 时间源封装（第一批由 file_playout 上移）
  au_trace.h          # E2E AU 追踪（TX/RX 共用，阶段2由 file_playout/shared 上移）
  extradata_handler.h # SPS/PPS 拼接（TX 拼接与 RX 解码辅助共用，阶段2上移）
  tx_tai_ts.h         # TX 用户时间戳/TAI 锚点（编码侧发送机制，阶段2上移）
src/
  ring_buffer.c  slab_pool.c  timer.c  sps_vui.c  sei_timestamp.c
  display_layout.c  drm_mode.c  ptp_time.c
  au_trace.c  extradata_handler.c  tx_tai_ts.c
```

设计约束与上游 MTL 一致：这些组件服务数据面，避免堆分配与阻塞调用；新组件加入前先确认两个应用确实都需要，避免单应用特有逻辑上提。
