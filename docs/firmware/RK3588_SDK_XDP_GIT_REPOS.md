# RK3588 编译服务器上的两个 SDK 仓库：rk3588_5.1_opensourceSDK 与 rk3588_5.1_XDP

调查对象：bingo 编译服务器（`ssh bingo`，bingo-MS-7996，Ubuntu 20.04 / 5.15 内核）上的
`/home/bingo/rk3588_5.1_opensourceSDK` 与 `/home/bingo/rk3588_5.1_XDP`。

> **6.1 移植（2026-10-24）**：同一套 AF_XDP/eBPF 内核使能已结构化移植到 devs 的
> `/data/kkkqkx/rk3588bsp`（RK3588 Linux 6.1.118 SDK）：新增 `kernel-6.1/arch/arm64/configs/ztl_defconfig`
> 与 `ztl_rk3588.config`、改 `Config.in.kernel` 默认值、DTS 禁 i2c4，已提交 `98e7f9606`（未 push），
> patch 存档 `patches/rk3588/0001-kernel-6.1-enable-af_xdp-ebpf-for-mtl.patch`（基于基线 `ab0040897` 重新生成，单文件双提交 mbox：AF_XDP/eBPF 提交 + 2026-10-26 构建修复——关闭 `CONFIG_STMMAC_UIO`；Mali CSF 固件被 gitignore 不进 patch，需编译前手动复制，见构建指南 §2）。
> 5.1 patch **不能**直接 apply 到 6.1（路径/上下文均变），移植判定与 olddefconfig 校验见
> `../analysis/RK3588_6.1_XDP_PORT.md`。

> **后续分析工作副本（2026-10-24 起生效）**：devs 编译服务器 `/data/kkkqkx/rk3588_5.1_XDP`
> （本地 `ssh devs` = itc@10.10.20.116，/data 剩 7.7T）。迁移方式：在 bingo 上
> `git bundle create --all`（4.99G 单文件，含 master/list 全部历史）→ 以本地开发机为中枢
> **两跳 rsync 中转**（每跳 `--partial` 断点续传、两端 md5 校验一致）→ devs 上
> `git bundle verify` + `git clone` 展开（247,818 文件 / 18G，属主 itc，无需 safe.directory），
> 再 `git apply xdp_uncommitted.patch` + 解出 `xdp_aux.tar`：5 处未提交改动、`D envsetup.sh`、
> `.org` 备份、`package-file`、SDK 符号链接、u-boot 散件与 9 个嵌套 `.git` 残壳完整镜像，
> `git status`/`git diff` 与旧树一致。**有意不同步构建产物**：`output/`（14G）、`rockdev`、
> `rootfs_rk3588_ubuntu20.04_gnome.img`（6.5G）留在 bingo 备份内。
> 备份链：`/data/kkkqkx/stage/rk3588_5.1_XDP.bundle`（便携单文件备份）+
> 旧树 `/home/bingo/rk3588_5.1_XDP`（含产物，只读取证需 safe.directory）。曾建又删除的
> bingo 本地副本与 SSH 隧道/agent 方案均已清理，不再使用。新副本 `origin` 指向 bundle 文件，
> 后续 push 请配置真实远端 URL。
配套阅读：`docs/analysis/AF_XDP_EBPF.md`（§6 启用清单）、`docs/dev/01-MTL编译体系分析.md`、`docs/dev/02-x86到RK3588交叉编译分析.md`。
调查时间：2026-10-24（只读取证，未修改服务器上任何文件；仅补了 `git config --global safe.directory` 例外，因目录属主与登录用户不一致，git 默认拒读）。

## 0. 一句话结论

两棵树是**同一份 Rockchip Linux-5.1 SDK（内核 5.10.226，ZTL/CM588 RK3588 核心板）的"基线 + 副本"关系**：`rk3588_5.1_opensourceSDK` 是 2026-02 由 ztl 做的本地 git 快照基线（顶层 4 条提交、无远程、工作区跟踪文件零修改）；`rk3588_5.1_XDP` 是 2026-05-11 以 root 整树拷贝出的副本（git 历史逐字节相同），在副本上做了 **AF_XDP/eBPF 内核使能**（ztl_defconfig 开 `XDP_SOCKETS/BPF_SYSCALL/BPF_JIT/DEBUG_INFO_BTF` 等）并完成了内核构建与 update.img 打包——但这些改动**至今未提交、两树均无 remote**，只存在于该服务器磁盘上。

## 1. 总览

| 项 | rk3588_5.1_opensourceSDK | rk3588_5.1_XDP |
|---|---|---|
| 路径 | `/home/bingo/rk3588_5.1_opensourceSDK` | `/home/bingo/rk3588_5.1_XDP` |
| 属主 | uid 1001（非 bingo，需 safe.directory） | root（以 sudo 操作） |
| 创建/拷贝时间 | 顶层 `.git` 最后写于 2026-02-06 | 目录 2026-05-11 14:28；`.git` 2026-05-27（index 刷新，无新提交） |
| 分支 | `master`（仅一个） | `master` + `list`（`list` 与 `master` 指向同一提交，历史遗留） |
| 提交数 | 4 | 4（哈希完全相同） |
| HEAD | `40733b7b2` "SDK ADD: cm588 board build part"（ztl，2026-02-05） | 同左 |
| remote | **无** | **无** |
| tag / stash | 无 | 无 |
| `.git` 体积 | 4.7G（基线提交含 24.7 万文件 / 1 亿行，编译产物也在内） | 4.7G |
| 跟踪文件修改 | 0 处（仅 ` D envsetup.sh`） | 5 处（见 §5） |
| 内核版本 | 5.10.226（Rockchip BSP，"5.1" 即 Linux 5.1 系列） | 5.10.226 |
| 内核构建痕迹 | `output/` 符号链接 2026-05-09；rootfs img 2026-02-06 | `kernel/.config` 2026-05-27 11:58；`output/firmware/update.img` 2026-05-27 13:43 |

## 2. 顶层 git 历史（两树相同）

```
40733b7b2 2026-02-05  SDK ADD: cm588 board build part     (parameter-rk3588-ztl.txt、
                                                           rockchip_rk3588_ztl_cm588_defconfig、
                                                           Config.in.kernel；reflog 显示曾 amend)
43419db64 2026-02-04  KERNEL: add cm588 board dts          (ztl-rk3588-cm.dts，695 行)
4cc30afe1 2026-02-04  KERNEL: add config file              (ztl_defconfig 950 行 + ztl_rk3588.config)
340029f3a 2026-02-04  SDK INIT: init sdk source code ...   (247,813 文件 / 100,772,559 行 全量快照)
```

这不是 `repo` 工具管理的多仓 SDK，而是把整棵 Rockchip SDK（kernel、u-boot、buildroot、debian、
yocto、rtos、hal、rkbin、external…）`git init` + 一次 add 全部提交的**单仓快照**，随后在快照上
叠了 3 条板级支持提交。提交基线时内核已编译过（`r8168/*.o`、`built-in.a` 等产物被一并提交），
这也是 `.git` 达 4.7G 的原因；之后在基线树上重新编译**不会**体现为 git 修改——分析两树差异时
要记住"跟踪文件未变 ≠ 没构建过"。

### 嵌套 `.git` 残留

`kernel/`、`u-boot/`、`buildroot/`、`hal/`、`rkbin/`、`rtos/`、`debian/`、`uefi/`、`tools/` 下各有一个
`.git` 目录（含 `HEAD`/`index`/`packed-refs`/`rr-cache`/`svn`，`kernel/.git/HEAD` 指向 `792a7d42…`），
是厂商 SDK 发布前用 repo/git-svn 拉取时留下的**残壳**：`objects/pack` 缺失，git 判
`fatal: not a git repository`。因此它们**不参与版本控制**，`kernel/` 等目录的文件由顶层单仓直接跟踪
（`git ls-tree HEAD kernel/arch/arm64/configs/` 可验证）。两树的残壳完全一致，进一步佐证"整树拷贝"。

## 3. rk3588_5.1_opensourceSDK：基线树状态

- 工作区：`## master`，仅 1 处跟踪文件差异 ` D envsetup.sh`；untracked 为构建/发布产物与 SDK 自带
  符号链接：`output/`、`rockdev -> output/firmware`、`README.md`、`common`（这两个是指向
  `device/rockchip/common/` 的软链，git init 时未被 add）、`rootfs_rk3588_ubuntu20.04_gnome.img`
  （6.9G，2026-02-06）、`u-boot/bl31_*.digest/.gz`、`u-boot/tee.bin.*`、`u-boot/examples/standalone/rkspi`。
- 结论：**2 月做完快照后没有再产生任何已跟踪内容的修改**；它是"发板级（CM588）→ 出 rootfs/update.img"
  的原始工作树，5 月仍被用于构建（firmware 符号链接 2026-05-09），但构建的是基线内核。

## 4. rk3588_5.1_XDP：XDP 使能副本状态

- git 身份与 OSDK 完全相同（同 4 条提交、同 HEAD、同 `.git` 体积、untracked 集合是 OSDK 的超集）
  ——即 `cp -a`（且保留 root 属主）级别的整树拷贝，创建时间 2026-05-11。
- 工作区新增 5 处跟踪文件修改（`git diff --stat`：6 files, 236+/307-）与 3 处新 untracked：

| 文件 | 改动 | 用途 |
|---|---|---|
| `kernel/arch/arm64/configs/ztl_defconfig` | 重写（533 行）：按 savedefconfig 风格并入原尾部 "#for docker/#for k3s/#for iptables" 补丁段，**新增 AF_XDP/eBPF 关键项**：`CONFIG_XDP_SOCKETS=y`、`CONFIG_XDP_SOCKETS_DIAG=y`、`CONFIG_BPF_SYSCALL=y`、`CONFIG_BPF_JIT=y`、`CONFIG_BPF_JIT_ALWAYS_ON=y`、`CONFIG_BPF_UNPRIV_DEFAULT_OFF=y`、`CONFIG_DEBUG_INFO_BTF=y`、`CONFIG_CGROUP_BPF=y`、`CONFIG_BPFILTER=y`、`CONFIG_BPF_STREAM_PARSER=y`、`CONFIG_HUGETLBFS=y`；并大段补入 nftables/iptables/wireguard/bonding/tun/veth/CAN/MD-RAID/`CONFIG_STMMAC_UIO=y` 等；删除了 `CONFIG_SYN_COOKIES`、`CONFIG_ARM64_CRYPTO`、`CONFIG_VT_HW_CONSOLE_BINDING`、`CONFIG_CRYPTO_SHA512` 等个别旧项 | 满足 `docs/analysis/AF_XDP_EBPF.md` §6 第 1 步的内核 CONFIG 前提（含 BTF，供 CO-RE/`mtl.xdp.o`） |
| `kernel/tools/bpf/bpftool/Makefile` | +1 行 `$(warning 警告目标: $(CC))` | 交叉编译 bpftool 时排查工具链；留有 `Makefile.org` 备份 |
| `kernel/arch/arm64/boot/dts/rockchip/ztl-rk3588-cm.dts` | `&i2c4 { status = "disabled"; };` | 板级调试（留有 `ztl-rk3588-cm.dts.org` 备份） |
| `device/rockchip/.chips/rk3588/rockchip_rk3588_ztl_cm588_defconfig` | +`RK_PACKAGE_FILE_CUSTOM=y` | 配合新 untracked 的 `package-file`，自定义打包分区表 |
| `device/rockchip/common/scripts/mk-updateimg.sh` | +1 行 echo 调试 | 排查 update.img 打包 |
| `envsetup.sh` | 删除（与基线树一致） | — |

- **构建实证**：`kernel/.config`（2026-05-27 11:58）实测含
  `CONFIG_XDP_SOCKETS=y / CONFIG_XDP_SOCKETS_DIAG=y / CONFIG_BPF_SYSCALL=y / CONFIG_BPF_JIT=y / CONFIG_DEBUG_INFO_BTF=y`，
  `net/xdp/` 下 `xsk.c/xdp_umem.c/xsk_buff_pool.c` 均已编译出 `.o/.a`；`output/firmware/` 全套镜像
  符号链接 2026-05-27（`update.img` 13:43 打包完成）。即这棵树**确实构建并打包过带 AF_XDP 的内核**。

## 5. 彼此的关系

```
Rockchip Linux-5.1 SDK (kernel 5.10.226, ZTL CM588)
        │  2026-02-04 git init + 全量快照 + 3 条板级提交（作者 ztl，本地无远程）
        ▼
rk3588_5.1_opensourceSDK ──── 基线树：工作区不再改动，继续用于基线构建（2026-05）
        │  2026-05-11 以 root 整树拷贝（含 .git 与构建产物、嵌套 .git 残壳）
        ▼
rk3588_5.1_XDP ──── XDP 实验树：defconfig 开 AF_XDP/BPF/BTF + bpftool/dts/打包改动（未提交），
                    2026-05-27 构建内核并打包 update.img
```

- 命名中的 **"5.1"** = Rockchip Linux SDK 5.1 系列（内核 5.10）；**XDP** = 该副本的目的——为
  MTL 的 `native_af_xdp:` 后端准备内核。
- 两树共享同一 git 历史意味着：XDP 树上 `git diff` 的输出**恰好就是**"XDP 使能改动"本身，
  与 OSDK 的差异不掺历史噪音；反向对比（OSDK vs XDP 工作区）也只差 §4 那张表 + 3 个 `.org`/`package-file`。
- 没有任何机制保证两树同步：OSDK 若继续改动（如再提交基线修复），XDP 的 diff 基线就会漂移。

## 6. 对 MTL AF_XDP 集成（docs/analysis/AF_XDP_EBPF.md §6）的意义与缺口

已满足（XDP 树内核）：

- §6 步骤 1 的 CONFIG 前提全部打开，且 `DEBUG_INFO_BTF=y` 支持 `mtl.xdp.o` 的 CO-RE/`bpftool` 路线；
- `HUGETLBFS`、`CGROUP_BPF`、XDP diag 齐备；`CONFIG_R8168=y` 确认 r8168 厂商驱动在树内。

**新发现的缺口（原 §6 风险表假设偏乐观）**：

| 实测 | 影响 |
|---|---|
| `stmmac_main.c`（gmac0/gmac1 → `eth0`）**无任何 XDP/`ndo_bpf` 代码**（grep 0 命中，目录下无 xdp 文件） | eth0 不能 native 模式挂 XDP；libxdp 会退 generic(SKB) 模式，MTL `XDP_ZEROCOPY` 必失败、退 copy UMEM |
| r8168 厂商驱动源码 **0 处 XDP 引用**（TX 口 `enP3p49s0`） | 同上：只能 generic XDP + copy 模式，RX 路径每包过协议栈，吞吐/时延显著打折 |
| 内核内 grep `tx_max_rate` 无命中（原命令属性名写错：内核通用属性实为 **`tx_maxrate`**，在 `net-sysfs.c`，grep 应搜 `tx_maxrate`） | `MT_RL_TYPE_XDP_QUEUE_SYSFS` 仍不可用：MTL 只在驱动名为 `ice` 时才做该探测（`mt_af_xdp.c:792`，且探测写死 `tx-1`）。pacing 只能 TSC 软件方式——与既定结论（req/02 §2.2 R6）一致 |
| **板上实测（2026-10-24，`docs/env/RK3588_BOARD_ENV.md`）与上两行假设不符**：活动网口不是 stmmac/r8168，而是 **Motorcomm yt6801**（eth0=`enP2p33s0`、eth1=`enP3p49s0`，PCIe builtin 驱动）；`ip link set ... xdpdrv` 被明确拒绝（`Underlying driver does not support XDP in native mode`），`xdpgeneric` 可用；手工写 `tx-0/tx_maxrate` 返回 `EOPNOTSUPP` | 结论方向不变（只能 generic XDP + copy 模式、无队列限速），但**驱动对象与属性名需按实测更正** |

即：**内核侧（socket 机制 + BPF 工具链）已就绪，驱动侧（`ndo_bpf`）两个板载网卡都缺**。板上最小验证
（AF_XDP_EBPF.md §6）应预期以 generic XDP/copy 模式跑通功能，性能验证需另寻支持 XDP 的网卡或补驱动。

## 7. 风险与建议

1. **改动零备份**：XDP 使能的 5 处修改未提交、无 remote、`list` 分支未被利用。建议立即在 XDP 树
   `git add` 相关改动并提交（`.org` 备份与 `package-file` 一并纳入或写 `.gitignore`），再 push 到
   内部 git 服务——否则磁盘故障即失去整个 AF_XDP 内核配置。
2. **提交时剔除产物噪音**：基线把 `.o/built-in.a/output/` 全量入库，后续提交建议只 add 源码/配置，
   并考虑给 SDK 树补 `.gitignore`（`output/`、`*.o`、`*.digest`、`*.gz`、rootfs img）。
3. **防基线漂移**：要么把 XDP defconfig 改动回合进 opensourceSDK 形成单一基线，要么在 XDP 树提交后
   冻结 OSDK；两树并行修改会让 `git diff` 不再等价于"XDP 改动清单"。
4. **驱动缺口跟进**：如需真正的 native/zerocopy AF_XDP，评估给 rk gmac（stmmac）补 `ndo_bpf`
   （上游 stmmac 的 XDP 实现可移植）或换用支持 XDP 的 PCIe/USB 网卡；短期接受 generic 模式做功能验证。
5. 嵌套 `.git` 残壳无害但易误导（`git -C kernel` 会报 dubious/not-a-repo），可在提交清理时顺手删除，
   或留 README 注明"由顶层单仓管理"。

## 附：取证命令（服务器侧，全部只读）

```bash
git remote -v / branch -a / log --oneline / rev-list --count HEAD
git rev-list --max-parents=0 --format="%h %ad %s" HEAD        # 根提交 340029f3a
git status -sb                                                  # OSDK 12 行 / XDP 20 行
git show --stat 4cc30afe1 43419db64 40733b7b2 340029f3a | tail -1
git --no-pager diff --stat                                      # XDP 树 6 文件
grep -E "XDP_SOCKETS|BPF_SYSCALL|BPF_JIT|DEBUG_INFO_BTF" kernel/.config
grep -rc "xdp" kernel/drivers/net/ethernet/stmicro/stmmac/stmmac_main.c \
     kernel/drivers/net/ethernet/realtek/r8168/r8168_n.c        # 均为 0
ls kernel/net/xdp/ ; grep -rl tx_maxrate kernel/net/core/net-sysfs.c kernel/drivers/net  # 属性名是 tx_maxrate（旧记录误写作 tx_max_rate）
```
