#ifndef _RK3588_COMMON_CODEC_PLUGIN_H_
#define _RK3588_COMMON_CODEC_PLUGIN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum codec_format { CODEC_FMT_H264, CODEC_FMT_H265 };

struct codec_caps {
  enum codec_format fmts[4];
  int min_w, max_w, min_h, max_h;
  bool supports_dmabuf;
  int max_b_frames;
};

enum codec_param {
  CODEC_PARAM_BITRATE,
  CODEC_PARAM_GOP_SIZE,
  CODEC_PARAM_B_FRAMES,
  CODEC_PARAM_LOW_LATENCY,
  CODEC_PARAM_NUM_SLICES,
  CODEC_PARAM_QP,
};

struct codec_config {
  enum codec_format format;
  int width, height;
  float fps;
  int bitrate_bps;
  int gop_size;
  int b_frames;
  bool low_latency;
  int num_slices;
  /* end2end extension; default 0 = off */
  int intra_refresh_mode;  /* 0=off, 1=MB-row, 2=MB-col, 3=MB-gap */
  int intra_refresh_arg;   /* rows/cols/gap count per frame */
  /* V4L2 pixel format of the input dma-buf (e.g. V4L2_PIX_FMT_BGR24);
   * 0 = default NV12. Encoders use it to select prep:format directly. */
  uint32_t input_fourcc;
};

struct codec_encode_stats;

struct codec_plugin {
  const char* name;
  struct codec_caps caps;
  void* priv;  /* opaque impl context */

  int  (*init)  (void** ctx, struct codec_config* cfg);
  int  (*start) (void* ctx);
  void (*stop)  (void* ctx);
  void (*free)  (void* ctx);

  /* send input (non-blocking), recv output (may block) */
  int  (*send)  (void* ctx, int dma_fd, uint32_t stride);
  int  (*recv)  (void* ctx, uint8_t** data, size_t* size,
                 int* is_keyframe);
  /** Write encoded output into caller buffer (avoids extra slab alloc). */
  int  (*recv_into)(void* ctx, uint8_t* dst, size_t cap, size_t* size,
                    int* is_keyframe);

  int  (*set_param)(void* ctx, enum codec_param k, int v);

  /** Optional (NULL allowed): last encoder output time (CLOCK_MONOTONIC ns). */
  int  (*get_last_output_ns)(void* ctx, uint64_t* ns);

  /** Optional (NULL allowed): frames dropped because internal output ring full. */
  int  (*get_full_drops)(void* ctx, uint64_t* count);

  /** Optional (NULL allowed): one-shot encode observability snapshot. */
  int  (*get_encode_stats)(void* ctx, struct codec_encode_stats* out);
  /** Optional (NULL allowed): take and clear the windowed packet counter. */
  int  (*take_packets_in_window)(void* ctx, uint64_t* n);
  /** Optional (NULL allowed): read back hot-reconfig state. */
  int  (*get_rc_state)(void* ctx, int* bitrate, int* gop, int* qp,
                        int* pending);
};

/** Encode-side observability snapshot (lock-free reads, cumulative unless noted). */
struct codec_encode_stats {
  uint32_t encode_fps;      /* frames emitted in the last 1s window */
  uint32_t encode_cost_us;  /* submit-to-output cost EMA */
  uint32_t latency_p50_us;  /* windowed percentiles over recent frames */
  uint32_t latency_p90_us;
  uint32_t latency_p99_us;
  int32_t  avg_qp;          /* -1 when the driver reports no meta */
  int32_t  start_qp;
  int32_t  bps_rt;
  int64_t  sse;
  int32_t  pskip;
  uint32_t mpp_buf_now;     /* global MPP buffer usage in bytes */
  uint32_t mpp_buf_peak;
  uint64_t packets_total;   /* frames ever emitted to the output ring */
  uint64_t oversize_drops;  /* frames dropped for exceeding slab block */
  uint64_t meta_missing;    /* frames emitted without MPP meta */
};

#endif /* _RK3588_COMMON_CODEC_PLUGIN_H_ */
