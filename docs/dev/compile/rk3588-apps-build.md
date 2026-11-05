# RK3588 应用直接构建说明

`app/rk3588_live_tx` 与 `app/rk3588_playout_rx` 是独立 meson 工程，**不**纳入根 `./build.sh`，也**不**支持 x86 桌面构建。它们只在 RK3588 (aarch64) sysroot 上交叉编译。

## 1. 前置

- aarch64 sysroot（含 `rockchip_mpp`、`librga`、`libdrm`、`alsa`、`json-c`、`FFmpeg`、`mtl` 与对应 `.pc`）。
- ARM GNU Toolchain 13.3（`aarch64-none-linux-gnu-gcc`）。
- `mtl-pkg-config` 包装脚本 + 交叉文件 `aarch64.ini`（sysroot 锁定，避免宿主 pkg-config 泄漏）。参考 `docs/dev/compile/5.10-debian-cross-compile.md` 与 `rk3588-cross-build-script.md`。

## 2. 直接构建（本地或编译服务器）

```bash
export PATH=<toolchain>/bin:$PATH
export PKG_CONFIG_SYSROOT_DIR=<sysroot>
export PKG_CONFIG_LIBDIR=<sysroot>/usr/lib/pkgconfig:<sysroot>/usr/lib/aarch64-linux-gnu/pkgconfig

# 编码发送端 (TX-only)
cd app/rk3588_live_tx
meson setup build --cross-file <sysroot>/aarch64.ini
ninja -C build

# 解码接收端 (RX-only)
cd app/rk3588_playout_rx
meson setup build --cross-file <sysroot>/aarch64.ini
ninja -C build
```

产物：
- `app/rk3588_live_tx/build/live_ingest`
- `app/rk3588_playout_rx/build/file_playout_rx`

## 3. 本地 LSP 刷新

为 clangd 保留 `build/`（含 `compile_commands.json`）；交叉文件仍用 wrapper 三件套。本地构建成功即可刷新代码提示，无需运行二进制。

## 4. 编译服务器

devs 上使用 `script/build_rk3588_cross.sh`（`--stages mtl,apps` 可跳过 DPDK），或按 §2 直接 `meson setup build-aarch64 ...`。产物路径为 `build-aarch64/`。

## 5. 注意

- `rk3588_live_tx` 已无 RX/解码/显示代码；`rk3588_playout_rx` 已无 TX 发送传输代码。二者通过网络互通验证，不互相编译源码。
- `dvs_media` in-tree 复用 `rk3588_playout_rx/rx/*`、`shared/*`（含 `mp4_demux`/`extradata_handler`）与 `rk3588_live_tx` donor chain；修改这些路径时同步更新 `app/dvs_media/meson.build`。
