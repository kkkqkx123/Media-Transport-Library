/* SPDX-License-Identifier: BSD-3-Clause
 * MPP H.264 encoder (MPP 1.5.0). NV12 dma-buf → H.264 Annex B packets.
 */

#include "codec_plugin.h"
#include "../hal/slab_pool.h"
#include <errno.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct mpp_enc {
  MppCtx ctx; MppApi* api; MppEncCfg cfg;
  struct slab_pool* pool;
  int w, h, gop, bitrate;
  int intra_refresh_mode, intra_refresh_arg;
  int frames_out;  /* completed recv_into count (cold-start tuning) */
};

static int enc_init(void** out, struct codec_config* c) {
  struct mpp_enc* e = calloc(1, sizeof(*e));
  if (!e) return -ENOMEM;
  e->w = c->width; e->h = c->height;
  e->gop = c->gop_size ? c->gop_size : 60;
  e->bitrate = c->bitrate_bps;
  e->intra_refresh_mode = c->intra_refresh_mode;
  e->intra_refresh_arg  = c->intra_refresh_arg;

  MPP_RET r = mpp_create(&e->ctx, &e->api);
  if (r) { free(e); return -EIO; }

  r = mpp_init(e->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
  if (r) { mpp_destroy(e->ctx); free(e); return -EIO; }

  /* config via MPP API (MPP 1.5.0 uses MppEncCfg from rk_venc_cfg.h) */
  MppEncCfg cfg = NULL;
  mpp_enc_cfg_init(&cfg);
  mpp_enc_cfg_set_s32(cfg, "prep:width",  e->w);
  mpp_enc_cfg_set_s32(cfg, "prep:height", e->h);
  mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", e->w);
  mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", e->h);
  mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
  /* framerate (required) */
  int fps_num = (int)(c->fps + 0.5f);
  mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps_num);
  mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
  mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps_num);
  mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
  /* color range: mirror HDMI source (full range) */
  mpp_enc_cfg_set_s32(cfg, "prep:range", 0); /* 0=full range */
  /* rate control */
  mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
  mpp_enc_cfg_set_s32(cfg, "rc:bps_target", e->bitrate);
  mpp_enc_cfg_set_s32(cfg, "rc:bps_min", (int)(e->bitrate * 0.9f));
  mpp_enc_cfg_set_s32(cfg, "rc:bps_max", (int)(e->bitrate * 1.1f));
  /* QP constraints — tighter I-frame ceiling and smoother quality */
  mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 26);
  mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 10);
  mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 38);
  mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 35);
  mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 10);
  mpp_enc_cfg_set_s32(cfg, "rc:gop", e->gop);
  /* codec */
  mpp_enc_cfg_set_s32(cfg, "h264:profile", 66);
  mpp_enc_cfg_set_s32(cfg, "h264:level", 42); /* 4.2 for 1080p60 */
  mpp_enc_cfg_set_s32(cfg, "h264:stream_type", 0); /* Annex B start codes */

  r = e->api->control(e->ctx, MPP_ENC_SET_CFG, cfg);
  mpp_enc_cfg_deinit(cfg);
  if (r) { fprintf(stderr,"mpp_enc: cfg err %d\n",r); mpp_destroy(e->ctx); free(e); return -EIO; }

  {
    MppEncHeaderMode hdr_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    e->api->control(e->ctx, MPP_ENC_SET_HEADER_MODE, &hdr_mode);
  }

  *out = e;
  printf("mpp_enc: %dx%d gop=%d %dbps ready\n", e->w, e->h, e->gop, e->bitrate);
  return 0;
}

static int enc_start(void* ctx) { (void)ctx; return 0; }
static void enc_stop(void* ctx) { (void)ctx; }
static void enc_free(void* ctx) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  if (e) { e->api->reset(e->ctx); mpp_destroy(e->ctx); free(e); }
}

static int enc_send(void* ctx, int dma_fd, uint32_t stride) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;

  size_t nv12_size = stride * e->h * 3 / 2;

  /* Import external dma-buf fd as MPP buffer (zero-copy).
   * MPP 1.5.0 + RK3588 supports MPP_BUFFER_TYPE_EXT_DMA for fd import. */
  MppBufferInfo info = {
    .type = MPP_BUFFER_TYPE_EXT_DMA,
    .size = nv12_size,
    .fd   = dma_fd,
  };
  MppBuffer buf = NULL;
  MPP_RET r = mpp_buffer_import(&buf, &info);
  if (r || !buf) {
    /* fallback to mmap+copy if import fails (e.g. unsupported format) */
    void* src = mmap(NULL, nv12_size, PROT_READ, MAP_SHARED, dma_fd, 0);
    if (src == MAP_FAILED) return -EIO;
    r = mpp_buffer_get(NULL, &buf, nv12_size);
    if (r) { munmap(src, nv12_size); return -EIO; }
    void* dst = mpp_buffer_get_ptr(buf);
    if (dst) memcpy(dst, src, nv12_size);
    munmap(src, nv12_size);
  }

  MppFrame f = NULL;
  mpp_frame_init(&f);
  mpp_frame_set_width(f, e->w);   mpp_frame_set_height(f, e->h);
  mpp_frame_set_hor_stride(f, stride); mpp_frame_set_ver_stride(f, e->h);
  mpp_frame_set_fmt(f, MPP_FMT_YUV420SP);
  mpp_frame_set_buffer(f, buf);
  mpp_buffer_put(buf);

  r = e->api->encode_put_frame(e->ctx, f);
  mpp_frame_deinit(&f);
  return (r == MPP_OK) ? 0 : -EAGAIN;
}

/* Short busy-wait for encode_get_packet after put_frame (MPP is async). */
static inline void enc_busy_wait_us(int us) {
  for (int i = 0; i < us; i++)
    __asm__ volatile("yield" ::: "memory");
}

static int enc_get_packet_retry(struct mpp_enc* e, MppPacket* pkt_out,
                                int step_us, int max_us) {
  for (int waited = 0; waited <= max_us; waited += step_us) {
    MppPacket pkt = NULL;
    MPP_RET r = e->api->encode_get_packet(e->ctx, &pkt);
    if (r == MPP_OK && pkt) {
      *pkt_out = pkt;
      return 0;
    }
    enc_busy_wait_us(step_us);
  }
  return -EAGAIN;
}

/* Drop any queued packets so the next put_frame does not inherit stale data. */
static void enc_drain_discard(struct mpp_enc* e) {
  MppPacket pkt = NULL;
  while (enc_get_packet_retry(e, &pkt, 50, 200) == 0) {
    mpp_packet_deinit(&pkt);
    pkt = NULL;
  }
}

static int enc_append_packet(MppPacket pkt, uint8_t* dst, size_t cap, size_t* total,
                             int* is_key) {
  size_t sz = mpp_packet_get_length(pkt);
  void*  pd = mpp_packet_get_pos(pkt);
  if (!pd)
    pd = mpp_packet_get_data(pkt);
  if (!pd || !sz)
    return 0;

  if (mpp_packet_has_meta(pkt)) {
    MppMeta meta = mpp_packet_get_meta(pkt);
    RK_S32 intra = 0;
    if (mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &intra) == MPP_OK && intra)
      *is_key = 1;
  }

  if (*total + sz > cap)
    return -ENOSPC;
  memcpy(dst + *total, pd, sz);
  *total += sz;
  return 1;
}

static int enc_recv_into(void* ctx, uint8_t* dst, size_t cap, size_t* size,
                         int* is_key) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  MppPacket pkt = NULL;
  size_t total = 0;
  RK_U32 eoi = 0;

  *is_key = 0;
  if (!dst || !size) return -EINVAL;

  /* mpi_enc_test pattern: drain all packets until EOI before next put_frame. */
  do {
    int wait_us = (total == 0) ? ((e->frames_out < 5) ? 30000 : 20000)
                               : ((total < 32 * 1024) ? 30000 : 8000);
    if (enc_get_packet_retry(e, &pkt, 100, wait_us) != 0) {
      enc_drain_discard(e);
      return -EAGAIN;
    }

    int ar = enc_append_packet(pkt, dst, cap, &total, is_key);
    if (mpp_packet_is_partition(pkt))
      eoi = mpp_packet_is_eoi(pkt);
    else
      eoi = 1;
    mpp_packet_deinit(&pkt);
    pkt = NULL;
    if (ar < 0) {
      enc_drain_discard(e);
      return -ENOSPC;
    }
  } while (!eoi);

  if (total == 0) {
    enc_drain_discard(e);
    return -EAGAIN;
  }

  e->frames_out++;
  *size = total;
  return 1;
}

static int enc_recv(void* ctx, uint8_t** data, size_t* size, int* is_key) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  uint8_t* out = slab_alloc(e->pool);
  if (!out) return -ENOMEM;
  size_t sz = 0;
  int r = enc_recv_into(ctx, out, slab_blk_size(e->pool), &sz, is_key);
  if (r != 1) {
    slab_free(e->pool, out);
    return r;
  }
  *data = out;
  *size = sz;
  return 1;
}

static int enc_set_param(void* ctx, enum codec_param k, int v) {
  (void)ctx; (void)k; (void)v; return -ENOSYS;
}

struct codec_plugin* mpp_encoder_create(struct codec_config* cfg,
                                         struct slab_pool* coded_pool) {
  struct codec_plugin* p = calloc(1, sizeof(*p));
  struct mpp_enc* e = NULL;
  if (!p || enc_init((void**)&e, cfg)) { free(p); return NULL; }
  e->pool = coded_pool;
  p->priv  = e;
  p->name  = "mpp_encoder";
  p->init  = enc_init;
  p->start = enc_start; p->stop = enc_stop; p->free = enc_free;
  p->send  = enc_send;
  p->recv  = enc_recv;
  p->recv_into = enc_recv_into;
  p->set_param = enc_set_param;
  p->caps  = (struct codec_caps){ .fmts={CODEC_FMT_H264}, .min_w=64,.max_w=4096,
      .min_h=64,.max_h=2160, .supports_dmabuf=true };
  return p;
}
