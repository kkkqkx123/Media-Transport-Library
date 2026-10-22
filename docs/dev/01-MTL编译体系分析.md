# MTL 编译体系分析（面向 DVS / RK3588 落地）

本文梳理 Media-Transport-Library（下称 MTL）当前仓库“如何编译”，并按 `docs/req` 的需求聚焦到 DVS 真正关心的部分：`docs/req/02-MTL基座分析.md` §6 构建与运行约束、§4.5 编解码插件，以及 `docs/req/05-实施路线与验证.md` P0 基座验证（“在 RK3588 上编译 MTL、DPDK/AF_XDP、MtlManager、示例应用”）与 §13 启动清单第 1 步（“在 RK3588 上编译 MTL，跑通 `./build.sh`”）。

## 0. 一句话结论

MTL 是“一个库工程 + 多个独立子工程”的 meson 结构：根工程只编译 `libmtl`，`app/`、`tests/`、`plugins/`、`manager/`、`ld_preload/`、`tests/tools/RxTxApp/` 各自是独立 meson 工程，全部通过 pkg-config 找已安装的 `mtl.pc` 来链接库；`./build.sh` 只是把这些工程按固定顺序 setup→ninja→install 串起来。对 DVS 最关键的三件事：DPDK 必须先以打补丁方式构建（即使走 AF_XDP/kernel 后端也依赖 DPDK API）；AF_XDP 后端取决于构建时能否找到 libxdp+libbpf；两个 rk3588 参考应用是**独立工程，既不在 build.sh 也不在 CI 里**，需要在板上单独构建。

## 1. 总体构建结构

### 1.1 工具链与工程组织

- 构建工具：meson + ninja，全仓库统一；核心库为 C99（`meson.build` 未设 `c_std`，用编译器默认，测试为 C++，`MtlManager` 为 C++17）。
- 版本号统一来自根目录 `VERSION`（当前 `26.09.0.DEV`），各子工程 project() 都读取它。
- 工程清单：

| 工程 | 路径 | 产物 | 被谁消费 |
| --- | --- | --- | --- |
| mtl（库，根工程） | `meson.build` + `lib/` | `libmtl.so`、头文件、`mtl.pc` | 其余全部工程（pkg-config `mtl`） |
| app 示例 | `app/` | 数十个 sample/perf 可执行文件 | 演示与评估 |
| 集成测试 | `tests/` | `KahawaiTest` | 真实 VF 上的 gtest |
| 单测 | `tests/unit/`（根工程 subdir，非独立工程） | `UnitTest` | 无 NIC、无 root |
| 模糊测试 | `tests/fuzz/`（根工程 subdir） | libFuzzer harness | nightly CI |
| 插件 | `plugins/`（sample）、`plugins/st22_avcodec/`（独立） | `libst_plugin_*.so` | ST2110-22 编解码 |
| 守护进程 | `manager/` | `MtlManager`（+`mtl.xdp.o`） | 多进程资源仲裁、AF_XDP |
| ld_preload | `ld_preload/` | 空壳，无构建目标 | — |
| RxTxApp | `tests/tools/RxTxApp/` | `RxTxApp` | pytest 验收套件 |
| rk3588 参考应用 | `app/rk3588_end2end/`、`app/rk3588_mp4_dist/` | `e2e_app`、`mp4_dist_tx/rx` 等 | DVS 节点媒体引擎起点 |

### 1.2 `build.sh` 流程

`./build.sh [debug|debugonly|debugoptimized|plain|release] [enable_fuzzing] [unit]`：

1. `meson setup build … && ninja && ninja install` —— 库；
2. 若设了 `MTL_INSTALL_PREFIX`，把含 `mtl.pc` 的目录插入 `PKG_CONFIG_PATH`，保证下游找到新装库而不是陈旧系统版本；
3. 依次对 `app/`、`tests/`、`plugins/`、`ld_preload/`（非 Windows）、`manager/`（非 Windows）、`tests/tools/RxTxApp/` 重复 setup→ninja→install；
4. `unit` 模式单独用 `build_unit/` 目录配 `-Denable_unit_tests=true`，构建并直接运行 `UnitTest` 后退出。

构建目录与安装：库在 `build/`，各子工程在 `build/<组件>`，RxTxApp 在 `tests/tools/RxTxApp/build/`。默认前缀 `/usr/local`；非 root 用户执行 `ninja install` 会自动 `sudo`。**用 `MTL_INSTALL_PREFIX=$PWD/.local_install` 可全程免 root**，但注意仓库存在两套安装树且不可互换（`/usr/local` 供 gtest，`.local_install` 供 pytest 验收，见 AGENTS.md）。

环境变量开关（等价于命令行参数）：`MTL_BUILD_ENABLE_ASAN`、`MTL_BUILD_ENABLE_TAP`（Windows）、`MTL_BUILD_DISABLE_USDT`、`MTL_BUILD_ENABLE_FUZZING`、`MTL_INSTALL_PREFIX`；插件脚本另有 `MTL_PLUGIN_PREFIX`。

### 1.3 依赖版本钉在 `versions.env`

| 键 | 值 | 用途 |
| --- | --- | --- |
| DPDK_VER / DPDK_MTL_MINOR_VER | 26.07 / 91 | 必选外部依赖（打补丁构建，见 §2） |
| ICE_VER / IAVF_VER | 2.6.7 / 4.13.35 | Intel 内核驱动补丁（x86 硬件 pacing，RK3588 无关） |
| FFMPEG_VERSION | 7.0 | `st22_avcodec` 插件、`mp4_dist` TX 解复用 |
| XDP_TOOLS_VER / EBPF_VER | 1.6.3 / 1.7.0 | AF_XDP 后端（RK3588 推荐数据面，见 §3.2） |
| SVT_JPEG_XS_VER | 8e50180… | JPEG-XS 编码链（DVS V1.0 用 H.264/HEVC，暂不涉及） |
| OPENH264_VER | 2.6.0 | 编码库（同上） |

系统包清单见 `doc/build.md` §1（Ubuntu/Debian：`git gcc meson ninja-build python3 python3-pyelftools pkg-config libnuma-dev libjson-c-dev libpcap-dev libgtest-dev libgmock-dev libssl-dev systemtap-sdt-dev llvm clang flex byacc`，SDL2 可选）。两个“隐形依赖”值得写进节点镜像构建脚本：`libdpdk.pc` 的 `Requires.private` 含 libelf 与 zlib（缺了会在 pkg-config 阶段莫名失败）；DPDK 的 meson 需要 `python3-pyelftools`。`script/build_ebpf_xdp.sh --check build` 就是为此提供的预检。

## 2. DPDK：先决步骤，补丁是平铺序列

`script/build_dpdk.sh`（由 `script/common.sh` 读 `versions.env`）：

1. 先查 pkg-config：若已安装 `libdpdk` 且版本串匹配 `26.07.91_mtl_*`（26.03 起补丁会把 MTL 版本写进 DPDK 版本串），则跳过重建；本地前缀安装（`MTL_INSTALL_PREFIX`）则总是重建；
2. 否则下载 `DPDK/dpdk` v26.07 zip → `for patch in ../../patches/dpdk/26.07/*.patch; do patch -p1 -i …; done`（**只应用平铺 `*.patch`，`windows/`、`hdr_split/` 子目录不在默认序列**）→ `meson build && ninja -C build && (sudo) ninja install` → 删除源码目录。
3. `script/check_dpdk_patches.sh` 校验补丁与上游 tarball 无偏移/无 fuzz。

26.07 补丁集内容以 Intel 网卡特性为主（ice TxPP/PHC/e830 MMIO 修复、iavf 队列限速/中断竞态等）。对 RK3588 的意义有两点：

- **仍然必须完整构建安装 DPDK**：`doc/build.md` 开头明确，即使使用 XDP/kernel socket 数据面，MTL 仍大量使用 DPDK 的 CPU、内存、队列、mbuf API；
- 与硬件 pacing/PTP 相关的补丁能力在 RK3588 上不可用（无 E810/E830），对应 `docs/req/02` §6 的“ARM pacing”约束——运行时只能 TSC 软件 pacing，构建期不需要为此加任何开关。

`script/build_drivers.sh`（打补丁的 ice/igc 内核模块）属 x86/Intel 网卡场景，DVS 节点镜像不需要。

## 3. 库层 `libmtl` 的编译细节

### 3.1 依赖探测一览（`lib/meson.build`）

| 依赖 | 必需性 | 影响 |
| --- | --- | --- |
| `libdpdk`（pkg-config，`>=25.03`） | 必需（非 Windows） | 链接基座 |
| `numa`、`m`、`pthread`、`dl`、`json-c` | 必需 | 配置解析与线程 |
| `libxdp` + `libbpf` | 可选探测 | **两者都找到才定义 `MTL_HAS_XDP_BACKEND`，并把 `lib/src/dev/mt_af_xdp.c` 编入库**；缺一则静默退化为“无 AF_XDP 后端” |
| `systemtap-sdt-dev`（`sys/sdt.h`）+ `dtrace` | 默认要求（`enable_usdt=true` 时缺失直接 `error()`） | USDT 探针对象 `mt_usdt_provider.o`；不需要时用 `MTL_BUILD_DISABLE_USDT=true` 或 `-Denable_usdt=false` |
| `mtl_gpu_direct` | 可选 | Level Zero GPU Direct |

源文件组织：`lib/src/meson.build` 汇总 core `mt_*.c`，`subdir` 进 `datapath/`（`mt_queue`、`mt_shared_queue`、`mt_shared_rss`、`mt_dp_socket`——kernel socket 数据面无条件编译）、`dev/`（`mt_dev.c` 恒编，`mt_af_xdp.c` 条件编）、`st2110/`（视频/音频/ANC 会话 + `pipeline/` + `experimental/`）。

### 3.2 SIMD 与 ARM

编译参数按“编译器是否接受该 flag”探测：`-msse4.2`、`-mavx2`（→`MTL_HAS_AVX2`）、`-mavx512vl`、`-mavx512vbmi2`。`st_avx2.c`/`st_avx512*.c` 恒在源列表中，但整个文件被 `#ifdef MTL_HAS_AVX2` 等包裹，因此 **aarch64 上这些文件编译为空翻译单元，不会失败**——库层对 RK3588 在源码级是 ARM 兼容的，颜色转换等退化为标量路径。

### 3.3 其他编译期行为

- 非 `debug` 构建启用 `-Werror`（对编译器版本敏感，见 §7 R6）；
- 非 `release` 构建全局注入 `MTL_SIMULATE_PACKET_DROPS=1`（模拟丢包，做时延测量须用 release）；
- 嵌入 `git describe` 的 `__MTL_GIT__` 与库版本串；`-Wl,-z,now -Wl,-z,relro` 加固；
- Windows 分支（静态 DPDK、TAP、`win_posix.c`）与 DVS 无关。

## 4. 其余组件的编译条件

| 组件 | 必需依赖 | 可选/条件 | RK3588 注意点 |
| --- | --- | --- | --- |
| `app/` | `mtl`、`json-c`、`pcap` | sdl2/sdl2_ttf/openssl/gpu_direct | 只是示例，P0 可整体跳过 |
| `manager/`（MtlManager） | C++17、`-lstdc++fs` | 找到 libxdp+libbpf 时**强制要求 `clang` 与 `llvm-strip`**，现场编译 `mtl.xdp.c`→`mtl.xdp.o` 并安装到 `<prefix>/lib/bpf/` | AF_XDP 路线必须带 XDP 库一起构建 MtlManager；`docs/req/02` §2.2 的推荐后端正是要这个 |
| `plugins/`（build.sh 只编 sample） | `mtl`、pthread | — | — |
| `plugins/st22_avcodec/`（独立工程，`script/build_st22_avcodec_plugin.sh`） | `mtl`、`libavcodec`、`libavutil` | meson 里**硬编码 `-msse4.2`，未做探测** | **aarch64 上无法编译**；DVS 用 MPP 编解码，本就不该用它（对应 req/02 §4.5 结论） |
| `ld_preload/` | 声明了 `mtl`、`dl` | 无任何构建目标 | 空壳（UDP shim 已随用户态协议栈移除），保留只为流程兼容 |
| `tests/`（KahawaiTest） | `mtl`、`gtest`、`numa`、`openssl` | gtest≥1.9 定义宏 | 运行需真实 VF，RK3588 上用不上 |
| `tests/unit` | 根工程 + `-Denable_unit_tests=true`、`gtest`、`gmock` | rpath 钉死 DPDK libdir（防第二份 DPDK 双载 bus 库崩溃） | 无 NIC、无 root，板上也能跑 |
| `tests/fuzz` | `-Denable_fuzzing`、clang libFuzzer | — | CI nightly 场景 |
| `tests/tools/RxTxApp` | `mtl`、`json-c`、`pcap` | sdl2（显示）、openssl、`-flarge-source-files` 探测 | 验收框架用 `.local_install` 里安装的那份 |

## 5. 两个 RK3588 应用（DVS 重点）

两者都是**独立 meson 工程，不在 `app/meson.build`、不在 `build.sh`、不在 CI**（`.github/workflows` 无 rk3588/aarch64 job），构建方式固定为在各自目录 `meson setup build && ninja -C build`，并依赖已安装的 `mtl.pc`（注意用 `MTL_INSTALL_PREFIX` 时同步导出 `PKG_CONFIG_PATH`）。

### 5.1 `app/rk3588_end2end`（工程名 rk3588_e2e，`warning_level=3`）

按阶段组织为 4 个静态库 + 5 个可执行文件，依赖探测决定构建深度：

| 阶段 | 目标 | 依赖 | 缺失行为 |
| --- | --- | --- | --- |
| P1 HAL | `e2e_hal`、`test_hal` | `libdrm`（必需） | 缺 libdrm 直接 configure 失败 |
| P2 IO | `e2e_io`、`test_io` | libdrm + threads（V4L2 用内核头，无库依赖） | 同上 |
| P3 Codec | `e2e_codec`、`test_codec` | `rockchip_mpp` + `librga`（pkg-config） | **缺失时静默跳过**（连 test_codec 都不生成） |
| P4 Pipeline | `e2e_pipe`、`test_pipeline` | mpp+rga（`mtl` 有才链） | 同上 |
| P5 整机 | `e2e_app` | mpp+rga+**mtl+json-c 四者全齐** | 缺失时静默跳过 |

风险：`rockchip_mpp`、`librga`、`mtl`、`json-c` 都是 `required: false`，依赖没装齐时 `ninja` 仍“成功”，但 `e2e_app` 根本没构建——**构建脚本必须显式校验 `e2e_app` 产物存在**，不能只看退出码。

### 5.2 `app/rk3588_mp4_dist`（工程名 mp4_dist）

- `meson_options.txt`：`build_tx`（默认 true）、`build_rx`（默认 true），全关时报错。
- 公共必需：`threads`、`mtl`（`required: true`）；TX 与 RX 任一开启都必需 `libavformat/libavcodec/libavutil/json-c`（FFmpeg 版本按 versions.env 钉 7.0 的发行版包或自编译）。
- TX 平台无关（FFmpeg 解复用 + ST2110-22/30 发送），x86 上可用 `-Dbuild_tx=true -Dbuild_rx=false` 单独构建。
- RX 仅 RK3588：`build_rx=true` 时做 **MPP 链接测试**（`#include <rockchip/…>` + `cc.links`），失败即 configure 报错并提示改用 tx-only；另必需 `libdrm`、`alsa`。
- 产物：`mp4_dist_tx`、`mp4_dist_rx`、`rx_minimal`、`test_drm_plane`，以及 5 个注册为 meson test 的主机侧单测（`tx_tai_ts`、`e2e_stamp`、`media_clk`、`drm_mode`、`display_layout`，`ninja -C build test` 可跑）。

### 5.3 板端外部依赖清单（pkg-config 名）

| pkg-config | 来源 |
| --- | --- |
| `mtl` | 本仓库构建安装产物 |
| `rockchip_mpp` | BSP / `librockchip-mpp-dev` |
| `librga` | BSP / rga 库包（部分发行版需自配 `.pc`） |
| `libdrm` | `libdrm-dev` |
| `alsa` | `libasound2-dev` |
| `libavformat/avcodec/avutil` | FFmpeg（建议对齐 7.0） |
| `json-c` | `libjson-c-dev` |

V4L2 采集只依赖内核 UAPI 头文件，无用户库依赖。

## 6. 交叉编译现状

仓库内**没有任何 meson cross 文件**（`**/*cross*.ini` 无结果），上游文档与 CI 全部假设目标机本机编译。对应 DVS 的落地口径：

1. P0 按 `docs/req/05` §13 在 RK3588 板上原生编译（DPDK、MTL、两个应用全在板或同架构容器内构建），不引入交叉工具链变量；
2. 若后期做 CI 交叉，需要自备 aarch64 toolchain 的 `machine file`，且 DPDK 也要用同一份交叉配置重建，`build_dpdk.sh` 的“已安装版本检测”会自然拦住混用；
3. 无论哪种方式，DPDK 默认 `-march=native`：同型号 RK3588 之间制品可互用，跨 CPU 型号部署时参考 `doc/build.md` §4.3，用固定 `cpu_instruction_set` 重新配置。

## 7. 与 docs/req 关注的差距与风险

| ID | 风险/差距 | 说明与应对 | 关联 req 条目 |
| --- | --- | --- | --- |
| RB1 | aarch64 无 CI 覆盖 | 上游 CI 仅 x86（容器 + E810 裸机）。RK3588 上的构建成功不能靠上游保证，DVS 需在节点镜像流水线中自锁版本并留构建日志 | req/05 P0 交付物“可复现的节点镜像和构建脚本” |
| RB2 | AF_XDP 后端“静默缺失” | libxdp/libbpf 缺任何一个，库照常构建但无 `native_af_xdp:` 后端；MtlManager 也不会生成 `mtl.xdp.o`。构建脚本应先跑 `script/build_ebpf_xdp.sh`，并在 configure 日志断言 `MTL_HAS_XDP_BACKEND` | req/02 §2.2（RK3588 推荐 AF_XDP）、§6 |
| RB3 | USDT 默认开启是硬依赖 | 缺 `systemtap-sdt-dev`/`dtrace` 时根工程 configure 直接 `error()`。镜像不打算用 eBPF 探针就设 `MTL_BUILD_DISABLE_USDT=true` | req/02 §4.6（可观测性） |
| RB4 | `st22_avcodec` 插件在 ARM 不可编译 | 其 meson 硬编码 `-msse4.2`。DVS 的 ST2110-22 编解码走 MPP（应用层已有，或后续封装成 MTL 插件），不要引入该插件构建 | req/02 §4.5 |
| RB5 | end2end 缺依赖时静默降级 | mpp/rga/mtl/json-c 缺失只是跳过目标，`ninja` 仍返回成功。验收必须检查产物清单 | req/05 P0-2 单板回环 |
| RB6 | `-Werror` 对编译器版本敏感 | 非 debug 构建全工程 `-Werror`；Ubuntu 22.04 gcc 11 与 24.04 gcc 13/14 的告警集不同，换基座 OS 可能编不过。失败时优先用 `debugonly`（无 -Werror）定位，再决定固定 gcc 版本 | req/05 D12（节点 OS 选型） |
| RB7 | 非 release 注入模拟丢包 | `buildtype != release` 全局定义 `MTL_SIMULATE_PACKET_DROPS`，时延/丢帧测量必须用 release 产物；需要 ASan 时用 `debug`+ASan 并接受丢包注入存在的事实 | req/05 P0-5 指标测量 |
| RB8 | 两套安装树不可互换 | `/usr/local`（gtest/RxTxApp 默认）与 `.local_install`（pytest 验收硬编码）。镜像内统一 prefix，避免运行时加载到陈旧 `libmtl.so` | AGENTS.md §Build |
| RB9 | DPDK 无法省略 | 即使只用 `kernel:`/`native_af_xdp:`，libmtl 仍链接 DPDK。镜像必须带 patched DPDK 运行时 | req/02 §2.2、doc/build.md 开头 |
| RB10 | 构建期≠运行期约束 | hugepages、CAP_NET_RAW/VFIO、MtlManager 需 root 属运行期（`docs/req/02` §6），构建本身除 `ninja install` 外不需要特权——用 `MTL_INSTALL_PREFIX` 可全程非 root | req/02 §6 |

## 8. RK3588 推荐构建顺序（速查）

```bash
# 0) 系统依赖（见 doc/build.md §1 + §5.3 板端包）
# 1) 预检并构建 xdp-tools/libbpf（仅当启用 AF_XDP）
script/build_ebpf_xdp.sh                 # 已装则只 --check all
# 2) 打补丁构建安装 DPDK（版本自动读 versions.env）
script/build_dpdk.sh                     # pkg-config 报 26.07.91_mtl_* 即已就绪
# 3) 构建安装 MTL 全栈（本地前缀免 root）
MTL_INSTALL_PREFIX=$PWD/.local_install ./build.sh
# 4) 单测（无 NIC、无 root）
./build.sh unit
# 5) 两个参考应用（确保 PKG_CONFIG_PATH 能找到 mtl.pc，注意 bash 赋值不做 glob 展开，用 find 定位）
export PKG_CONFIG_PATH="$(find $PWD/.local_install -name mtl.pc -printf '%h\n' | head -1):${PKG_CONFIG_PATH:-}"
( cd app/rk3588_end2end  && meson setup build && ninja -C build )
( cd app/rk3588_mp4_dist && meson setup build && ninja -C build )
# 6) 产物断言（防 RB5 静默降级）
test -x app/rk3588_end2end/build/e2e_app
test -x app/rk3588_mp4_dist/build/mp4_dist_tx -a -x app/rk3588_mp4_dist/build/mp4_dist_rx
```

## 9. 参考文件索引

`build.sh`、`meson.build`、`meson_options.txt`、`versions.env`、`VERSION`；`lib/meson.build`、`lib/src/{,datapath,dev,st2110}/meson.build`；`script/build_dpdk.sh`、`script/build_ebpf_xdp.sh`、`script/build_drivers.sh`、`script/check_dpdk_patches.sh`、`script/common.sh`；`manager/meson.build`；`plugins/meson.build`、`plugins/st22_avcodec/meson.build`、`script/build_st22_avcodec_plugin.sh`；`ld_preload/meson.build`；`tests/meson.build`、`tests/unit/meson.build`、`tests/tools/RxTxApp/meson.build`；`app/meson.build`、`app/rk3588_end2end/meson.build`、`app/rk3588_mp4_dist/meson.build(+meson_options.txt)`；`doc/build.md`；`docs/req/02-MTL基座分析.md` §6、`docs/req/05-实施路线与验证.md` P0/§13。
