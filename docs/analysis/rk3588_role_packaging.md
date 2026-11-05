# RK3588 应用角色与打包分析

## 结论

- `rk3588_live_ingest` 与 `rk3588_file_playout` 都支持收发，适合开发、联调和回环验证。
- 生产节点角色互斥：输入盒（编码/TX）与输出盒（解码/RX）。
- 应按角色拆分部署包，而不是让一个节点包同时收发。

## 角色归属

- `rk3588_live_ingest`：更偏输入盒/编码参考实现，包含 V4L2 采集、RGA、MPP 编码、MTL TX；RX 侧主要用于回环验证。
- `rk3588_file_playout`：更偏输出盒/解码参考实现，包含 MTL RX、MPP 解码、DRM/ALSA；TX 侧是文件源播放。
- `dvs_media` 由 `rk3588_file_playout` 的 RX 侧演进，是生产输出盒媒体引擎。

## 打包建议

- 输出盒包：现有 `deploy/dvs-node`（`dvs_agent` + `dvs_media`，RX-only）。
- 输入盒包：新增 TX-only 媒体引擎，从 live_ingest 采集/编码/MTL TX 链路或 file_playout TX 演进，并与 agent 同包发布。
- 生产镜像只包含对应角色的二进制，不交付 `both` 模式。
- `both` 模式保留为开发/回环验证能力，并明确其运行方式。
