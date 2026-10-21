# 配置文件说明

完整说明见上级 [README.md](../README.md)。

## 文件一览

| 文件 | 用途 |
|------|------|
| `tx_dual_host.json` | 双机 TX（PC → RK） |
| `rx_dual_host.json` | 双机 RX |
| `tx_mp4_dist.json` | 单板 TX（RK 双网口） |
| `rx_mp4_dist.json` | 单板 RX |

测试片源均为 **`../InTheGrey.mp4`**。

## 快速运行

```bash
# 双机
./build/mp4_dist_rx 120 --config config/rx_dual_host.json          # RK 先起
./build-tx/mp4_dist_tx 120 --config config/tx_dual_host.json       # PC 后起

# 单板
./tests/mp4_dist_both.sh 120 --config config/tx_mp4_dist.json --fps 23.976
```
