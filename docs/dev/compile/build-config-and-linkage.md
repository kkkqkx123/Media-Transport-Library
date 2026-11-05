# MTL 包编译配置与模块链接关系分析

本文整理仓库中各独立构建包（meson / cargo / setuptools / Makefile）的编译配置、产物与模块链接关系。分析基于仓库当前源码，不修改构建逻辑。

## 1. 总览

- 主构建系统为 **Meson + Ninja**，顶层 `build.sh` 串行编排多个独立 meson 项目。
- 核心产物是 **`libmtl`**（`libmtl.so` / Windows `libmtl.dll`），所有媒体、应用、插件、测试包最终都围绕它链接。
- 顶层 `meson.build` 只构建库本体、头文件、unit/fuzz 子目录；`app/`、`tests/`、`plugins/`、`manager/`、`ld_preload/`、`tests/tools/RxTxApp/` 都是独立 meson 项目，通过已安装的 `mtl.pc` 发现 libmtl。
- RK3588 应用族（`rk3588_live_ingest`、`rk3588_file_playout`、`dvs_media`、`dvs_agent`、`dvs_platform`、`rk3588_common`、`dvs_common`）也是独立 meson 项目，不纳入顶层 `build.sh`，需在各自目录单独 `meson setup build && ninja`。
- Rust 绑定、Python SWIG 绑定、eBPF/抓包/调度仿真工具使用 Cargo / setuptools / Makefile，不参与 Meson 主流程。

## 2. 构建入口与编排

### 2.1 顶层 Meson（库）

- 项目名：`mtl`，语言 C，版本来自 `VERSION`。
- 默认 `buildtype=release`。
- `meson_options.txt` 控制 ASan、USDT、fuzzing、unit tests、RX 预取/非临时存储、Windows TAP、DPDK 根目录。
- 构建库、安装头文件、生成 `mtl.pc`，并安装 `nicctl.sh` / `common.sh` / `versions.env`。

### 2.2 build.sh 编排顺序

1. `build/`：库 `libmtl`，`ninja` 后 `do_install`。
2. `build/app/`：应用包。
3. `build/tests/`：集成测试包。
4. `build/plugins/`：插件包。
5. `build/ld_preload/`：预加载包（仅非 Windows）。
6. `build/manager/`：MTL 管理器（仅非 Windows）。
7. `tests/tools/RxTxApp/build/`：RxTxApp。

额外模式：

- `./build.sh unit`：单独配置 `build_unit/`，启用 `-Denable_unit_tests=true`，构建并运行 `UnitTest` 后退出。
- `enable_fuzzing` 参数或 `MTL_BUILD_ENABLE_FUZZING=true`：在库构建中启用 fuzz 子目录。
- `MTL_INSTALL_PREFIX`：启用自包含安装树、`$ORIGIN` RPATH、pkg-config 重定位。

## 3. 包/项目清单

| 包 | Meson 项目名 | 语言 | 构建目录（build.sh） | 主要产物 |
|---|---|---|---|---|
| 核心库 | `mtl` | C | `build/` | `libmtl.so` |
| 应用 | `mtl_app` | C | `build/app/` | 大量 sample/benchmark 可执行文件 |
| 集成测试 | `mtl_test` | C++ | `build/tests/` | `KahawaiTest` |
| 插件 | `mtl_plugins` | C | `build/plugins/` | `libst_plugin_st22_sample.so`、`libst_plugin_convert_sample.so` |
| 预加载 | `mtl_preload` | C | `build/ld_preload/` | 当前 meson 中未声明最终共享库目标 |
| 管理器 | `mtl_manager` | C++17 | `build/manager/` | `MtlManager`、XDP 对象 |
| RxTxApp | `mtl_app` | C | `tests/tools/RxTxApp/build/` | `RxTxApp` |
| GStreamer 工具 | `gstreamer_mtl_tools` | C | 独立 | `libancgenerator`、`libtimeinserter` |
| GPU Direct | `mtl_gpu_direct` | C | 独立 | `libmtl_gpu_direct.so` |
| RK3588 common | `rk3588_common` | C | 独立 | `librk3588_common.a` |
| live_ingest | `rk3588_live_ingest` | C | 独立 | `live_ingest` 及测试 |
| file_playout | `file_playout` | C | 独立 | `file_playout_tx`、`file_playout_rx` |
| dvs_media | `dvs_media` | C | 独立 | `dvs_media` |
| dvs_agent | `dvs_agent` | C++17/C | 独立 | `dvs_agent` |
| dvs_platform | `dvs_platform` | C++17/C | 独立 | `dvs_platform` |
| dvs_common | `dvs_common` | C++17/C | 独立 | `libdvs_common.a`（语法检查目标） |
| Rust 绑定 | `imtl-rs` / `imtl-sys` | Rust | Cargo | Rust crate |
| Python 绑定 | `pymtl` | Python/C | setuptools | `_pymtl` 扩展 |

## 4. 核心库 libmtl

### 4.1 构建配置

- 库类型：`shared_library('mtl', ...)`。
- 编译参数：
  - 非 debug 构建启用 `-Werror`；debug 构建定义 `MTL_SIMULATE_PACKET_DROPS`。
  - 常用：`-Wall`、`-Wunused-parameter`、`-msse4.2`，可选 AVX2/AVX512 宏。
  - ASan 可选。
  - 安全链接参数：`-Wl,-z,now`、`-Wl,-z,relro`（非 Windows）。
- 宏控制：
  - `ALLOW_EXPERIMENTAL_API`
  - `MTL_HAS_XDP_BACKEND`（libxdp + libbpf 同时存在时）
  - `MTL_GPU_DIRECT_ENABLED`（发现 `mtl_gpu_direct` 时）
  - `MTL_HAS_USDT`（systemtap SDT + dtrace）
  - `MTL_HAS_RX_VIDEO_PKT_PREFETCH`
  - `MTL_HAS_RX_VIDEO_NT_STORE`
  - Windows：`WINDOWSENV`、`MTL_HAS_TAP`

### 4.2 依赖

必需：

- `libm`
- `pthread`
- `dl`
- `libdpdk >= 25.03`
- `libnuma`（非 Windows）
- `json-c`

可选：

- `mtl_gpu_direct`
- `libxdp`
- `libbpf`
- USDT：`sys/sdt.h` + `dtrace`

Windows：

- `ws2_32`
- DPDK 静态依赖
- `mt_tap.c`（启用 TAP 时）

### 4.3 源码模块

`lib/src/meson.build` 聚合以下源码：

- 核心：`mt_main.c`、`mt_sch.c`、`mt_cni.c`、`mt_ptp.c`、`mt_arp.c`、`mt_dhcp.c`、`mt_mcast.c`、`mt_util.c`、`mt_dma.c`、`mt_admin.c`、`mt_config.c`、`mt_socket.c`、`mt_stat.c`、`mt_rtcp.c`、`mt_flow.c`、`mt_instance.c`、`mt_log.c`、`mt_pcap.c`，非 Windows 还有 `mt_dma_auto.c`。
- 设备层：`dev/mt_dev.c`，XDP 后端存在时追加 `dev/mt_af_xdp.c`。
- 数据路径：`datapath/mt_queue.c`、`mt_shared_queue.c`、`mt_shared_rss.c`、`mt_dp_socket.c`。
- ST2110：TX/RX video/audio/ancillary session、transmitter、timing parser、convert、fmt、AVX 实现。
- Pipeline：`st20/22/30/40` TX/RX pipeline、plugin。
- 实验：`st20_redundant_combined_rx.c`。
- USDT：custom target 生成 `mt_usdt_provider.h/.o`。

### 4.4 对外接口与安装

- 公共头文件安装到 `include/mtl/`，包括 `mtl_api.h`、`st_api.h`、`st20_api.h`、`st30_api.h`、`st40_api.h`、`st_pipeline_api.h`、`st_convert_api.h`、`mtl_sch_api.h` 等。
- 实验头文件安装到 `include/mtl/experimental/`。
- `mtl_build_config.h` 由 configure_file 生成并安装。
- `include/meson.build` 仅安装白名单头文件；内部头文件不安装。

## 5. 应用与工具包

### 5.1 app 包

- 独立 meson 项目 `mtl_app`，C 语言。
- 依赖：`mtl`（必需）、`m`、`pthread`、`json-c`、`pcap`；可选 `sdl2`、`SDL2_ttf`、`openssl`、`mtl_gpu_direct`。
- 公共编译参数：`-Wall`、`-Wunused-parameter`、非 debug `-Werror`、`-msse4.2`、include 路径指向仓库 include 与 build include。
- 产物：`ConvApp`、`LcoreMgr`、大量 `Perf*`、pipeline sample、fwd sample、misc/ext-frame/low-level video sample、DMA sample、`V4l2toIPApp`（条件）、GPU Direct sample（条件）。
- `app/meson.build` 只包含 `tools`、`sample`、`perf`、`v4l2_to_ip` 四个子目录；RK3588 应用不在此构建。

### 5.2 RxTxApp

- 独立 meson 项目 `mtl_app`，是验收/E2E 工具。
- 依赖与 app 包类似，额外 `libjson_c`、`libpcap`、可选 SDL2/TTF/OpenSSL。
- 源码聚合 `src/`、`src/legacy/`、`src/experimental/`。
- 产物：`RxTxApp`。
- Debug 构建定义 `MTL_SIMULATE_PACKET_DROPS`。

### 5.3 GStreamer 工具

- 项目 `gstreamer_mtl_tools`，LGPL。
- 依赖：`gstreamer-1.0 >= 1.19`、`gstreamer-base-1.0`、`mtl`。
- 产物：`libancgenerator`、`libtimeinserter`，安装到 `libdir/gstreamer-1.0`。
- 使用 `$ORIGIN` RPATH，可选附加 `rpath` 选项。

## 6. 测试包

### 6.1 集成测试 KahawaiTest

- 项目 `mtl_test`，C++。
- 依赖：`mtl`、`gtest`、`numa`、`openssl`、`pthread`，Windows 下 `ws2_32`。
- 聚合 `tests/integration_tests/` 下主测试与 `st20/`、`noctx/` 子目录。
- 产物：`KahawaiTest`（非 Windows）。
- 需真实 VF、root 权限；`NoCtxTest.*` 需独立进程。

### 6.2 Unit 测试

- 通过顶层 `-Denable_unit_tests=true` 启用，构建目录 `build_unit/`。
- 项目仍为顶层 `mtl`，但 `tests/unit/meson.build` 聚合大量 harness/test。
- 关键链接方式：使用 `mtl_internal_dep`（`link_with: mtl_lib`）直接链接库对象，同时包含 DPDK、m、numa、gtest/gmock、pthread、USDT provider。
- 单元测试源码直接 `#include` 生产 `.c`，因此需要 `lib/src` include 路径。
- `UnitTest` 不安装。

### 6.3 Fuzz

- 通过 `-Denable_fuzzing=true` 启用。
- 每个 fuzz target 是独立 executable，聚合 `mtl_internal_dep`、DPDK、m、stdc++、可选 GPU Direct。
- 入口为 `LLVMFuzzerTestOneInput`，链接 `-fsanitize=fuzzer`。

## 7. 插件包

### 7.1 plugins 包

- 项目 `mtl_plugins`，C。
- 依赖：`mtl`、`pthread`；Windows 下 `ws2_32`。
- 可选 ASan。
- 产物：
  - `libst_plugin_st22_sample.so`
  - `libst_plugin_convert_sample.so`
- 这些插件通过 MTL plugin 机制被会话加载。

### 7.2 st22 avcodec 插件

- 独立 meson 项目，位于 `plugins/st22_avcodec/`。
- 依赖：`mtl`、`pthread`、`libavcodec`、`libavutil`。
- 产物：`libst_plugin_st22_avcodec.so`。
- 安装后由 `kahawai.json` 注册（build.sh 在 prefix 安装时注册）。

## 8. 管理器与预加载

### 8.1 MtlManager

- 项目 `mtl_manager`，C++17。
- 依赖：可选 `libxdp`、`libbpf`；ASan 可选。
- 若 XDP 依赖存在：
  - 编译 `mtl.xdp.c` 为 BPF 对象 `mtl.xdp.o`，安装到 `prefix/lib/bpf`。
  - 定义 `MTL_HAS_XDP_BACKEND`。
- 产物：`MtlManager`。
- 与 libmtl 无直接链接依赖（不链接 mtl），但运行时协同管理 lcore/队列。

### 8.2 ld_preload

- 项目 `mtl_preload`，C。
- 声明依赖：`mtl`、`dl`。
- 当前 `ld_preload/meson.build` 只完成依赖发现，未声明 `shared_library`/`executable` 目标；目录内仅有 `preload_platform.h`。这是当前仓库状态，构建时 Ninja 不会产生 ld_preload 产物。

## 9. GPU Direct

- 项目 `mtl_gpu_direct`，C。
- 依赖：`libze_loader`（必需）。
- 产物：`libmtl_gpu_direct.so`，安装头文件 `gpu.h`，生成 `mtl_gpu_direct.pc`。
- libmtl 和 fuzz/unit 在发现该 `.pc` 时自动启用 `MTL_GPU_DIRECT_ENABLED` 并链接该库。

## 10. RK3588 应用族

### 10.1 rk3588_common

- 项目 `rk3588_common`，C。
- 静态库：`librk3588_common.a`，源码 `ring_buffer.c`、`slab_pool.c`、`timer.c`。
- 依赖：`threads`；可选 `mtl`（发现时定义 `HAVE_MTL=1`）。
- 为 live_ingest/file_playout/dvs 提供公共环形缓冲、slab pool、定时器。

### 10.2 live_ingest

- 项目 `rk3588_live_ingest`，C。
- 必需：`libdrm`、`threads`。
- 可选：`rockchip_mpp`、`librga`、`mtl`、`json-c`。
- 构建分阶段：
  - HAL 静态库 + 纯 C 测试
  - IO 静态库
  - Codec 静态库（MPP+RGA 存在时）
  - Pipeline 静态库（MPP+RGA 存在时，可选 MTL）
  - `live_ingest` 可执行文件（MPP+RGA+MTL+json-c 全部存在时）
- 依赖链：`live_ingest` 链接 HAL/Codec/IO/Pipeline 静态库，并依赖 DRM/RGA/MPP/pthread/MTL/json-c。

### 10.3 file_playout

- 项目 `file_playout`，C。
- 选项：`build_tx`、`build_rx`（至少一个）。
- 必需：`mtl`、`threads`；TX/RX 共用 FFmpeg（avformat/avcodec/avutil）与 json-c。
- RX 额外必需：Rockchip MPP（链接测试）、`libdrm`、`alsa`。
- 静态库分层：
  - `file_playout_shared`：common + ring/slab/timer + DRM mode/display layout/PTP/E2E 统计
  - `file_playout_probe`：MP4 demux
  - `file_playout_tx_lib`：TX 侧 MTL/调度/播放列表
  - `file_playout_rx_lib`：RX 侧 ALSA/playout/AV sync/MTL RX/MPP/DRM/compositor
- 产物：`file_playout_tx`、`file_playout_rx`、若干 test executable。
- TX-only 模式可在 x86/generic host 构建。

### 10.4 dvs_media

- 项目 `dvs_media`，C，RK3588 RX 侧媒体引擎。
- 必需：MTL、threads、MPP、DRM、ALSA、json-c、FFmpeg。
- 不复制 file_playout 源码，而是 **in-tree 相对路径直接编译**：
  - `rk3588_common/src/*`
  - `rk3588_file_playout/shared/*`
  - `rk3588_file_playout/tx/mp4_demux.c`
  - `rk3588_file_playout/rx/*`
  - `rk3588_file_playout/shared/mpp_decoder.c`、`drm_renderer.c`
- 静态库：`dvs_shared`、`dvs_rx`。
- 可执行文件：`dvs_media`，链接 `dvs_shared`、`dvs_rx`、`file_playout_probe`。
- 测试：`test_state_machine`、`test_ipc`、`test_session_admit`、`test_tx_session`。
- TX/input 角色当前为占位，尚未接入 live_ingest donor chain。

## 11. DVS 节点侧包

### 11.1 dvs_common

- 项目 `dvs_common`，C++17/C。
- 提供 `http_server.cpp`、`http_client.cpp`、`sha256.cpp`、SQLite amalgamation。
- 依赖：`threads`、`dl`、可选 `json-c`。
- 产物 `libdvs_common.a` 仅作为语法检查目标；消费者直接编译相同源码，不链接该库。

### 11.2 dvs_agent

- 项目 `dvs_agent`，C++17/C。
- 依赖：`json-c`、`threads`、`dl`。
- 直接编译 `dvs_common` 源码。
- 产物：`dvs_agent`、`dvs_agent_test`。
- 无 MTL 依赖。

### 11.3 dvs_platform

- 项目 `dvs_platform`，C++17/C。
- 依赖：`json-c`、`threads`、`dl`。
- 复用 `dvs_agent` 与 `dvs_common` 源码（HTTP 服务/客户端、SQLite），不预抽公共库。
- 产物：`dvs_platform`、`dvs_platform_test`。
- 无 MTL 依赖。

## 12. Rust / Python / Makefile 工具

### 12.1 Rust

- `rust/imtl-sys`：build-dependency 使用 `bindgen` + `pkg-config`，从已安装 `mtl.pc` 生成 FFI。
- `rust/imtl-rs`：依赖 `imtl-sys`，提供 Rust 封装。
- 需先安装 libmtl 并确保 pkg-config 可找到 `mtl.pc`。

### 12.2 Python

- `python/swig/setup.py` 使用 setuptools，扩展 `_pymtl` 链接 `mtl`。
- 需要 SWIG 生成 `pymtl_wrap.c`；include/library 路径在 setup.py 中当前为注释占位。

### 12.3 Makefile 工具

- `tools/ebpf/Makefile`：eBPF 监控工具。
- `tools/readpcap/Makefile`：pcap 读取工具。
- `tools/sch_smi_emulate/Makefile`：调度 SMI 仿真工具。
- `tools/set_tai_offset/meson.build`：独立 meson 项目，产物 `set_tai_offset`，不链接 MTL。

## 13. 生态插件

### 13.1 FFmpeg 插件

- `ecosystem/ffmpeg_plugin/tests/` 提供 MTL/FFmpeg stub 的单元测试，项目 `mtl_ffmpeg_plugin_unit_tests`。
- 真实插件构建脚本为 `script/build_ffmpeg_plugin.sh`（不在 Meson 主流程）。

### 13.2 GStreamer 插件

- 项目 `gstreamer_mtl_plugins`，LGPL。
- 依赖：GStreamer 1.0/base/video/audio、`mtl`。
- 先构建 `gstmtl_common` 共享库，再构建 TX/RX st20p、RX/TX st30p、TX/RX st40p 插件。
- 所有插件安装到 `libdir/gstreamer-1.0`，使用 `$ORIGIN` RPATH。

### 13.3 OBS 插件

- 项目 `linux-mtl`，C。
- 依赖：`libobs`、`mtl`、`pthread`。
- 产物：`linux-mtl.so`，安装到 OBS 插件目录。

## 14. 依赖与链接关系总图

```text
libdpdk / libnuma / json-c / pthread / m
  -> libmtl.so
       -> app (ConvApp, samples, perf, V4l2toIPApp, ...)
       -> tests/KahawaiTest
       -> tests/UnitTest (via mtl_internal_dep + production .c)
       -> plugins/* (st_plugin_*)
       -> tests/tools/RxTxApp/RxTxApp
       -> tests/tools/gstreamer_tools (ancgenerator/timeinserter)
       -> ecosystem gstreamer/obs plugins
       -> gpu_direct? (libmtl optional depends on it)
       -> RK3588: live_ingest/file_playout/dvs_media (optional or required)
       -> Rust imtl-sys (pkg-config)
       -> Python _pymtl (link mtl)

libmtl_gpu_direct.so (libze_loader)
  -> libmtl (optional)
  -> unit/fuzz (optional)

FFmpeg (avformat/avcodec/avutil)
  -> file_playout (probe/tx/rx)
  -> dvs_media
  -> st22_avcodec plugin
  -> ffmpeg plugin tests stub

Rockchip MPP / librga / libdrm / ALSA
  -> live_ingest (phase codec/pipe/app)
  -> file_playout RX
  -> dvs_media

threads / dl / json-c
  -> dvs_common / dvs_agent / dvs_platform

libobs
  -> ecosystem/obs_mtl

GStreamer
  -> ecosystem/gstreamer_plugin
  -> tests/tools/gstreamer_tools
```

## 15. 安装、RPATH 与 pkg-config

- 默认安装前缀：`/usr/local`；root 用户安装后执行 `ldconfig`。
- `MTL_INSTALL_PREFIX` 时：
  - 库、应用、插件记录 `$ORIGIN` 相对 RPATH，安装树可整体移动。
  - `relocate_pkgconfig` 将 `.pc` 的 `prefix` 改为相对 `pcfiledir`。
  - build.sh 计算 `LIB_LINK_ARGS` / `BIN_LINK_ARGS` / `CPP_LINK_ARGS`。
- `mtl.pc` 是下游项目发现 libmtl 的主要方式；RK Rust 绑定和 Python 绑定也依赖它。
- Unit 测试额外设置 `unit_build_rpath = dpdk_dep libdir`，并使用 `--disable-new-dtags`，避免 DPDK EAL dlopen 二次加载总线库。
- GStreamer 插件使用 `$ORIGIN` RPATH，保证无 `LD_LIBRARY_PATH` 可加载。

## 16. 构建选项矩阵

| 选项来源 | 选项 | 作用 |
|---|---|---|
| 根 meson | `enable_asan` | 地址消毒 |
| 根 meson | `enable_usdt` | USDT 支持 |
| 根 meson | `enable_fuzzing` | fuzz 子目录 |
| 根 meson | `enable_unit_tests` | unit 子目录 |
| 根 meson | `enable_rx_video_pkt_prefetch` | RX 视频包预取 |
| 根 meson | `enable_rx_video_nt_store` | RX 非临时存储 |
| 根 meson | `enable_tap` | Windows TAP |
| 根 meson | `dpdk_root_dir` | Windows DPDK 路径 |
| app/RxTxApp/tests/plugins/manager | `enable_asan` | ASan |
| file_playout | `build_tx` / `build_rx` | TX/RX 角色 |
| gpu_direct | `enable_tests` | GPU Direct 测试 |
| gstreamer 生态 | `rpath` | 附加 RPATH |
| gstreamer_tools | `rpath` | 附加 RPATH |

## 17. 当前状态与注意事项

- `ld_preload/meson.build` 当前只声明依赖，没有最终目标；不要假设 `build/ld_preload` 会产出预加载库。
- `dvs_common` 的静态库只是语法检查目标；`dvs_agent` / `dvs_platform` 直接编译相同源码，修改 common 源码会影响两者。
- `dvs_media` 直接 in-tree 编译 `rk3588_file_playout` 的 RX/shared/MP4 demux 源码，修改这些文件会影响 file_playout 与 dvs_media 两套构建。
- RK3588 应用不被顶层 `build.sh` 覆盖，需单独构建；部分目标按依赖存在与否条件跳过。
- Rust/Python 绑定需要先安装 libmtl 并配置 pkg-config/SWIG 路径。
