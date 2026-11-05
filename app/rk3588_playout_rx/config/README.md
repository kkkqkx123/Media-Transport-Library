# 配置文件说明

完整说明见上级 [README.md](../README.md)。本包为解码接收端，只保留接收配置；发送端配置由编码包 `rk3588_live_tx` 提供。

## 文件一览

| 文件 | 用途 |
|------|------|
| `rx_dual_host.json.example` | 双机 RX，复制为 `rx_dual_host.json` 后按网卡修改 |

参考流探针片源为 **`../InTheGrey.mp4`**（只做期望流描述，不做发送）。

## 快速运行

```bash
# 双机（发送端 Details 见 rk3588_live_tx）
./build/file_playout_rx 120 --config config/rx_dual_host.json          # RK 先起

# 单板（RX 先起，2 秒后编码端发送，参数两侧对齐）
./build/file_playout_rx 120 --config config/rx_dual_host.json --fps 60 --width 1920 --height 1080
```
