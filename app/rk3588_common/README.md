# rk3588_common — RK3588 各应用共享基础库

`rk3588_live_ingest`、`rk3588_file_playout`、`dvs_media` 共用的纯 C 静态库，只放跨应用复用的无状态基础组件（additive-only）。可选依赖 MTL：找到时以 `-DHAVE_MTL=1` 编译，找不到也能单独构建。消费者通过 `rk3588_common_dep`（declare_dependency 别名）或在各自 meson 工程内直接编译 `src/*.c` 引入。

## 结构

```
include/rk3588_common/
  pipeline_common.h   # 跨应用数据结构：dmabuf_frame、codec_packet、
                      # file_playout_rx_pkt、decoded_frame、file_playout_mode
  codec_plugin.h      # 编解码插件接口（MPP 封装实现该接口）
  ring_buffer.h       # 无锁环形缓冲
  slab_pool.h         # slab 内存池（预分配，避免数据面 malloc）
  timer.h             # 单调定时器
src/
  ring_buffer.c  slab_pool.c  timer.c
```

设计约束与上游 MTL 一致：这些组件服务数据面，避免堆分配与阻塞调用；新组件加入前先确认两个应用确实都需要，避免单应用特有逻辑上提。
