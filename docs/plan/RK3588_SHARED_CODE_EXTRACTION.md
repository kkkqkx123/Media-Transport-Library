# RK3588 应用共用代码抽取方案（不动存量代码）

> 约束（用户明确要求）：
> 1. 两个 rk3588 应用**暂不独立化**，留在本仓库；
> 2. **原始代码不改动**，以免影响与上游 MTL 的同步；
> 3. `ref/` 已迁移至仓库根 `ref/mpp-ref/`，不在本方案范围内。
>
> 因此本方案是**纯增量（additive-only）**方案：新建 `app/rk3588_common/`，
> 存量两个应用的源文件一个字符都不改。改动仅限新建文件与（可选的）
> 两个应用各自 meson.build 的 include 路径追加。

## 0. 完成情况总览（2026-10-29 实测代码）

对 `app/` 实际代码与本方案逐项核对，**所有阶段均未落地**：

| 阶段 | 状态 | 证据 |
|---|---|---|
| 阶段 1：建立 `app/rk3588_common/` + A 档模块 + `check_sync.sh` | **未开始** | `app/rk3588_common/` 不存在；仓库内无 `check_sync.sh`；两个 meson.build 无 `rk3588_common` 引用 |
| 阶段 2：新代码改用 common（惯性迁移） | **未开始** | 两侧 meson.build 未追加 `../../rk3588_common/include`，无 `librk3588_common.a` 链接 |
| 阶段 3：B 档超集合并（timer / pipeline_common / codec_plugin） | **未开始** | 两侧本地拷贝仍各自存在；`pipeline_common.h` 并非 mp4_dist 对 end2end 的超集，需重新评估（见 §1 修订） |
| 阶段 4：C 档深度分叉（drm_renderer / mpp_decoder） | **未开始** | 两份拷贝仍独立，无合并信号触发 |

A 档 3 组模块的 md5 一致性已复核（仍完全相同），可作为阶段 1 的直接输入：
`ring_buffer`（.c/.h）、`slab_pool`（.c/.h）、`timer.h` 三方 md5 一致。

### 0.1 §1 盘点表的修订（与实测代码对齐）

- `codec_plugin.h`：差异不是 "3 行 / 两字段"。实测 end2end 版额外包含
  `codec_config.intra_refresh_mode/arg` 两字段，以及 `codec_plugin` 的两个可选回调
  `get_last_output_ns`、`get_full_drops`（mp4_dist 版均无）。约 12 行差异。
- `pipeline_common.h`：差异 82 行，且**不是** "mp4_dist 为超集"。end2end 定义
  `dmabuf_frame` + `codec_packet`（含 `cap_timestamp`/`frame_seq`/`mtl_owner`）；
  mp4_dist 定义 `codec_packet`（字段不同：`capture_tai_ns`/`owner`）+ `rx_pkt` +
  `decoded_frame` + `enum mp4_dist_mode`。同名 `codec_packet` 字段语义不同，
  不能直接"以 mp4_dist 版为准"合并，阶段 3 需先做字段对齐设计。
- 其余条目（timer.c 23 行、drm_renderer 764 行、mpp_decoder 994 行）与实测一致。

## 1. 现状盘点（2026-10 实测 md5 / diff）

两个应用各自带一份基础模块拷贝，分歧程度分三档：

| 模块 | end2end 位置 | mp4_dist 位置 | 差异 | 分级 |
|---|---|---|---|---|
| `ring_buffer` | `pipeline/` | `shared/` | md5 完全一致（.c/.h） | A：完全相同 |
| `slab_pool` | `hal/` | `shared/` | md5 完全一致（.c/.h） | A：完全相同 |
| `timer.h` | `hal/` | `shared/` | md5 完全一致 | A：完全相同 |
| `timer.c` | `hal/` | `shared/` | 23 行：mp4_dist 版是超集（`mtl_handle` 非空时读 PTP 时钟，否则退回 `CLOCK_MONOTONIC`） | B：超集分叉 |
| `codec_plugin.h` | `codec/` | `shared/` | ~12 行：end2end 多 `intra_refresh_mode/arg` 两字段 + `get_last_output_ns`/`get_full_drops` 两回调 | B：互为超集分叉（方向相反） |
| `pipeline_common.h` | `pipeline/` | `shared/` | 82 行；同名 `codec_packet` 字段不同，非超集关系 | B：分叉（需重新评估） |
| `drm_renderer` | `io/` | `shared/` | .c 差 764 行；接口不同（`drm_renderer_create(device,…)` vs `drm_renderer_create(params*)`，mp4_dist 多 layers/layout 能力） | C：深度分叉 |
| `mpp_decoder` | `codec/` | `shared/` | .c 差 994 行；mp4_dist 多 stats/AU 级输入/EOS/reset 等约 10 个接口 | C：深度分叉 |
| `drm_mode` / `display_layout` / `ptp_time` / `e2e_stamp` 等 | 无 | `shared/` 独有 | — | D：单侧独有，不属于共用 |

结论：**真正可直接合并的只有 A 档 3 组；B 档可在接口取并集后合并；C 档现阶段不合并**，
强行合并会迫使其中一侧行为改变，违反"不改原始代码"约束。

## 2. 目标形态

```
app/
├── rk3588_common/            # 新增：共享静态库工程（独立 meson 子工程）
│   ├── meson.build
│   ├── include/rk3588_common/ # 对外头文件（带 rk3588_common/ 前缀，避免与本地拷贝重名冲突）
│   │   ├── ring_buffer.h
│   │   ├── slab_pool.h
│   │   └── timer.h
│   └── src/
│       ├── ring_buffer.c
│       ├── slab_pool.c
│       └── timer.c
├── rk3588_end2end/           # 存量不动；旧拷贝继续使用
└── rk3588_mp4_dist/          # 存量不动；旧拷贝继续使用
```

- `rk3588_common` 构建为 `librk3588_common.a`，依赖只允许：threads、libdrm（按需）、`mtl`（可选，供 timer 的 PTP 路径）。
- **头文件加 `rk3588_common/` 目录前缀**是关键设计：两个应用旧源码里的
  `#include "ring_buffer.h"` 仍解析到本地拷贝，互不干扰；新代码显式写
  `#include "rk3588_common/ring_buffer.h"` 才走公共库。

## 3. 分阶段实施

### 阶段 1：建立 common 工程 + 同步校验（零风险，先行落地）【未开始】

1. 新建 `app/rk3588_common/`，按 §2 形态放入 A 档模块（`ring_buffer`、`slab_pool`、
   `timer`），`timer.c` 采纳 mp4_dist 超集版本（PTP 优先、单调时钟兜底，兼容两侧语义）。
2. `codec_plugin.h` 暂不进 common（两侧互为超集，见 §4 待决问题）。
3. 新增同步校验脚本 `app/rk3588_common/check_sync.sh`：
   对 A 档模块，md5 比对 common 与两个应用的存量拷贝；不一致即非零退出。
   在两个应用 `meson test` 中注册为 test（或在构建脚本中调用），使
   "改了 common 忘了同步"在 CI/本地立刻暴露。
   —— 这是替代"合并代码"的同步机制：不动存量文件，但保证三方一致。

### 阶段 2：新代码改用 common（惯性迁移，不强制）【未开始】

- 后续新增/新写的模块一律放入 `rk3588_common`，两侧通过
  `include_directories('../../rk3588_common/include')`（追加到各自 meson.build，
  不触碰任何 .c/.h）+ 链接 `librk3588_common.a` 消费。
- 存量源文件**仅在因功能改动而被触碰时**，顺手把对应 include 切到
  `rk3588_common/…` 头并从本地拷贝中删除——由开发者按当前改动节奏自然收敛，
  不做一次性大迁移。

### 阶段 3：B 档超集合并（有明确验收后再做）【未开始，pipeline_common 需重新评估】

- `timer.c`：已由阶段 1 覆盖（超集行为兼容 end2end：end2end 不传 `mtl_handle`
  即等价旧逻辑）。两侧本地拷贝删除后，由 check_sync.sh 保证不再回潮。
- `pipeline_common.h`：**暂不按原计划推进**。实测两版 `codec_packet` 同名但字段不同
  （end2end 用 `cap_timestamp`/`frame_seq`/`mtl_owner`，mp4_dist 用
  `capture_tai_ns`/`owner`），且各自独有类型（`dmabuf_frame` vs `rx_pkt`/
  `decoded_frame`/`mp4_dist_mode`）。需先做字段对齐设计，再决定是否合并；
  当前保持两份独立拷贝。
- `codec_plugin.h`：两侧互有增补——end2end 多 `intra_refresh_mode/arg` 两字段及
  `get_last_output_ns`/`get_full_drops` 两个可选回调，mp4_dist 版均无。需先形成
  并集（字段与回调均置可选/默认 0/NULL）再进 common。见 §4 Q1。

### 阶段 4：C 档（长期，不承诺时间点）【未开始】

`drm_renderer` / `mpp_decoder` 深度分叉是**接口级**分叉（创建参数、显示接口、
解码输入粒度都不同），合并等于重设计。仅当出现以下信号才启动：
- 两侧同一模块需要修同一个 bug（第二次出现时）；
- 或新增第三个消费方使三份拷贝不可维持。
届时以 mp4_dist 超集接口为基准重设计，end2end 侧做适配层——这必然触碰
原始代码，属于未来独立决策，不在本方案承诺范围内。

## 4. 待决问题（需要用户拍板后才进入对应阶段）

| # | 问题 | 建议 |
|---|---|---|
| Q1 | `codec_plugin.h` 并集会把 end2end 的 `intra_refresh_*` 字段及 `get_last_output_ns`/`get_full_drops` 两回调带进 mp4_dist 可见的头文件（仅声明，不改变 mp4_dist 行为） | 可接受，字段默认 0 = off，回调默认 NULL |
| Q2 | `timer.c` 的 PTP 路径依赖 `mtl_handle`：common 版把 `mtl` 变为必选依赖，还是保持可选探测？ | 建议 `mtl` 在 common 中 `required: false`，运行期以 `mtl_handle` 判空兜底，两侧现有调用方式均不变 |
| Q3 | meson.build 追加 include 路径与依赖是否算"改原始代码"？ | 本方案假定 meson.build 允许最小追加（C 源码零改动）；若 meson.build 也禁改，则阶段 2 退化为"新模块只进 common，存量完全冻结" |

## 5. 验收口径

1. `git status` 中两个应用的 `.c/.h` 零改动（meson.build 仅允许 §4 Q3 认可的最小追加）；
2. `ninja -C build` 两侧构建产物与改造前一致（二进制行为不变）；
3. `check_sync.sh` 全绿；
4. `app/rk3588_mp4_dist` 的 5 个 meson test 与 `rk3588_end2end/tests/` 四组单测通过。

## 6. 参考

- 差异量化方法：`md5sum` + `diff`（见 §1 表，2026-10-28 实测）。
- 相关文档：`docs/app/README.md`（两应用共同特征，"修改时注意两处同步"即本方案动机）、
  `docs/dev/01-MTL编译体系分析.md` §5（两应用构建结构）、`docs/analysis/`（库侧架构，common 不依赖库侧内部）。
