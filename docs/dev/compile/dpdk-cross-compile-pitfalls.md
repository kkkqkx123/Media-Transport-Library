# DPDK 26.07 aarch64 交叉编译踩坑记录（devs / RK3588）

记录 `dpdk-26.07` 在编译服务器 `devs`（Ubuntu 18.04 x86_64，项目 `/data/kkkqkx/mtl`）上为 RK3588 交叉编译时遇到的全部问题、根因与最终可用方案。DPDK 已成功构建（729/729，0 错误）并安装进 sysroot。

配套阅读：`docs/dev/compile/5.10-debian-cross-compile.md`（应用层交叉编译总流程，下称「应用文档」）。**本文档取代应用文档 §6.1 的交叉文件写法**——那里直接直连 `gcc`/`pkg-config`，在本机实测会因 buildroot 的 pkg-config 泄漏与 meson 逗号解析问题而失败。

---

## 0. 环境与最终可用方案

| 项 | 值 |
|---|---|
| 编译机 | `devs`，Ubuntu 18.04 x86_64，项目根 `/data/kkkqkx/mtl` |
| 工具链 | ARM GNU Toolchain `aarch64-none-linux-gnu-gcc` 13.3（x86 宿主、aarch64 目标），`/data/kkkqkx/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu` |
| meson/ninja | buildroot host 版：`/data/kkkqkx/rk3588bsp/buildroot/output/rockchip_rk3588/host/bin/` |
| sysroot | `/data/kkkqkx/mtl/sysroot`（从 RK3588 板导出，glibc 2.31） |
| DPDK | `/data/kkkqkx/mtl/dpdk-26.07`，安装到 sysroot（`DESTDIR=sysroot`，`--prefix=/usr`） |

**最终可用的核心思路**（三件套，全部位于 `/data/kkkqkx/mtl/bin` 与 `sysroot/aarch64.ini`）：

1. **gcc/g++ 包装脚本**（`aarch64-cc-wrap` / `aarch64-cxx-wrap`）：在每次编译/链接末尾自动追加 `-Wl,-rpath-link,$SYSROOT/.../aarch64-linux-gnu`（各两份，覆盖 `usr/lib` 与 `lib`）和 `-lm`。  
   → 解决了「传递 NEEDED 依赖解析不到」（libelf→zlib / libbsd→libmd / libpcap→dbus）与「rte_sched 需要 libm」两个问题。
2. **专用 pkg-config 包装**（`mtl-pkg-config`）：强制 `PKG_CONFIG_SYSROOT_DIR`/`PKG_CONFIG_LIBDIR` 指向本仓库 sysroot，杜绝 buildroot 的 pkg-config 把 buildroot sysroot 的库（如 libbsd）泄漏进构建。
3. **交叉文件 `aarch64.ini`** 的 `[binaries]` 指向上述包装脚本，并在 `[properties]` 提供 `platform = 'generic'` 与 sysroot 相关 `c_args`/`c_link_args`。

> 为什么用包装脚本而不是把 `-Wl,-rpath-link` 写进交叉文件 `c_link_args`？因为 **meson 的交叉文件数组解析器会按逗号切分数组元素**，而 `-Wl,-rpath-link,/abs/path` 内部带逗号，会导致这些元素被整段丢弃（前 4 个无逗号的元素 `--sysroot/-B/-L/-L` 正常保留，后面的 `-rpath-link`/`-lm` 全部消失）。详见坑 P6。

---

## 1. 关键文件（当前生效版本，可直接复现）

### 1.1 交叉文件 `sysroot/aarch64.ini`

```ini
[binaries]
c = '/data/kkkqkx/mtl/bin/aarch64-cc-wrap'
cpp = '/data/kkkqkx/mtl/bin/aarch64-cxx-wrap'
ar = 'aarch64-none-linux-gnu-ar'
strip = 'aarch64-none-linux-gnu-strip'
pkg-config = '/data/kkkqkx/mtl/bin/mtl-pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'

[properties]
platform = 'generic'
c_args = ['--sysroot=/data/kkkqkx/mtl/sysroot', '-isystem/data/kkkqkx/mtl/sysroot/usr/include/aarch64-linux-gnu']
c_link_args = ['--sysroot=/data/kkkqkx/mtl/sysroot', '-B/data/kkkqkx/mtl/sysroot/usr/lib/aarch64-linux-gnu', '-L/data/kkkqkx/mtl/sysroot/usr/lib/aarch64-linux-gnu', '-L/data/kkkqkx/mtl/sysroot/lib/aarch64-linux-gnu']
cpp_args = c_args
cpp_link_args = c_link_args
```

### 1.2 gcc 包装 `/data/kkkqkx/mtl/bin/aarch64-cc-wrap`

```sh
#!/bin/sh
REAL=/data/kkkqkx/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc
SYSROOT=/data/kkkqkx/mtl/sysroot
exec "$REAL" "$@" -Wl,-rpath-link,"$SYSROOT/usr/lib/aarch64-linux-gnu" -Wl,-rpath-link,"$SYSROOT/lib/aarch64-linux-gnu" -lm
```

### 1.3 g++ 包装 `/data/kkkqkx/mtl/bin/aarch64-cxx-wrap`

```sh
#!/bin/sh
REAL=/data/kkkqkx/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-g++
SYSROOT=/data/kkkqkx/mtl/sysroot
exec "$REAL" "$@" -Wl,-rpath-link,"$SYSROOT/usr/lib/aarch64-linux-gnu" -Wl,-rpath-link,"$SYSROOT/lib/aarch64-linux-gnu" -lm
```

### 1.4 pkg-config 包装 `/data/kkkqkx/mtl/bin/mtl-pkg-config`

```sh
#!/bin/sh
export PKG_CONFIG_SYSROOT_DIR=/data/kkkqkx/mtl/sysroot
export PKG_CONFIG_LIBDIR=/data/kkkqkx/mtl/sysroot/usr/lib/pkgconfig:/data/kkkqkx/mtl/sysroot/usr/share/pkgconfig:/data/kkkqkx/mtl/sysroot/usr/lib/aarch64-linux-gnu/pkgconfig
exec /usr/bin/pkg-config "$@"
```

> 三个包装脚本都 `chmod +x`。gcc/g++ 包装把 `-lm` 追加在末尾：对 `-c` 纯编译无害（链接标志被忽略），对链接则位于所有目标文件之后，`--as-needed` 会保留被引用的 `-lm`（rte_sched 的 `pow`/`log2` 因此被满足）。

---

## 2. 踩坑与修复（按排查顺序）

| ID | 现象 | 根因 | 修复 |
|---|---|---|---|
| P1 | 编译报 `<bits/pthread.h>` / `pthread.h` 宏重定义、结构体冲突 | 工具链 `include-fixed/pthread.h` 与板端 sysroot 的 glibc 2.31 `pthread.h` 冲突（工具链自带的一份覆盖了 sysroot 的） | 重命名/移走工具链自带 `include-fixed` 目录（`.../aarch64-none-linux-gnu/include-fixed`），让 sysroot 头文件优先 |
| P2 | `pmdinfogen.py` 报 `ImportError` / `AttributeError` | DPDK 26.07 自带脚本 import 了 buildroot 环境的 `elftools`（chromium 改版），与不兼容的 py3.11 冲突 | 换成标准 `pyelftools 0.31`（`pip3 install pyelftools==0.31`，用其 `elftools`），该脚本即正常 |
| P3 | `bpf`/`sched` 链接报 `libelf.so: undefined reference to inflate/deflate` | sysroot 里 `libelf.a`/`libpcap.a` 静态库被 `find_library` 选中，链接器把 `.so` 当普通输入对象，其 NEEDED（zlib/dbus）不被拉取 | 强制**共享链接**：删掉所有 `*.a`（保留无 `.so` 对应物的 glibc 静态辅助归档，见 P5），并补 `libfoo.so -> libfoo.so.N` 软链 |
| P4 | `RTE_USE_LIBBSD 1` 被打开，构建混入 buildroot 的 libbsd | `devs` 的 `PATH` 里 `pkg-config` 是 buildroot 包装，忽略 `PKG_CONFIG_*` 环境变量，指向 buildroot sysroot；DPDK 经 pkg-config 拾取其 libbsd | 写专用 `mtl-pkg-config` 强制锁定本仓库 sysroot（§1.4），交叉文件 `[binaries] pkg-config` 指向它 |
| P5 | 删 `.a` 后报 `__libc_csu_init` / `__stack_chk_fail` 等未定义 | 误删了**无 `.so` 对应物的 glibc 静态辅助归档**：`libc_nonshared.a`、`libg.a`、`libmcheck.a`、`librpcsvc.a`、`libffi_pic.a`、`libmali-hook-injector.a` 等 | 从板 `ssh board 'cd /usr/lib/aarch64-linux-gnu && tar cf - ...' \| ssh devs 'tar xf -'` 恢复这批归档到 sysroot |
| P6 | 交叉文件里写的 `-Wl,-rpath-link,...` 完全没进 `build.ninja` | meson 交叉文件数组按逗号切分元素，带逗号的 `-Wl,-rpath-link,/path` 被整段丢弃 | 改用 gcc/g++ **包装脚本**注入（§1.2/§1.3），绕开交叉文件数组解析 |
| P7 | 传递 NEEDED 依赖解析不到：`libelf.so` 需 `libz`，`libbsd.so` 需 `libmd`，`libpcap.so` 需 `libdbus-1` | 用 `--sysroot` 时链接器默认只在 `$SYSROOT/lib`、`$SYSROOT/usr/lib`（非 multiarch）找 NEEDED；multiarch 目录要靠 `-rpath-link` 显式指定 | 包装脚本追加 `-Wl,-rpath-link,$SYSROOT/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$SYSROOT/lib/aarch64-linux-gnu` |
| P8 | `librte_sched.so` 报 `undefined reference to pow/log2` | `rte_sched` 用 `pow`/`log2`，DPDK 不为该库加 `-lm` | 包装脚本追加 `-lm`（末尾追加，被 `--as-needed` 保留） |
| P9 | 不提供 libbsd 时 DPDK 仍要 `ext_deps += libbsd` | DPDK `lib/meson.build` 无条件把 libbsd 加入 ext_deps，缺它直接 configure 失败 | 在 sysroot 提供 `libbsd-dev`（头+`.so`+`.pc`）+ `libmd`（libbsd 的 NEEDED，靠 P7 的 `-rpath-link` 拉取） |
| P10 | configure 报 `Arm SoC must be specified in the cross file` | DPDK arm 交叉构建从交叉文件 `[properties] platform` 读 SoC，缺则报错（注意：不是命令行 `-Dplatform`） | 交叉文件 `[properties]` 加 `platform = 'generic'` |
| P11 | configure 报 `Compiler does not support "cortex-a76" arch flag` | 该工具链不接受把 `cortex-a76` 当 `-march` 值（它是 `-mcpu` 值） | 交叉文件 `[host_machine] cpu = 'armv8-a'`（与 `-Dcpu_instruction_set=armv8-a` 一致） |

### 2.1 sysroot 静态库清理要点（P3 / P5）

- **可删**：凡有 `.so` 对应物的 `lib*.a`（如 `libelf.a`、`libpcap.a`）—— 逼 DPDK 走共享链接。
- **必留**：无 `.so` 对应物的 glibc 辅助归档（`libc_nonshared.a`、`libg.a`、`libmcheck.a`、`librpcsvc.a`、`libffi_pic.a`、`libmali-hook-injector.a` 等），否则链接期缺失符号。
- **补软链**：对每个 `libfoo.so.N` 补 `libfoo.so -> libfoo.so.N`，保证 `-lfoo` 能解析。
- 清理后务必用 P5 的命令把误删的辅助归档从板恢复。

### 2.2 验证传递依赖是否被满足（P7 的快速自检）

```bash
# 最小用例：仅 -lelf（无 -rpath-link）必失败；加 -rpath-link 必成功
echo 'int main(){return 0;}' > /tmp/t.c
TOOL=/data/kkkqkx/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu/bin
SYS=/data/kkkqkx/mtl/sysroot
# 失败样例（应报 libz.so.1 not found）：
$TOOL/aarch64-none-linux-gnu-gcc --sysroot=$SYS -L$SYS/usr/lib/aarch64-linux-gnu -L$SYS/lib/aarch64-linux-gnu -lelf /tmp/t.c -o /tmp/t 2>&1 | head
# 成功样例（加 -rpath-link，等价于包装脚本的效果）：
$TOOL/aarch64-none-linux-gnu-gcc --sysroot=$SYS -L$SYS/usr/lib/aarch64-linux-gnu -L$SYS/lib/aarch64-linux-gnu \
  -Wl,-rpath-link,$SYS/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$SYS/lib/aarch64-linux-gnu -lelf /tmp/t.c -o /tmp/t && echo OK
```

---

## 3. 进程管理注意（重要，踩过坑）

- **不要用 `pkill -f <进程名>` 按名字杀进程**。执行该 ssh 命令的远端 shell 自身命令行里就含有这个字符串，`pkill -f` 会把它自己也匹配上，从而把 **ssh 会话进程一并 kill**，表现为莫名 `exit 255` / 连接断开。
- 推荐做法：
  - 启动时保存 PID：`setsid ... & echo $! > /tmp/dpdk_ninja.pid`，需要时 `kill $(cat /tmp/dpdk_ninja.pid)`；
  - 或 `pkill -f "[n]inja -C build-aarch64"`（用 `[n]` 让模式本身不匹配字面量，既不自杀也不误伤 ssh shell）；
  - 轮询存活用 `pgrep -af "[n]inja -C build-aarch64"`（同样加 `[n]`）。
- 后台构建务必 `setsid ... < /dev/null &` 脱离会话，避免 ssh 断开导致进程被 SIGHUP。

---

## 4. 复现构建命令

```bash
cd /data/kkkqkx/mtl/dpdk-26.07
export REPO=/data/kkkqkx/mtl
export SDK=/data/kkkqkx/rk3588bsp
export TOOL=/data/kkkqkx/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu
export PATH=$TOOL/bin:$SDK/buildroot/output/rockchip_rk3588/host/bin:$PATH
CROSS=$REPO/sysroot/aarch64.ini

# 注意：pkill 用 [n] 模式，避免误伤 ssh
pkill -f "[n]inja -C build-aarch64" 2>/dev/null; sleep 1
rm -rf build-aarch64

MESON="$SDK/buildroot/output/rockchip_rk3588/host/bin/python3.11 $SDK/buildroot/output/rockchip_rk3588/host/bin/meson"
$MESON setup build-aarch64 --cross-file $CROSS \
  -Dplatform=generic -Dcpu_instruction_set=armv8-a \
  -Denable_drivers=net/null,bus/pci,bus/vdev,common/iavf \
  -Dtests=false -Dexamples= --prefix=/usr

# 后台构建 + 保存 PID
setsid $SDK/buildroot/output/rockchip_rk3588/host/bin/ninja -C build-aarch64 >/data/kkkqkx/mtl/dpdk_build.log 2>&1 < /dev/null &
echo $! > /tmp/dpdk_ninja.pid

# 轮询（每 ~60s 心跳，避免 idle 超时）
for i in $(seq 1 400); do sleep 60; echo "[$(date +%H:%M:%S)] tick $i";
  tail -3 /data/kkkqkx/mtl/dpdk_build.log;
  pgrep -f "[n]inja -C build-aarch64" >/dev/null || { echo BUILD_DONE_OR_GONE; break; }; done

# 安装到 sysroot（libdpdk.pc 落在 sysroot/usr/lib/pkgconfig，库落在 sysroot/usr/lib/aarch64-linux-gnu）
DESTDIR=$REPO/sysroot ninja -C build-aarch64 install

# 校验 DPDK 可被 pkg-config 发现
/data/kkkqkx/mtl/bin/mtl-pkg-config --modversion libdpdk   # 期望 26.07.0
```

> `-Denable_drivers=...` 按需裁剪；本仓库 RK3588 软件 pacing 路线不需要 ICE/iavf 硬件限速，保留 `null/pci/vdev/iavf` 即可。`--prefix=/usr` 配合 `DESTDIR=sysroot` 把库/头/`.pc` 装到 `sysroot/usr` 下，供后续 MTL 经 pkg-config 拾取。

---

## 5. 与应用文档（5.10）的差异说明

应用文档 §6.1 的交叉文件写法是：

```ini
c = 'aarch64-none-linux-gnu-gcc'
cpp = 'aarch64-none-linux-gnu-g++'
pkg-config = 'pkg-config'
```

**本方案已取代它**，原因：

1. `pkg-config = 'pkg-config'` 直连会被 buildroot 的 pkg-config 包装劫持，泄漏 buildroot sysroot（P4）；必须用 `mtl-pkg-config` 锁死本仓库 sysroot。
2. 直接 `aarch64-none-linux-gnu-gcc` 无法注入 `-rpath-link`/`-lm`（P6/P7/P8）；必须用 gcc/g++ 包装脚本。

其余（sysroot 绝对软链改相对、pkg-config 三件套、`-B`/`-isystem` 处理非 Debian 多 arch 感知工具链等）仍按应用文档 §4.2 / §6.2 执行，保持不变。
