# RK3588 PTP 验证记录

## 日期

2026-11-12

## 板端准备

- `eth0` 静态 IP 设置为 `192.168.80.204/24`，网关 `192.168.80.254`
- WiFi 默认路由保持优先
- 安装 `linuxptp 3.1.1`
- 设置内核 TAI offset 为 37 秒
- `CLOCK_TAI` 可用，`CLOCK_TAI - CLOCK_REALTIME ≈ 37s`
- 停止 `systemd-timesyncd`

## 可执行文件同步

- 从 devs 同步 `live_ingest`、`file_playout_rx`、`libmtl.so` 及 DPDK/libxdp/libbpf 依赖到板端 `/home/ztl/code/mtl/bin` 与 `/usr/local/lib`
- 生成测试视频 `/home/ztl/media/test_720p30.mp4`

## PTP 尝试结果

1. `ptp4l` master 模式在 `eth0` 上启动失败：

```text
interface 'eth0' does not support requested timestamping mode
failed to create a clock
```

2. 原因：`eth0`/`eth1` 使用 Motorcomm YT6801 驱动，仅支持 `software-receive` 时间戳，不支持 PTP 发送时间戳。

3. 因此 linuxptp 无法在该网卡上完成同步，外部 PTP 网络同步未完成。

## 应用运行尝试

1. 首次运行失败：DPDK EAL 无法读取 hugepage 信息，内核未启用 `CONFIG_HUGETLBFS`。
2. 临时在 devs 上为 MTL 添加 `--no-huge` 并移除 `--match-allocations` 后重新构建，应用仍失败：

```text
mt_mempool_create_by_ops(0), fail(Invalid argument) for T_P0_SYS, n 2047
mtl_init, st dev if init fail -12
```

3. 原因：运行内核缺少 `CONFIG_HUGETLBFS`，MTL/DPDK 内存分配无法完成。

## 结论

- 网络静态 IP 配置完成并验证。
- linuxptp 与 TAI offset 准备完成，`CLOCK_TAI` 可用。
- PTP 网络同步未完成，阻塞原因为网卡不支持 PTP 发送时间戳。
- 应用运行未完成，阻塞原因为内核未启用 `CONFIG_HUGETLBFS`。
- 要完成端到端验证，需要更换或启用支持 PTP 发送时间戳的网卡/驱动，并启用内核 hugepage 支持。
