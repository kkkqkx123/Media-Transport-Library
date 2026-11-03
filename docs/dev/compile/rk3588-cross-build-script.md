# RK3588 交叉编译脚本使用说明

脚本：`script/build_rk3588_cross.sh`

将 `docs/dev/02-x86到RK3588交叉编译分析.md` 与 `docs/dev/compile/5.10-debian-cross-compile.md` 中已验证的手工流程固化为可重复执行的编排脚本，统一控制编译范围与参数。

## 1. 解决的手工编译问题

| 手工痛点 | 脚本处理 |
|---|---|
| 工具链/包装脚本/软链路径散落、易不一致 | 自动探测 sysroot 与 ARM GNU Toolchain，生成 `aarch64-cc-wrap`/`aarch64-cxx-wrap`/`mtl-pkg-config` 与 `aarch64.ini` |
| `numaif.h` 缺 `MPOL_LOCAL` | 自动给 `numaif.h` 补 `#include <linux/mempolicy.h>` |
| FFmpeg 未版本化 `.so` 软链缺失 | 自动补齐 `libavformat.so`/`libavcodec.so`/`libavutil.so` |
| `-Werror` 在 aarch64 gcc 上新告警 | 包装脚本统一追加 `-Wno-error=unused-variable -Wno-error=unused-but-set-variable` |
| 依赖缺失时 `ninja` 仍“成功”但产物缺失 | 构建末尾 `assert_products` 强制检查 `libmtl.so` 与应用可执行文件 |
| XDP 后端静默缺失 | 构建日志明确输出 `XDP backend ENABLED/OFF` |
| DPDK/xdp-tools 源码需手动下载 | `--fetch` 可选自动下载 |

## 2. 前置条件

- x86_64 主机，具备：`meson`、`ninja`、`pkg-config`、`python3`、`clang`、`patch`、`curl`/`unzip`（仅 `--fetch`）。
- ARM GNU Toolchain 13.3（`aarch64-none-linux-gnu-gcc`）已安装，或由 `--toolchain` 指定。
- aarch64 sysroot 已从 RK3588 板导出并完成软链改写（见 `5.10-debian-cross-compile.md` §2–§4），或由 `--sysroot` 指定。
- 仓库源码（含 `patches/dpdk/26.07/*.patch`）。

## 3. 参数与环境变量

| 参数 | 环境变量 | 默认 | 说明 |
|---|---|---|---|
| `--sysroot PATH` | `RK3588_SYSROOT` | 自动探测 | aarch64 sysroot 根 |
| `--toolchain PATH` | `RK3588_TOOLCHAIN` | 自动探测 | ARM GNU Toolchain 根 |
| `--stages LIST` | `RK3588_STAGES` | `all` | 逗号分隔：`dpdk,xdp,mtl,manager,apps` |
| `--buildtype TYPE` | `RK3588_BUILDTYPE` | `release` | `release`/`debug`/`debugonly`/`plain` |
| `--jobs N` | `RK3588_JOBS` | `nproc` | 并行度 |
| `--clean` | — | 否 | 构建前清空对应 build 目录 |
| `--fetch` | — | 否 | 缺源码时自动下载 DPDK/xdp-tools |
| `--no-xdp` | `RK3588_ENABLE_XDP=0` | 启用 | 跳过 AF_XDP 后端 |
| `--no-apps-rx` | — | RX 开启 | file_playout 仅构建 TX |
| `--usdt` | `RK3588_ENABLE_USDT=1` | 关闭 | 启用 USDT（需 sysroot 有 `sys/sdt.h`） |
| `--patch-stdbool` | — | 否 | 为缺失 `stdbool.h` 的应用 `.c` 补 include |

## 4. 使用示例

```bash
# 全流程（自动探测 sysroot/toolchain）
script/build_rk3588_cross.sh

# 仅构建 MTL 与应用，跳过 DPDK/xdp（已装好）
script/build_rk3588_cross.sh --stages mtl,apps

# 调试构建，清空旧产物
script/build_rk3588_cross.sh --buildtype debugonly --clean

# TX-only，禁用 XDP
script/build_rk3588_cross.sh --no-xdp --no-apps-rx

# 指定 sysroot/toolchain
script/build_rk3588_cross.sh --sysroot /home/kkkqkx/mtl/sysroot \
  --toolchain /home/kkkqkx/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu

# 源码缺失时自动下载
script/build_rk3588_cross.sh --fetch
```

## 5. 构建产物与验证

- `libmtl.so`：`$SYSROOT/usr/lib/aarch64-linux-gnu/libmtl.so`（aarch64）。
- 应用：`app/rk3588_live_ingest/build-aarch64/live_ingest`、`app/rk3588_file_playout/build-aarch64/file_playout_tx`（及 `file_playout_rx`）。
- 脚本末尾自动执行 `file`/`test -x` 断言；若失败请检查 sysroot `.pc` 是否齐全（RB16）。

## 6. 常见问题

- **sysroot 路径硬编码不一致**：包装脚本与 cross file 使用 `/home/kkkqkx/mtl/...`；本地工作树在 `/home/kkkqkx/code/mtl/` 时，建立符号链接 `ln -s /home/kkkqkx/code/mtl /home/kkkqkx/mtl`，或用 `--sysroot` 指定真实路径（脚本会重新生成包装脚本）。
- **DPDK 源码缺失**：使用 `--fetch`，或手动按 `5.10-debian-cross-compile.md` §7.1 下载解压并打补丁。
- **XDP 后端 OFF**：检查 `libxdp.pc`/`libbpf.pc` 是否在 sysroot；老 BSP 内核可加 `--no-xdp` 退化为 socket 后端。
- **应用未生成**：多为依赖缺失静默跳过，检查 `meson` configure 日志与 `PKG_CONFIG_LIBDIR`。

## 7. 相关文档

- `docs/dev/02-x86到RK3588交叉编译分析.md`
- `docs/dev/compile/5.10-debian-cross-compile.md`
- `docs/dev/compile/dpdk-cross-compile-pitfalls.md`
- `docs/archive/RK3588_CROSS_COMPILE_PROGRESS.md`