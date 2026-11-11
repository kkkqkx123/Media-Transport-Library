# 8 lane 探针验证与回退说明（2026-11-11）

## 起因

`MP4_PIP_MAX_LANES=8` 与 `dvs` 侧 8 槽 admission 早已按 8 设计，但显示侧
`MP4_PIP_V1_NV12_LANES=4` 把第 5 路起静默截断（有 WARN、无会话、无显示）。
为消除静默丢弃，曾短暂放开到 8：布局不再截断，第 5 路起的 plane 在
`drm_renderer` 创建时经 DRM 探针运行时解析，同 CRTC 无空闲 NV12 plane 时
创建直接失败（fail-fast），并把已解析 plane 写回布局以保持几何快路径。

## 板上实测

- 5 lane 回环（5 路文件源 TX）：`drm: lane 4 has no free NV12 plane (4 probed)`，
  compositor 创建失败，行为符合 fail-fast 设计。
- 结论：本板同 CRTC 仅 4 个 NV12-capable plane，4 即同时直接显示的硬件上限。
- 4 lane 回环（4 路文件源 TX）：350 composites 全 lane 出数，每 lane
  `dec_p50/dec_p99` 与 `BR[]` 行齐全，四路 work 值一致（13~15us），
  `ring_max=0`，无争用迹象；`put_fail=0`。

## 回退决定

8 lane 同时直接显示在当前硬件上不可能实现，探针代码当日即拆除
（`drm_renderer` 恢复固定 4 plane，布局恢复截断到 4 + WARN，不留死代码），
上限改回 4。`dvs_platform` 单节点预算 4 与此一致，无需联动。

保留项（与 lane 数无关，不受回退影响）：`rx_decode_common` 共用解码核心、
compositor 每 lane `dec_p50/dec_p99`、`BR[]` bridge 争用量化、6 lane 单测
用例已随回退删除（截断语义下会失败）。

## 后续参考

- 同时点亮 5~8 路的唯一现实路径是 RGA 预合成（多 lane 合成到一个 plane），
  属新功能，不在本次范围。若将来做，准入、capability 上限、plane 分配与
  本次探针结论（4 plane 硬上限）都是输入。
- 本次 4 lane 实测的 dec/BR 基线（`work_ema` 13~15us、`wait_ema` ~32ms、
  `ring_max=0`）可作为 bridge 争用判定的单机参照。
