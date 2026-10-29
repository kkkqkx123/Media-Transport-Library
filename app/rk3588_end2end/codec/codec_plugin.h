/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Abstract codec plugin interface.
 * Encoder: NV12 dma-buf → H.264 coded packets
 * Decoder: H.264 coded packets → NV12 dma-buf
 */

#ifndef _E2E_CODEC_PLUGIN_H_
#define _E2E_CODEC_PLUGIN_H_

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
  int intra_refresh_mode;  /* 0=off, 1=MB-row, 2=MB-col, 3=MB-gap */
  int intra_refresh_arg;   /* rows/cols/gap count per frame */
};

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

  /** Optional (NULL allowed): last encoder output time (CLOCK_MONOTONIC ns)
   *  for hang supervision. Implementation must not block. */
  int  (*get_last_output_ns)(void* ctx, uint64_t* ns);

  /** Optional (NULL allowed): frames dropped because the internal output
   * ring was full. Implementation must not block. */
  int  (*get_full_drops)(void* ctx, uint64_t* count);
};

#endif
