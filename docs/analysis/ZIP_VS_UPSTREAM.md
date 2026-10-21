# Zip 改造版 vs 官方 MTL 差异与去侵入方案

对比对象：

- 官方仓库：当前工作区 `Media-Transport-Library/`，版本 `26.09.0.DEV`
- Zip 解压：当前工作区 `media-transport-library-zip/`（来自 `media-transport-library.zip`）

Zip 是一份**精简后的旧基线 MTL + RK3588 业务应用**。业务价值在两个独立 meson 工程里，库核心的多数差异来自上游演进，只有少量 meson 改动是为 ARM 编译做的。

## 1. 目录级对照

Zip 顶层只有：`app/` `lib/` `manager/` `plugins/` `python/` `ecosystem/` `gpu_direct/` `ld_preload/` `patches/` `config/` `script/` `tools/`。

官方额外有完整工程骨架：`include/` `VERSION` 根 `meson.build` `tests/` `rust/` `doc/` `docker/` `.github/` 等。Zip 依赖已安装的 `libmtl` 头文件与 pkg-config，本身不是可独立 bootstrap 的完整源码树。

### Zip 独有目录（本版本真正新增）

| 路径 | 角色 |
|------|------|
| `app/rk3588_end2end/` | HDMI 采集 -> RGA/MPP 编码 -> ST22 TX；ST22 RX -> MPP 解码 -> DRM |
| `app/rk3588_mp4_dist/` | MP4 解复用 -> ST22 视频 + ST30 音频 TX；RX 侧 MPP/DRM/ALSA + PiP |
| `app/udp/`、`ld_preload/udp/`、`lib/src/deprecated/udp/` | 旧用户态 UDP / UFD / preload，官方 26.09 已删除 |
| `lib/src/st2110/st_*fastmetadata*` | 旧版独立 ST41 会话实现 |
| `ecosystem/librist/` | librist 补丁与构建脚本 |
| `patches/dpdk/22.03`–`23.11`、旧 ICE 驱动补丁 | 旧 DPDK/ICE 补丁集 |

### 官方独有、Zip 未带的库文件

- `lib/src/st2110/st_rx_common.c/.h`
- `lib/src/st2110/st_tx_video_rl.h`
- ST2110-41 已并入 ST40 会话（`ST40_*_FLAG_FAST_METADATA`）

## 2. 这个版本实际改了什么

分三层看，避免把「旧上游」和「RK 改造」混在一起。

### A. RK3588 业务（应保留）

两个应用都是**独立 meson 工程**，通过 pkg-config 的 `dependency('mtl')` 链接已安装的库。

**`rk3588_end2end`**

- 采集：V4L2 `rk_hdmirx`（Multiplanar）
- 转换/编解码：RGA + Rockchip MPP
- 显示：DRM/KMS
- 传输：`st22_tx_create` / `st22_rx_create`，`ST22_TYPE_FRAME_LEVEL` + `ST22_PACK_CODESTREAM`
- 网卡路径：默认 `MTL_PMD_NATIVE_AF_XDP`（注释针对 r8168 单队列）
- 单板 both：fork 两个进程，各调一次 `mtl_init`（DPDK EAL 不能在同进程二次 init）

**`rk3588_mp4_dist`**

- TX：FFmpeg demux，H.264 Annex-B + ST22；PCM + ST30
- RX：ST22 -> MPP -> DRM；ST30 -> ALSA；playout / A-V sync / PiP
- 同样默认 `native_af_xdp:<ifname>`，JSON/CLI 可改 `kernel:<ifname>`
- TX 可在 x86 单独编译（`-Dbuild_rx=false`）

公共 API 使用面：

```text
mtl_init / mtl_start / mtl_stop / mtl_uninit
mtl_set_log_level
st22_tx_create / st22_tx_get_fb_addr / get_next_frame / notify_frame_done
st22_rx_create / notify_frame_ready / st22_rx_put_framebuff
st30_tx_create / st30_rx_create（仅 mp4_dist）
ST22_TX/RX_FLAG_DISABLE_BOXES
ST22_TX_FLAG_USER_TIMESTAMP | ST22_TX_FLAG_USER_PACING（mp4_dist TAI pacing）
```

头文件仅 `mtl_api.h`、`st_api.h`、`st20_api.h`、`st30_api.h`。没有 `#include` 库内部头，没有改 `lib/src/st2110` 热路径。

### B. 为 ARM 编译改的库构建（可去掉）

`lib/meson.build` 相对官方的实质改动：

1. 注释掉 `-msse4.2`，避免 aarch64 全局 SSE 标志
2. 强制 `mtl_has_simd = false`，从而不定义 `MTL_HAS_AVX2/AVX512`
3. `-Wunused-parameter` 改成 `-Wno-unused-parameter`

官方 26.09 已写成：

```meson
if cc.has_argument('-msse4.2')
  mtl_c_args += ['-msse4.2']
endif
```

AVX 源文件本体包在 `#ifdef MTL_HAS_AVX2` / `MTL_HAS_AVX512` 里。aarch64 上 `cc.has_argument('-mavx2')` 为假，这些 `.c` 编成空翻译单元即可。**官方构建脚本已经能在 ARM 上跳过 x86 SIMD，不必再改库源码。**

### C. 旧基线残留（不要移植到 26.09）

| 内容 | 说明 |
|------|------|
| `subdir('deprecated/udp')` | 用户态 UDP 栈，官方已删 |
| 独立 ST41 三个 transmitter/session 文件 | 官方用 ST40 + Fast Metadata 标志 |
| PTP `qbv_enabled`、旧 RL shaper、`double` pacing 字段 | 上游已重写（`long double`、独立 `st_tx_video_rl.h`） |
| `mt_dp_socket.c` 依赖 `deprecated/mudp_api.h` | 官方改为自包含 `mt_dp_init_sockaddr` |
| 旧 DPDK 22.x/23.x ICE RL 补丁 | RK3588 走 AF_XDP / kernel socket，用不上 E810 RL |
| 约 53 个 lib 文件、274 个非 lib 文件仅 CRLF 差异 | 无功能意义 |

`lib/` 中约 70 个「内容不同」的文件，主体是 26.09 相对旧快照的上游演进，不是 RK 功能补丁。

## 3. 数据路径与平台约束（改造时要守住）

RK3588 场景的运行时选择已经写在应用里：

```text
端口名 native_af_xdp:eth0  -> MTL_PMD_NATIVE_AF_XDP
端口名 kernel:eth0         -> MTL_PMD_KERNEL_SOCKET
```

配套条件（应用 README 已写）：

- MtlManager + hugepages（AF_XDP）
- libbpf / libxdp
- RK3588：CPU0 作 EAL main，worker 不要填 0；A55=0–3，A76=4–7
- r8168 单队列：`tx_queues_cnt=rx_queues_cnt=1`
- ST22 压缩码流，不走 ST20 未压缩 + AVX CSC，ARM 上关 SIMD 不影响这条产品路径

## 4. 能否零修改 / 最小修改官方源码

**可以做到零修改 `lib/`、`include/`。** RK 功能已经是库外应用。

推荐目标形态：

```text
Media-Transport-Library/          # 官方 26.09，原样构建安装
app/rk3588_end2end/               # 独立 meson，dependency('mtl')
app/rk3588_mp4_dist/              # 独立 meson，dependency('mtl')
```

两个应用目录保持独立工程即可，不必挂进官方 `app/meson.build`。官方 app 工程没有 `subdir('rk3588_*')`，zip 里也没有把它们编进主 `app/meson.build`。

### 零修改路径（优先）

1. 官方树 `./build.sh` 安装 `libmtl`（需 libxdp+libbpf，打开 AF_XDP）
2. 把 zip 的 `app/rk3588_end2end`、`app/rk3588_mp4_dist` 原样放到官方树旁或树内
3. 各自 `meson setup && ninja`
4. 运行时端口继续用 `native_af_xdp:` 或调试用 `kernel:`

官方 26.09 仍导出 zip 用到的符号：`st22_tx_create`、`st22_rx_create`、`ST22_PACK_CODESTREAM`、`ST22_TX_FLAG_DISABLE_BOXES`、`ST22_TX_FLAG_USER_TIMESTAMP`、`ST22_TX_FLAG_USER_PACING`、`MTL_PMD_NATIVE_AF_XDP`。

### 若 ARM 构建仍失败：最小补丁清单

只动构建，不动数据面：

1. **仅当** aarch64 的 meson 误判 `cc.has_argument('-msse4.2')` 为真时，给 `lib/meson.build` 加 `host_machine.cpu_family() != 'x86_64'` 守卫。大约数行。
2. 应用侧若 26.09 微调了 `st22_tx_ops` / `st22_rx_ops` 字段，只改 RK 应用适配，仍不改库。

不要做的事：

- 把 `mtl_has_simd = false` 写死进主线（x86 性能路径需要 AVX）
- 恢复 `deprecated/udp`、`ld_preload/udp`
- 为 RK 移植旧 ICE/DPDK RL 补丁
- 为 ST22 码流路径去改 `st_tx_video_session.c` / pacing

## 5. 建议落地步骤

1. 以官方 `26.09.0.DEV` 为唯一库基线。
2. 只迁 `app/rk3588_end2end/` 与 `app/rk3588_mp4_dist/`。
3. 在 RK3588 上先编官方库（确认 SIMD 宏未打开），再编两个 app。
4. 用 `kernel:` 做通路，再用 `native_af_xdp:` + MtlManager 做性能路径。
5. 核对 26.09 头文件与 zip 应用的 ops 字段；差异只修应用。

这条路径把「改 MTL 内核」收敛为「外挂两个应用 + 可选 meson 三行 CPU 族判断」。
