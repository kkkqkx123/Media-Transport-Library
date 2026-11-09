# BGR 直投编码 3 分块故障分析与 MPP 步距矩阵结论

日期：2026-11-09。关联：单板 TX/RX 联调中 RX 显示横向约 3 重复加条纹、
无法看清内容；直通显示正常。

## 1 现象

`file_playout_rx` 全屏显示链路画面呈约 3 分块横向重复、垂直方向疑似拉伸、
整体偏灰绿、伴一条横向错位带。直通（V4L2 采集 CPU 转 NV12 直接送显）
同一信号源显示正常，故显示硬件、模式、同步机制出局。

## 2 定位方法（确认分块在网流字节里）

先排除呈现：在板上以网流转储（`FILE_PLAYOUT_DUMP_RX`/`_IDX`）抓取 ST22
重组后的 H264，用 `mpi_dec_test` 离线重解为 YUV 后做两项无视觉判定：

- 列自相关：中段行平均的列轮廓在位移 640 处出现精确峰（1920/640=3），
  正常内容无此峰；
- ASCII 俯瞰：抽稀为约 96 列字符画，分块结构肉眼可辨，无需传图。

网流离线解码同样 640 周期，而 MPP 直播解码计数器（`err/discard`）全零、
重组计数（`corrupt/incomplete/drop`）全零——计数器只能证明字节完整，
证明不了像素正确，这是本次最大的教训。

## 3 对照

zlmediakit 的 HDMI 编码链路：V4L2 采集 → RGA 转 NV12（64 对齐步距）→
MPP 以 `MPP_FMT_YUV420SP` 编码，**从不直喂 BGR**。本仓库 TX 当时为省 RGA
开销直喂 `MPP_FMT_BGR888`（`hor_stride=1920` 像素），自称经探针验证可行，
但该验证只看了"可出流、可解码"，没看像素。

## 4 采集与编码格式处理隐患

V4L2 侧（`v4l2_capturer`，MPLANE 单平面）：BGR3 上报 `bytesperline=5760`
即宽×3，中途若驱动重协商步距，编码侧按旧步距读取即错位。
TX 以该步距除以 3 得 MPP 像素步距，隐含"步距单位为像素"假设。

MPP 侧（`prep:width/height/format/hor_stride/ver_stride`＋EXT_DMA 导入）：

- 步距单位按格式而异：NV12 下像素即字节（Y 平面），packed RGB 下像素
  与字节差 3 倍，实现中两处（prep 配置与 `mpp_frame_set_hor_stride`）
  必须一致，否则读错行；
- 导入内存大小必须覆盖步距×高，外部分配（RGA 池或 dmabuf）与 MPP 步距
  不一致即越界或欠读；
- NV12 建议 64 对齐步距（1920 恰好满足），BGR 无成熟对齐实践；
- `prep:format` 只做接纳校验，不保证该格式在该 MPP 构建上像素正确。

RGA 归一化契约（zlmediakit 与本仓库修复后一致）：任何非 NV12 采集一律
经 RGA 转 NV12 后再交编码器，编码器只见 NV12，步距语义单一。

## 5 步距矩阵验证

探针 `debug/mpp_enc_frame_latency.c` 新增 `--hor-stride`（MPP 像素步距）、
`--file-stride`（文件行字节）、`--infmt rgb3`，缺省行为不变。
矩阵脚本 `debug/bgr_stride_matrix.sh`（板端原生编译运行）：合成三分区
可区分 BGR3 图案（三分区色相不同＋白线＋行带），一路 CPU 转 NV12 作
REF，其余格直投编码 8 帧后离线解码打分（E640＝640 周期能量，
thirds＝三分区可区分度，MADvsREF＝与 REF 的平均差异）。

| 格 | 设置 | E640 | thirds | MADvsREF | 判定 |
|---|---|---|---|---|---|
| ref | NV12 hs1920 | -0.55 | 25.3 | 0.0 | CLEAN |
| bgr1920 | BGR888 hs1920（生产原语义） | 2.01 | 0.0 | 36.6 | TILED |
| bgr5760 | BGR888 hs5760（字节步距假设） | 1.96 | 0.1 | 65.3 | TILED |
| rgb1920 | RGB888 hs1920（通道序假设） | 2.02 | 0.0 | 36.6 | TILED |

packed 24 位直投无论像素步距、字节步距、通道序全部 3 分块，
thirds 塌为 0 说明三区被抹成相同内容；只有 NV12 正确。

## 6 结论

本栈 MPP 的 BGR888/RGB888 直接编码输入像素错误（配成功能但几何错），
**BGR 直投不可用**，RGA→NV12 是必经之路。生产修复：`tx_encode.c` 去掉
BGR 直投分支（非 NV12 一律 RGA），`mpp_encoder.c` 收敛为纯 NV12。
修复后 `rga ok` 从 0 变为 60/s（ema 约 1.2ms），网流离线重解恢复干净
单场景，看屏待人工复检。探针保留 `bgr3` 格作为回归哨兵（应持续 TILED，
若某天变 CLEAN 说明 MPP 行为变了）。
