# RK3588 应用共用代码抽取方案（收敛迁移，已落地）

> 约束：两个 rk3588 应用暂不独立化，留在本仓库；为消除重复与漂移，
> 以 `app/rk3588_common/` 为唯一事实来源，允许修改存量代码（含删除本地拷贝）。
> `ref/` 已迁移至仓库根 `ref/mpp-ref/`，不在本方案范围内。

## 0. 当前状态（唯一状态表，2026-11-02 实测代码）

| 范围 | 状态 | 说明 |
|---|---|---|
| A 档：`ring_buffer`、`slab_pool`、`timer`（.c/.h） | 已完成 | 已迁入 common，两应用本地拷贝已删除，meson 直接消费 common 源 |
| B 档：`timer.c` 超集、`codec_plugin.h` 并集、`pipeline_common.h` 并集 | 已完成 | 均已迁入 common，本地拷贝已删除；`rx_pkt` 重命名为 `mp4_rx_pkt` 避免与 end2end 冲突 |
| C 档：`drm_renderer`、`mpp_decoder` | 未开始 | 深度分叉，保持两份独立，见 §2 触发条件 |
| D 档：`drm_mode` / `display_layout` / `ptp_time` / `e2e_stamp` 等单侧独有模块 | 不合并 | 仅 mp4_dist 使用，继续留在 `shared/` |

本节是唯一的状态记录，实施细节不再重复标注完成状态。

## 1. 合并结果

`app/rk3588_common/` 为独立 meson 子工程，对外提供 5 个头文件与 3 个源文件：

- 头文件：`ring_buffer.h`、`slab_pool.h`、`timer.h`、`codec_plugin.h`、`pipeline_common.h`
- 源文件：`ring_buffer.c`、`slab_pool.c`、`timer.c`（`codec_plugin` 与 `pipeline_common` 为纯头文件类型定义，无对应源文件）

消费方式：两应用的 meson 通过相对路径引用 common 源文件并链接 common 静态库，
同时追加两级 include 路径，使存量源码中的无前缀 `#include "ring_buffer.h"` 等
无需改写即可解析到 common。

各模块合并策略：

- A 档三组迁移前经 md5 确认完全一致，直接迁入。
- `timer.c` 采用 mp4_dist 超集语义：`mtl_handle` 非空时读 PTP 时钟，否则退回
  `CLOCK_MONOTONIC`；end2end 不传 `mtl_handle` 即等价旧逻辑。common 中 `mtl` 为
  可选依赖，end2end 按探测结果决定是否定义 `HAVE_MTL`，mp4_dist 侧必选。
- `codec_plugin.h` 取两侧并集：以 mp4_dist 版为基，加入 end2end 独有的
  `intra_refresh_mode/arg` 两字段及 `get_last_output_ns`、`get_full_drops` 两个
  可选回调（默认 0/NULL，不改变 mp4_dist 行为）。
- `pipeline_common.h` 取字段并集而非语义统一：保留 end2end 的 `dmabuf_frame`，
  `codec_packet` 同时保留两侧字段（`cap_timestamp`/`frame_seq`/`mtl_owner` 与
  `pts`/`capture_tai_ns`/`owner`），各调用方仍只使用自己原有字段；
  mp4_dist 独有的 `rx_pkt` 重命名为 `mp4_rx_pkt` 后并入，另有 `decoded_frame`
  与 `mp4_dist_mode`。后续 common 若需新增包类型，应起独立类型名，
  不复用 `codec_packet`，避免语义混淆。

历史上的增量同步设想（common 与本地拷贝并存、附带 md5 校验脚本）已废弃：
本地拷贝删除后校验脚本随之删除，不再有三方一致性问题。

## 2. 未合并项

- C 档 `drm_renderer`（.c 差约 764 行，创建参数与显示接口不同）与
  `mpp_decoder`（.c 差约 994 行，mp4_dist 多 stats/AU 级输入/EOS/reset 等接口）
  为接口级分叉，合并等于重设计，现阶段不合并。仅当同一模块需在两侧修同一个
  bug（第二次出现时），或出现第三个消费方时，才以 mp4_dist 接口为基准重设计，
  end2end 侧做适配层，属未来独立决策。
- D 档单侧独有模块不属于共用范围，保持现状。

## 3. 验收口径

1. 两应用不再保留已迁入 common 模块的本地拷贝；
2. 两应用 meson 构建通过（含 MTL 可用/不可用两种配置的 end2end 构建），
   end2end 四组单测与 mp4_dist 的 meson test 通过；
3. 行为一致性：timer 的 PTP 优先路径对 end2end 为新增改进但兼容旧调用；
   codec_plugin 并集字段对 mp4_dist 无行为变化；
   pipeline_common 并集下双方仅使用各自原有字段。

## 4. 参考与遗留待办

- 差异量化方法：`md5sum` + `diff`（2026-10 实测）。
- 相关文档：`docs/app/README.md`、`docs/dev/01-MTL编译体系分析.md` §5、库侧 `docs/analysis/`。
- 遗留待办：`docs/app/README.md` 中"当前未抽取公共库，修改时注意两处同步"的
  说明已过时，需更新为"以 `app/rk3588_common/` 为准"。
