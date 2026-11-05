# dvs_media 是否应集成 rk3588_live_ingest —— 分析与设计方案

> 关联分析：`docs/analysis/DVS_MEDIA_RK3588_INTEGRATION.md`
> 关联代码：`app/dvs_media/`（现状）、`app/rk3588_live_ingest/`（donor）、`app/rk3588_file_playout/`（已复用 RX 源）
> 状态：方案待评审；**结论：应当集成**，以使节点后台可在解码（output）与编码（input）角色间切换。

---

## 1. 需求与约束

- **需求**：节点后台应能切换"编码 / 解码"角色。即同一 `dvs_media` 进程在受控（IPC）模式下，既可作为 output 接收 ST 2110 流解码显示（已具备），也可作为 input 捕获本机 HDMI → 编码 → 发送 ST 2110 流（当前缺失）。
- **角色互斥**：`engine_ctx.c` 已实现 `role` 互斥——`input` 时停掉 RX compositor，`output` 时停掉 TX。两套数据面不会同时跑。
- **现状缺口**：`dvs_media` 的 TX / input 路径（`engine/tx_session.c`）只是**参数校验 + 状态机**，`dvs_tx_backend_fn` 为 NULL，启动后只报 `DVS_TX_CONFIGURED` + `backend_pending`，没有任何数据面流动（`tx_session.c:112-120`）。`tx_session.h:54` 明确写 donor chain 来自 `rk3588_live_ingest`。

---

## 2. 结论：应当集成

理由：

1. **需求硬要求**：角色切换意味着 input role 必须有能工作的捕获→编码→发送链路。该链路在仓库内只有 `rk3588_live_ingest` 完整实现（V4L2 → RGA → MPP encode → MTL ST22 TX），没有理由重新造一套。
2. **复用成本低、风险可控**：live_ingest 已是纯 C、模块化（hal / io / codec / pipeline / mtl_tx），且自身就 **in-tree 复用 `rk3588_common`**（timer/slab/ring_buffer），与 dvs_media 现有的基础库来源一致，不存在第三套重复。
3. **MTL 实例天然可共享**：`mtl_tx_params.st` 是外部传入的 `mtl_handle`，dvs_media 已有 `e->st`，可直接复用同一 MTL 实例（一个进程只能 `mtl_init` 一次），无需为 TX 另起 MTL。
4. **编排层即插即用**：live_ingest 的 `tx_pipeline` 通过 `tx_mtl_sink_ops`（acquire_fb/publish_fb/abort_fb 回调）把编码结果喂给 MTL，正好对接 `mtl_tx`，dvs_media 只需在 backend 里把两者 glue 起来，不必改写 pipeline 内部。

> 否集成方案（仅 output、不接 input）无法满足"角色切换"需求，排除。

---

## 3. live_ingest TX 链路拆解（可复用组件）

来自 `app/rk3588_live_ingest/meson.build` 与头文件：

| 组件 | 文件 | 职责 | 是否引入 dvs_media |
|---|---|---|---|
| HAL | `hal/dma_allocator.c`、`hal/dma32_allocator.c`、`hal/sei_timestamp.c` | DMA 缓冲分配、SEI 时间戳 | 引入（capture/DRM 需要） |
| rk3588_common | `ring_buffer.c`、`slab_pool.c`、`timer.c` | 基础无锁组件 | **已引入**，复用同一份 |
| IO | `io/v4l2_capturer.c` | V4L2 捕获（/dev/videoN → dma-buf 帧回调） | 引入 |
| Codec | `codec/rga_converter.c`、`codec/mpp_encoder.c`、`codec/sps_vui.c` | RGA 格式转换 + MPP 编码 | 引入（encoder 路径需要） |
| Codec | `codec/mpp_decoder.c` | MPP 解码 | **不引入**（dvs_media 已用 file_playout/shared/mpp_decoder.c） |
| Pipeline | `pipeline/tx_pipeline.c`、`pipeline/slice_reassembler.c` | 捕获→编码线程编排 + MTL sink 钩子 | 引入 |
| MTL 包装 | `mtl_tx.c` / `mtl_tx.h` | ST22 TX session（acquire/publish framebuff） | 引入 |
| 应用编排 | `live_ingest_app.c` | 把上面串起来的示例 | **不引入**（改为 dvs_media 自己的 backend 编排） |

关键接口（已确认）：
- `struct tx_pipeline_params`（`pipeline/tx_pipeline.h`）：`v4l2_device`、`width/height/fps`、`drm_device`、`bitrate_bps`、`gop_size`、`num_slices`、`tx_mtl_sink_ops`、`time_now/time_priv`、`cpu_capture/cpu_encode`。
- `struct mtl_tx_params`（`mtl_tx.h`）：`st`（MTL 句柄）、`width/height/fps`、`udp_port`、`dip_addr`、`port_name`。
- `tx_pipeline_start/stop`、`mtl_tx_create/start/stop/free`、`mtl_tx_acquire_fb/publish_fb/abort_fb`。

---

## 4. 设计方案

### 4.1 复用策略（沿用 in-tree）

与现有 file_playout 复用方式一致：**不复制源码**，在 `app/dvs_media/meson.build` 新增一个 `dvs_tx` 静态库，in-tree 编译上述 live_ingest 源文件，链接进 `dvs_media` 可执行文件。include 路径追加 `../rk3588_live_ingest` 与 `../rk3588_live_ingest/codec`（codec_plugin.h 来自 rk3588_common，已含；rga/mpp encoder 头在 live_ingest 本地）。

新增 meson 依赖：`librga`（探测 `dependency('librga', required: false)`，但 dvs_media 目前是 RX 必需 MPP，集成 TX 后 RGA 也变必需——见 §6 风险）。

### 4.2 参数映射：`dvs_tx_params_t` → live_ingest 组件

| `dvs_tx_params_t` | live_ingest 落点 |
|---|---|
| `port`（"HDMI1"…） | `v4l2_device` = 端口映射表（`HDMI1→/dev/video0`，`HDMI2→/dev/video1` …），与 `capability.c` 的 `probe_input` 枚举保持一致 |
| `codec`（"h264"/"h265"） | MPP encoder `codec_config.codec` |
| `width`/`height` | `v4l2_capturer_create` + `rga_converter_create` + `mpp_encoder` + `mtl_tx_params` |
| `fps` | `v4l2_capturer` + encoder + `mtl_tx_params` |
| `bitrate_kbps` | `tx_pipeline_params.bitrate_bps = bitrate_kbps * 1000` |
| `target_sip` | `mtl_tx_params.dip_addr` |
| `target_port` | `mtl_tx_params.udp_port` |
| （NIC 发送端口） | `mtl_tx_params.port_name` + 引擎 `e->cfg` 的 TX port（新增字段，见 §4.4） |

### 4.3 实现 `dvs_tx_backend_fn`（真正的 TX 数据面）

在 `engine/` 新增 `tx_backend_live_ingest.c`（或并入 `tx_session.c`），实现 backend 钩子：

```
static struct mtl_tx* g_tx;
static struct tx_pipeline* g_tp;

static int dvs_media_tx_backend(const dvs_tx_params_t* p, char* err, size_t errlen) {
    // 0. 取模块级保存的 engine 上下文（st + tx_port），见 §4.5
    mtl_handle st = dvs_tx_ctx_st();
    const char* vdev = dvs_tx_port_to_v4l2(p->port);     // HDMIx -> /dev/videoN
    if (!vdev) { snprintf(err,…,"bad capture port"); return -1; }

    // 1. MTL TX session（复用 e->st，不新建 MTL）
    struct mtl_tx_params mp = {
        .st = st, .width=p->width, .height=p->height, .fps=p->fps,
        .udp_port = p->target_port, .dip_addr = p->target_sip,
        .port_name = dvs_tx_ctx_tx_port(),   // 例如 "0000:xx:00.0" 或 "enpX"
    };
    g_tx = mtl_tx_create(&mp);
    if (!g_tx) { … return -1; }
    mtl_tx_start(g_tx);

    // 2. TX pipeline（capture→RGA→encode），并把 MTL 作为 sink
    struct tx_mtl_sink_ops sink = {
        .priv = g_tx,
        .acquire_fb = (void*)mtl_tx_acquire_fb,
        .publish_fb = (void*)mtl_tx_publish_fb,
        .abort_fb   = (void*)mtl_tx_abort_fb,
    };
    struct tx_pipeline_params tp = {
        .v4l2_device = vdev, .width=p->width, .height=p->height, .fps=p->fps,
        .drm_device = "/dev/dri/card0", .rga_pool_size = 4,
        .bitrate_bps = p->bitrate_kbps * 1000,
        .mtl_sink = &sink,
        .time_now = (uint64_t(*)(void*))mtl_ptp_read_time, .time_priv = st,
    };
    g_tp = tx_pipeline_create(&tp);
    if (!g_tp) { mtl_tx_free(g_tx); … return -1; }
    if (tx_pipeline_start(g_tp) != 0) { … return -1; }
    return 0;   // 成功 → DVS_TX_RUNNING
}
```

停止/重建：backend 不负责 stop（现有 `dvs_tx_stop` 是 state-only）。需在 `engine_ctx.c` 的 `dvs_engine_tx_stop` 与角色切换处，调用 `tx_pipeline_stop` + `tx_pipeline_free` + `mtl_tx_stop` + `mtl_tx_free`，把 `g_tp/g_tx` 复位（目前这些 teardown 是 TODO，见 `tx_session.c:141-153` 注释）。

### 4.4 MTL 实例共享（关键设计点）

- dvs_media 当前 `file_playout_mtl_init(&e->cfg, e->cfg.rx_port, e->cfg.rx_lip, 0, 1, …)`（`engine_ctx.c:114`）只初始化 **RX port**。
- ST22 TX session（`mtl_tx_create`）必须在已 `mtl_init` 的 **TX port** 上创建。因此：
  1. **为 `file_playout_cfg` 增加 `tx_port` / `tx_lip` 字段**（或在 dvs_media 侧扩展配置），并在 `file_playout_mtl_init` 中一并初始化该 port；
  2. 或在 input role 启动 TX 时，确保 MTL 初始化覆盖 TX port。`dvs_engine_tx_start` 目前**未调用** `file_playout_mtl_init`——需补上"若 `e->st==NULL` 则带 tx_port 初始化"的逻辑（复用 main 在 IPC 无 layout 时已调的 `dvs_engine_init_mtl` 路径，但要传 tx_port）。
- 单一 `e->st` 同时承载 RX session 与 TX session，符合 MTL "一个实例多 port / 多 session" 模型，无需双 init。

### 4.5 backend 获取 engine 上下文

`dvs_tx_backend_fn` 签名只有 `(params, err, errlen)`，不含 engine 句柄。采用与 `ipc_engine_bridge.c` 的 `s_eng` 一致的**模块级全局**：在 `dvs_engine_tx_start` 调用 `dvs_tx_set_backend` 之前，把 `e->st` 与 TX port 写入 `tx_backend_live_ingest.c` 的模块全局（如 `dvs_tx_ctx_set(e)`）。backend 内只读这些全局。

### 4.6 角色热切换流程（更新 engine_ctx）

```
dvs_engine_set_role(eng, "input"):
    stop RX compositor (已有)
    不做其它
dvs_engine_tx_start(eng, params):
    确保 MTL init 且含 tx_port (新增)
    调用 dvs_tx_start → backend (live_ingest 链路)  (改造点)
dvs_engine_tx_stop(eng):
    tx_pipeline_stop + free; mtl_tx_stop + free  (新增 teardown)
    state → IDLE (已有)
dvs_engine_set_role(eng, "output"):
    dvs_tx_stop(eng->tx)  (已有，扩展为真正 teardown)
```

---

## 5. 验收 / 测试

- **单元（x86 可跑）**：`tests/test_tx_session.c` 现有 fake backend 保留；新增一个"backend 注册/参数映射"单测（验证 `HDMIx→/dev/videoN`、`bitrate_kbps→bps`、codec 字符串→枚举映射），不依赖硬件。
- **集成（编译服务器 + RK3588）**：
  - output role 回归：RX 合成/显示/ALSA 不受影响（file_playout 复用层不变）。
  - input role 冒烟：节点切到 input，V4L2 捕获→编码→MTL TX 出流；`dvs_engine_tx_status` 报告 `running` 且 `backendPending=false`；用对端 RxTxApp / dvs_media output 节点收流验证。
  - 角色切换：output→input→output 往返，验证旧链路干净 teardown、新链路重建、无 MTL 双 init。

---

## 6. 冲突与风险

1. **MTL TX port 初始化缺失（最高优先级）**：当前 `file_playout_mtl_init` 只覆盖 RX port，必须先补 TX port 初始化，否则 `mtl_tx_create` 失败。→ 见 §4.4。
2. **MPP decoder 重复**：live_ingest 的 `codec/mpp_decoder.c` 与 dvs_media 已用的 `file_playout/shared/mpp_decoder.c` 是两份不同实现。TX 路径**只引入 encoder + rga + sps_vui**，`mpp_decoder.c` 不引入，避免符号/语义冲突。若发现 `tx_pipeline` 间接依赖 decoder，则需二选一统一（优先 file_playout 那份，保持 dvs_media 单一 decoder 实现）。
3. **hal 重复**：live_ingest 的 `hal/dma_allocator.c` 与 file_playout 的 `shared/drm_renderer.c` 都碰 DRM/DMA。因都是 `static_library` 内部符号，链接不冲突；但逻辑重复，后续可考虑上提 `rk3588_common`，本期接受。
4. **新增 RGA 依赖**：dvs_media 此前无 `librga` 依赖；集成后 RGA 变为必需（与 MPP 同级别 `required: true` 或 `found()` 失败即 `error`）。
5. **V4L2 与 DRM 资源**：input role 下 RX compositor（DRM 显示）已停，V4L2 capture 与 DRM 不争用；但同一 `/dev/dri/card0` 既供 RGA DMA 分配又曾供显示，需确认 RGA/DMA 堆在显示关闭后仍可用（通常独立）。
6. **in-tree 强耦合蔓延**：dvs_media 现已绑定 file_playout，再绑定 live_ingest，修改任一方内部头都会波及。建议在 `docs/analysis` 持续记录三者共享边界。

---

## 7. 实施步骤（建议顺序）

1. **MTL 双 port 初始化**：扩展 `file_playout_cfg` + `file_playout_mtl_init` 支持 tx_port；dvs_media 配置增加 tx_port/tx_lip。
2. **meson 接入**：新增 `dvs_tx` 静态库（live_ingest 的 hal/io/codec-enc/pipeline/mtl_tx）+ `librga` 依赖 + include 路径。
3. **backend 实现**：`tx_backend_live_ingest.c`，含端口映射、mtl_tx + tx_pipeline glue。
4. **teardown 补全**：`dvs_engine_tx_stop` / 角色切换处真实停止并释放 live_ingest 链路。
5. **单测 + 编译服务器 RK3588 冒烟**（output 回归 + input 出流 + 角色切换）。

---

## 8. 待确认 / 开放问题

- `file_playout_mtl_init` 当前参数顺序与是否预留 TX port（`engine_ctx.c:114` 第 4/5 参数为 `0,1`，疑似 flags/num，需核对 `file_playout_config.h` 签名确认 TX port 入口）。
- RK3588 单板上 V4L2 设备节点与 HDMI 输入口的稳定映射（能力探测 `probe_input` 用 `/dev/videoN` 顺序枚举，需与 `port→videoN` 映射表对齐）。
- input role 是否需要音频 TX（ST2110-30）？当前 `dvs_tx_params_t` 仅视频；若需，复用 live_ingest 的 audio 路径或 file_playout 的 `mtl_rx_audio` 反向实现，留待后续增量。
