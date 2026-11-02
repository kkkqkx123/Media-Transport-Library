# RK3588 部署指南（MTL + rk3588 应用）

> 最后更新：2026-11-02
> 适用：把交叉编译产物部署到 RK3588 开发板（aarch64，Debian 11 / glibc 2.31），运行 ST2110 管线。
> 构建侧详情见 `docs/archive/RK3588_CROSS_COMPILE_PROGRESS.md`。

---

## 1. 产物清单（需部署的文件）

| 类别 | 文件 | 来源（devs） | 说明 |
|---|---|---|---|
| MTL 库 | `libmtl.so` | `sysroot/usr/lib/libmtl.so`（release，`meson install --prefix=/usr`） | soname `libmtl.so` |
| AF_XDP 后端 | `libxdp.so.1.6.0` + 软链 `libxdp.so.1`/`libxdp.so` | `sysroot/usr/lib/aarch64-linux-gnu/` | 动态链同一个 `libbpf` |
| BPF 库 | `libbpf.so.1.4.0` + 软链 `libbpf.so.1` | `sysroot/usr/lib/aarch64-linux-gnu/` | 与 MTL 内 DPDK 用的 libbpf 1.4.0 一致 |
| DPDK 运行时 | `librte_*.so*` 全部 | `sysroot/usr/lib/librte_*.so*` | DPDK 26.07 共享库（`libmtl.so` 的 NEEDED） |
| 应用 | `e2e_app` `mp4_dist_tx` `mp4_dist_rx` `rx_minimal` + `test_*` | `app/rk3588_{end2end,mp4_dist}/build/` | 各为独立可执行文件 |
| 配置 | `config/`（mp4_dist 的 `ptp/`、`*.json.example`） | `app/rk3588_mp4_dist/config/` | 运行时以 `--config config/xxx.json` 传入（相对路径） |

> **注意**：板端**已具备** `libnuma` `libjson-c` `libasound` `librockchip_mpp` `librga` `libdrm` `libavformat/codec/util` `libelf` `libbsd` 等；缺的只有上表的 MTL / XDP / BPF / DPDK 全家桶。

---

## 2. 板端正式部署位置

### 2.1 库 → 系统库目录（推荐，免设 `LD_LIBRARY_PATH`）
```
/usr/lib/aarch64-linux-gnu/
  libmtl.so
  libxdp.so.1.6.0  libxdp.so.1  libxdp.so
  libbpf.so.1.4.0  libbpf.so.1
  librte_*.so  librte_*.so.26  librte_*.so.26.*   (全部 DPDK 共享库)
```
部署后执行：
```bash
ldconfig
```

### 2.2 应用 → 独立目录（隔离、易卸载）
```
/opt/mtl-rk3588/
  bin/      e2e_app  mp4_dist_tx  mp4_dist_rx  rx_minimal  test_*
  config/   (复制 app/rk3588_mp4_dist/config/)
```
应用以 `--config` 读取相对 `config/` 的 JSON，因此在该目录下运行：
```bash
cd /opt/mtl-rk3588
./bin/mp4_dist_tx 300 --config config/tx_dual_host.json --fps 25
```

> 也可改为把二进制装到 `/usr/local/bin/`、配置装到 `/usr/local/share/mtl-rk3588/config/`，但需改用绝对 `--config` 路径。

---

## 3. 本机（构建中继/归档）release 目录

正式产物（**仅最终文件，不含 `build-aarch64/` 中间产物**）归档于：
```
/home/kkkqkx/mtl/release/
  lib/      libmtl.so + libxdp* + libbpf* + librte_*.so*
  bin/      e2e_app mp4_dist_tx mp4_dist_rx rx_minimal test_*
  config/   config/ 副本
```
该目录同时作为 devs → 板端 的中转暂存。

---

## 4. 传输（devs → 本机 → 板端）

本机作为中转（devs 不能直接 `ssh board`）：
```bash
# 1) devs 上打包（见 §1 来源），得 /tmp/mtl_release.tar
# 2) 拉到本机归档
rsync -a devs:/tmp/mtl_release.tar /home/kkkqkx/mtl/release/
mkdir -p /home/kkkqkx/mtl/release && tar xzf /home/kkkqkx/mtl/release/mtl_release.tar -C /home/kkkqkx/mtl/release/
# 3) 本机 → 板端
rsync -a /home/kkkqkx/mtl/release/lib/ board:/usr/lib/aarch64-linux-gnu/
rsync -a /home/kkkqkx/mtl/release/bin/  board:/opt/mtl-rk3588/bin/
rsync -a /home/kkkqkx/mtl/release/config/ board:/opt/mtl-rk3588/config/
ssh board 'ldconfig'
```

---

## 5. 运行前系统前置（实跑管线时必需）

- **大页内存**：MTL 用 DPDK 内存池，需要 hugepages。
  ```bash
  sudo sysctl -w vm.nr_hugepages=1024        # 2MB×1024，按负载调整
  ```
- **MtlManager 守护**：lcore/队列仲裁（按需）。
  ```bash
  sudo MtlManager &
  ```
- **网卡 XDP 模式**：AF_XDP 后端走内核驱动 + XDP，无需 DPDK PMD / `vfio-pci`；NIC 保持内核驱动即可。
- **PTP / 时钟**：ST2110 需要时间同步，参考 `config/ptp/` 与 `config/*_dual_host.json.example` 填实际 IP。
- 板端 `libbpf.so.1` / `libxdp.so.1` 由本部署提供，无需另装。

---

## 6. 验证

```bash
# 库解析
ssh board 'ldd /opt/mtl-rk3588/bin/mp4_dist_tx | grep -i "not found"'   # 应为空
# 启动自检（不需要硬件即可验证动态链接与参数解析）
ssh board '/opt/mtl-rk3588/bin/mp4_dist_tx --help'
ssh board '/opt/mtl-rk3588/bin/e2e_app --help'
```
完整管线（捕获/编码/收发/解码/显示）需接 NIC、摄像头、显示器后在板端实跑。
