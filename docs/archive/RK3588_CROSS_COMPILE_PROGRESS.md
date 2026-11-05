# RK3588 交叉编译进度归档（DPDK + MTL 库 + libxdp + rk3588 应用）

> 最后更新：2026-11-05
> 目标：在 x86_64 上为 RK3588（aarch64，板端 glibc 2.31 / Debian 11 5.10 内核）交叉编译 Media-Transport-Library 及其 rk3588 应用。
> 关联文档：
> - `docs/dev/compile/dpdk-cross-compile-pitfalls.md`（DPDK 踩坑详录，含三件套完整内容）
> - `docs/dev/compile/5.10-debian-cross-compile.md`（应用层交叉编译总流程）
> - `docs/plan/rk3588/RK3588_ENCODE_DECODE_ROLE_REFACTOR.md`（编码/解码角色重构：ingest 只产流，playout 只收流）

---

## 0. 当前状态（✅ 已完成）

| 阶段 | 结果 |
|---|---|
| DPDK 26.07 交叉编译 | ✅ 729/729，0 错误，已安装到 sysroot（`DESTDIR`，`prefix=/usr`） |
| MTL 核心库（lib/） | ✅ 52/52，0 错误，产出 `libmtl.so`（ARM aarch64，纯 aarch64 依赖）；已 `meson install --prefix=/usr` 装入 sysroot |
| libxdp（XDP 后端依赖） | ✅ 1.6.3 装入 sysroot，MTL 启用 **AF_XDP 后端**（动态链同一个 libbpf 1.4.0） |
| MTL **release** 构建（devs） | ✅ 52/52，0 错误（`-Werror` 对未用变量类告警降级，详见 §6） |
| rk3588 应用（角色重构后） | ✅ **rk3588_live_ingest** 编码发送端（TX-only）与 **rk3588_file_playout** 解码接收端（RX-only）均完成交叉配置验证（见 §6.4）；`x86` 桌面构建分支已删除，全部硬件依赖改为必需 |
| 环境同步 | ✅ sysroot/工具链/bin 已在本地 Arch 机 `/home/kkkqkx/mtl/` 镜像；libxdp 与增强包装脚本已**回写** devs；wrapper 系 sysroot 独缺的 `mtl` 三件套已从 devs `/data/kkkqkx/mtl/sysroot` 同步补齐（见 §1.3） |

**结论**：DPDK → libbpf/libxdp → MTL 库 → rk3588 应用的完整交叉编译链路已打通，并在 devs 完成正式 release 验证。

---

## 1. 环境与路径

### 1.1 两台机器
- **devs**（编译服务器，Ubuntu 18.04 x86_64）：工作区 `/data/kkkqkx/mtl/`；交叉文件 `sysroot/aarch64.ini`；包装脚本 `bin/`；工具链 `/data/kkkqkx/arm-gnu-toolchain-13.3...`；buildroot host meson/ninja 在 `/data/kkkqkx/rk3588bsp/.../host`。**正式大规模编译在此验证**。
- **本地 Arch 机**（x86_64）：仓库 `/home/kkkqqx/code/Media-Transport-Library`；环境镜像于 `/home/kkkqkx/code/mtl/`；本地 meson 1.12.1 / ninja 1.13.2 / clang 23（支持 `bpf` 目标）。

### 1.2 sysroot 清单与缺件同步（2026-11-05）

本地共三处 sysroot 镜像，分工不同：

- 仓库内 `sysroot/`：内容最全（含 `libmtl.so` 双份相同拷贝、`mtl.pc 26.09.0.DEV`、`usr/include/mtl/` 头、`rockchip_mpp 1.3.9`、`librga 2.1.0`、`libdrm`、`alsa`、`FFmpeg 58.45/58.91/56.51` 未版本化软链、`json-c`），应用构建以它为准。
- `/home/kkkqkx/mtl/sysroot`：wrapper 三件套环境（`bin/aarch64-cc-wrap`、`mtl-pkg-config`、`aarch64.ini` 指向 wrapper），其余硬件依赖齐备，**独缺 `mtl` 三件套**（库、头、`mtl.pc` 均无）。
- `/home/kkkqkx/code/mtl/sysroot`：早期环境镜像，同样缺 `mtl` 三件套，仅作备份参考。

缺件来源以 devs 的 `/data/kkkqkx/mtl/sysroot` 为准（`usr/lib/libmtl.so`、`usr/lib/pkgconfig/mtl.pc`、`usr/include/mtl/` 头齐备），**不用 rk3588bsp 的 3rdparty 拷贝**。同步方式按既有惯例手动 `rsync`（devs → 本地，系统库不在 `sync_to_devs.sh` 范围内）：

```bash
rsync -avz devs:/data/kkkqkx/mtl/sysroot/usr/lib/libmtl.so /home/kkkqkx/mtl/sysroot/usr/lib/
rsync -avz devs:/data/kkkqkx/mtl/sysroot/usr/lib/pkgconfig/mtl.pc /home/kkkqkx/mtl/sysroot/usr/lib/pkgconfig/
rsync -avz devs:/data/kkkqkx/mtl/sysroot/usr/include/mtl/ /home/kkkqkx/mtl/sysroot/usr/include/mtl/
```

另有一个本机验证坑：仓库内 `sysroot/aarch64.ini` 用裸 `pkg-config`，meson 会把 `.pc` 的 `-I/usr/include` 当宿主路径丢弃，导致 MPP 链接测试报头缺失。本地验证必须走 wrapper 三件套交叉文件（`/home/kkkqkx/mtl/sysroot/aarch64.ini`，`mtl-pkg-config` 锁定 sysroot），并把 ARM GNU 13.3 工具链 `bin` 加入 `PATH`（`aarch64-none-linux-gnu-ar` 等）。

### 1.3 本地环境布局（`/home/kkkqkx/code/mtl/`）
```
mtl/
├── sysroot/                 # 板端 rootfs（含 libdpdk.pc / libbpf / libxdp / mpp / rga / drm / alsa / ffmpeg / numa / json-c / libbsd 等）
│   └── aarch64.ini          # 交叉文件
├── bin/
│   ├── aarch64-cc-wrap      # gcc 包装（注入 --sysroot/-B/-L/-isystem/-rpath-link/-lm；并对未用变量类 -Werror 降级）
│   ├── aarch64-cxx-wrap     # g++ 包装
│   └── mtl-pkg-config       # pkg-config 包装（锁定 sysroot）
└── arm-gnu-toolchain-13.3.../   # ARM GNU Toolchain 13.3
```

---

## 2. 三件套（最终可用方案）

详见 `docs/dev/compile/dpdk-cross-compile-pitfalls.md`。要点：

1. **gcc/g++ 包装脚本**：每次调用自动追加 `--sysroot` + `-B` + `-L`(×2) + `-isystem`（解决裸 `make`/`configure` 找不到 crt/libm）+ `-Wl,-rpath-link`(×2)（解决 `libelf→zlib`/`libbsd→libmd`/`libpcap→dbus` 传递依赖）+ `-lm`（解决 `rte_sched` 数学函数）。
2. **`mtl-pkg-config`**：强制 `PKG_CONFIG_SYSROOT_DIR`/`PKG_CONFIG_LIBDIR` 指向本仓库 sysroot，杜绝 buildroot 的 pkg-config 泄漏。
3. **交叉文件 `aarch64.ini`**：`[binaries]` 指向上述包装脚本；`[host_machine] cpu='armv8-a'`；`[properties] platform='generic'`。

> devs 与本地两份包装脚本内容一致（仅绝对路径 `/data`↔`/home` 不同）。本地版额外支持裸 `make`/`configure`。

---

## 3. DPDK 阶段

踩坑与修复（详 `dpdk-cross-compile-pitfalls.md`，P1–P11）：toolchain `include-fixed` 冲突、`pmdinfogen` 的 elftools 不兼容、静态 `.a` 误选、buildroot pkg-config 泄漏 libbsd、误删 glibc 辅助归档、meson 逗号吞 `-Wl,-rpath-link`、传递 NEEDED 解析、`rte_sched` 缺 `-lm`、DPDK 强制 libbsd、缺 `platform`、 `cortex-a76` 不被接受。

---

## 4. MTL 库阶段

- `meson setup`（根工程只构建 `lib/`）：`-Dbuildtype=release -Denable_usdt=false`（USDT 默认开但 sysroot 无 `sys/sdt.h`，关闭不影响功能）。
- **`MPOL_LOCAL` 未定义**：sysroot 的 `numaif.h` 补 `#include <linux/mempolicy.h>`（与新版 glibc 一致，带 include guard）。
- **release `-Werror` 与 USDT-off 冲突**：USDT 关闭留下 `usdt_stat`/`rtp_ts`/`idx`/`meta_num_before_reset` 未用变量告警 → 在包装脚本 `"$@"` **之后**追加 `-Wno-error=unused-variable -Wno-error=unused-but-set-variable`（仅降级这一类、保留 `-O2` 与其余 `-Werror`）。
- 结果：`libmtl.so`（ARM aarch64，NEEDED 含 `libxdp.so.1`）。
- `meson install --prefix=/usr --destdir=sysroot`：把 `libmtl.so` + 头文件 + `mtl.pc` 装入 sysroot（供应用 `dependency('mtl')` 拾取）。

---

## 5. libxdp（XDP 后端依赖）

RK3588 走 native AF_XDP 必须。`xdp-tools 1.6.3` 自带 libbpf 子模块，但 **MTL 已用 sysroot 的 libbpf 1.4.0**；若 libxdp 静态链入子模块 libbpf 1.6 会导致运行时 `struct bpf_object*` ABI 不一致。故让 libxdp **动态链接同一个 libbpf 1.4.0**（5.10 文档验证过的组合）：

```bash
cd xdp-tools-1.6.3
export PKG_CONFIG=/.../bin/mtl-pkg-config CC=/.../bin/aarch64-cc-wrap CLANG=/usr/sbin/clang \
       BPF_TARGET=bpf PREFIX=/usr
./configure
make -C lib/libxdp libxdp.so libxdp.pc        # 仅 .so + .pc
```

装入 sysroot：`libxdp.so.1.6.0` + 软链、`headers/xdp/*.h`、`libxdp.pc`（`libdir` 修正为 `/usr/lib/aarch64-linux-gnu`）。已**回写** devs。

---

## 6. rk3588 应用构建（devs 验证）

两个独立 meson 工程（`app/rk3588_live_ingest`、`app/rk3588_file_playout`），各自 `meson setup build && ninja -C build`，传同一 `--cross-file sysroot/aarch64.ini`。

### 6.1 依赖（均经 `mtl-pkg-config` 从 sysroot 解析）
- end2end：`libdrm` / `threads` / `rockchip_mpp 1.3.9` / `mtl 26.09` / `json-c 0.15` / `librga 2.1.0` —— 全 `found`，live_ingest 全阶段构建。
- file_playout（TX+RX）：`mtl` / `libavformat 58.45` / `libavcodec 58.91` / `libavutil 56.51` / `json-c` / `rockchip_mpp`（含 **link test: YES**）/ `libdrm` / `alsa 1.2.4` —— 全 `found`。

### 6.2 需修正的问题
1. **应用源码缺 `#include <stdbool.h>`**：交叉环境下 `bool`/`true`/`false` 不再被间接引入（板端原生编译碰巧引入）。已在两个应用约 40 个 `.c` 顶部补 `#include <stdbool.h>`（标准头、幂等、不影响原生构建）。**这是源码改动，建议提交。**
2. **FFmpeg 软链**：sysroot 已有 `libavformat.so.58.91.100` 等，但需未版本化 `libavformat.so`/`libavcodec.so`/`libavutil.so` 软链才能被 `-lavformat` 解析（从板拷贝时符号链接被压平所致，与 MPP/RGA 同源）。已在 sysroot 补齐。

### 6.3 结果（重构前快照，TX+RX 双向时期）
- end2end：23/23，0 错误 → `live_ingest`/`test_pipeline`/`test_io`/`test_codec`/`test_hal`/`test_sps_vui`（ARM aarch64）。
- file_playout：57/57，0 错误 → `file_playout_tx`/`file_playout_rx`/`rx_minimal` 及测试（ARM aarch64；`file_playout_rx` NEEDED 含 libmtl/libasound/libavformat/libavcodec/librockchip_mpp/libdrm）。

### 6.4 角色重构后复验（2026-11-05，本地 wrapper 三件套配置验证）

重构内容见 `docs/plan/rk3588/RK3588_ENCODE_DECODE_ROLE_REFACTOR.md`：`live_ingest` 收为编码发送单入口（删 RX/解码/显示），`file_playout` 收为解码接收单入口（删 TX 发送传输与 `build_tx` 开关，`mp4_demux`/`extradata_handler` 作为解码辅助搬到 `shared/`），`x86` 分支删除，硬件依赖全部必需。`dvs_media` 引用路径随搬移更新（两处）。

验证为配置验证（未执行编译；`PATH` 需含 ARM GNU 13.3 工具链 `bin`，交叉文件用 wrapper 三件套 `/home/kkkqkx/mtl/sysroot/aarch64.ini`）：两包 `meson setup` 均通过。`live_ingest` 8 targets，依赖 `libdrm`/`threads`/`rockchip_mpp 1.3.9`/`librga 2.1.0`/`mtl 26.09.0.DEV`/`json-c 0.15` 全 `found`；`file_playout` 9 targets，`mtl`/`FFmpeg 58.45/58.91/56.51`/`json-c`/`rockchip_mpp`（**link test: YES**）/`libdrm`/`alsa 1.2.4` 全 `found`。另做一次全仓 `files()` 路径与相对 `#include` 可解析检查，结论通过。正式 `ninja` 全量编译与单板冒烟仍在编译服务器执行。

---

## 7. 后续 / 注意事项

1. **板端运行时依赖**：应用运行于 RK3588 时，需板端具备 libmtl.so 及其 NEEDED（DPDK/libbpf/libxdp 已在 sysroot；FFmpeg 的私有依赖 xml2/bz2/gnutls 等由 libavformat.so 自身拉取，板端已具备）。
2. **源码改动待提交**：应用 `stdbool.h` 补 include 是真正的代码改动，建议 commit。
3. **libxdp BPF 对象**：当前未编 `xdp-dispatcher.bpf.o`（运行期默认 dispatcher）；若 RK3588 路线需要，本地 clang 23 已支持 `-target bpf` 可补编。
4. **本地环境再同步（2026-11-05 已补 `mtl` 三件套）**：devs 此后在 sysroot 新增了 libxdp、MTL 安装产物、FFmpeg 软链、numaif.h 补丁；wrapper 系 sysroot（`/home/kkkqkx/mtl/sysroot`）独缺的 `mtl` 三件套已按 §1.3 从 devs `/data/kkkqkx/mtl/sysroot` 同步补齐，后续新增产物沿用同样手动 `rsync`。

---

## 8. 进程管理注意（重要，踩过坑）

- **不要用 `pkill -f <名字>` 按名字杀**：执行该 ssh 命令的远端 shell 自身命令行就含此字符串，`pkill -f` 会把 ssh 会话一并 kill（表现为莫名 `exit 255`）。改用 PID 文件（`setsid ... & echo $! > /tmp/x.pid` + `kill $(cat /tmp/x.pid)`）或 `pkill -f "[n]inja -C build"`（`[n]` 让模式不匹配自身/ssh 命令行）。后台构建务必 `setsid ... < /dev/null &` 脱离会话。
