# RK3588 PTP 现状与方案对比

## 当前现状

- 网卡：Motorcomm YT6801 (`eth0`/`eth1`)
- 时间戳能力：仅 `software-receive`，无 `software-transmit`，无硬件 PTP
- linuxptp：已安装 3.1.1，但无法在该网卡上运行 master/slave
- 内核：`6.1.118`，未启用 `CONFIG_HUGETLBFS`
- 应用：支持 `external`、`builtin`、`monotonic` 三种 PTP 模式
- 单板基线：`monotonic` 可用
- 跨节点严格同帧：需要共享 PTP 时间域

## 方案对比

| 方案 | 是否需要 TX 时间戳 | 精度 | 部署复杂度 | 是否支持跨节点 | 适用场景 |
|---|---:|---:|---:|---:|---|
| linuxptp 双向 PTP | 是 | 高 | 中 | 是 | 有支持时间戳的网卡 |
| 单向 PTP | 否 | 中 | 中 | 是 | 当前 YT6801 板卡 |
| NTP/chrony | 否 | 毫秒级 | 低 | 是 | 粗同步、过渡方案 |
| monotonic 相对时钟 | 否 | 进程内 | 低 | 否 | 单板调试 |
| 硬件 PTP GM | 是 | 微秒级 | 高 | 是 | 生产环境 |

## 结论

- 当前板卡不能使用 linuxptp 双向 PTP。
- `monotonic` 不能跨节点同步。
- 正式方案：单向 PTP + 交换机 TC/BC（若交换机支持）。
- 无交换机场景：仅使用单向 PTP 做验证。
- NTP/chrony 仅作为过渡或调试方案。
- 生产环境仍建议使用支持 PTP 的网卡或独立 PTP Grandmaster。
