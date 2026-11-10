# RK3588 H265 编码支持设计

## 背景与现状

MTL 库传输层已支持 H265 码流（`ST22_CODEC_H265 / H265_CBR`，`ST_FRAME_FMT_H265_CODESTREAM`），`dvs_media` 控制面也已透传 `h264,h265`（能力上报、会话校验、后端映射）。但数据面写死 H264：MPP 编解码初始化固定 `MPP_VIDEO_CodingAVC`，SEI 时间戳、SPS VUI、AVCC 解析、BSF 选择、IDR 识别均为 H264 专用。选择 `h265` 会通过校验但实际产出 H264 码流。

宿主机 `D:\project\embedded\zlmediakit` 在本编译环境不可见；以下 H265 事实以 ctx7 查询的 ZLMediaKit 上游实现与 Rockchip MPP 编码器文档为依据。

## 参考依据

* ZLMediaKit：`H265Frame` 按首 NAL 分类（`H265_TYPE(x)=((x)>>1)&0x3f`），`VPS=32/SPS=33/PPS=34/AUD=35/SEI 前缀=39/后缀=40/IRAP=16..23（其中 IDR=19,20,21）`；未就绪或首 NAL 为参数集/SEI 时按 start-code 切分，`inputFrame_l` 逐 NAL 处理。本项目为 ST22 整帧传输，无需 RTP FU 分片，只借用其 AnnexB 切分与分类规则。
* MPP：编码器通过 `MppEncCfg` 字符串键配置，H264 用 `h264:profile/level/stream_type`，H265 用 `h265:profile/tier/level` 等；`mpp_init` 编码类型需与 `codec:type` 一致。FFmpeg 复用侧按编码类型选择 `h264_mp4toannexb` 或 `hevc_mp4toannexb`（HVCC 转 AnnexB）。

## 目标与非目标

目标：编码发送侧可选 H265，文件源 H265 MP4 可发送，接收解码侧可解 H265，端到端 SEI 时间戳可用。

非目标：多码流混合合成按编码自适应（合成器本期保持 H264 默认，后续由信令携带编码时再扩展）；H265 VUI 色彩重写（本期直通，依赖 MPP 与采集端默认）；B 帧与 SVC 分层。

## 方案

编码器按配置选择 MPP 编码类型，H265 使用主档配置并保持与现有 H264 相同的码控、GOP、QP 热更新路径。SEI 时间戳保持 25 字节载荷不变，仅 NAL 头区分：H264 沿用单字节 `06`，H265 使用双字节前缀 SEI 头。参数集处理区分 AVCC 与 HVCC：创建时自适应解析为 AnnexB，IDR 拼接与预处理按编码类型识别关键帧。解复用按流编码选择对应 BSF。接收桥对两种 SEI 都尝试剥离。发送侧 H265 跳过 H264 专用的 SPS VUI 重写。单机 TX 配置与文件播放管线新增编码选项，缺省保持 H264。

## 验收

编码侧产出可用 HEVC AnnexB 码流并经 ST22 整帧传输，解码侧正常出帧；H264 回归行为不变；未知编码仍明确拒绝。

## 后续

合成器按流编码选择解码器；H265 VUI 色彩按需补齐；能力协商失败路径的端到端提示。
