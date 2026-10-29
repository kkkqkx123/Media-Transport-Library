/* SPDX-License-Identifier: BSD-3-Clause
 * MPP H.264 encoder (MPP 1.5.0). NV12 dma-buf → H.264 Annex B packets.
 *
 * Async model (mirrors zlmediakit HdmiInAsyncEncoder):
 *   - send()  : Feeder path — import dma-buf, put_frame, return immediately.
 *   - Collector thread: drain encode_get_packet with a bounded timeout (the
 *     single output-timeout entry point), assemble packets until EOI into a
 *     slab buffer, push into a bounded frame ring (drop-on-full + counter).
 *   - recv()/recv_into(): pop from the ring; -EAGAIN when empty.
 * This removes the in-thread busy-wait that previously stalled the encode
 * thread for up to 10ms per frame inside a 16.7ms frame budget.
 *
 * Hang-guard bookkeeping (last_output_ns) is maintained here; supervision
 * lives in the pipeline layer (see docs/plan/RK3588_END2END_IMPROVEMENT_PLAN.md).
 */

#include "codec_plugin.h"
#include "../hal/slab_pool.h"
#include <errno.h>
#include <pthread.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ENC_RING_DEPTH 8

struct enc_frame {
  uint8_t* data;   /* slab-allocated */
  size_t   size;
  int      is_key;
};

struct enc_ring {
  struct enc_frame slot[ENC_RING_DEPTH];
  int  head, tail, count;
  struct slab_pool* pool; /* for freeing dropped/leftover frames */
  pthread_mutex_t lock;
  pthread_cond_t  not_empty;
  atomic_int full_drops;
  /* RC hot-reconfig mailbox: collector applies it serially with
   * encode_put_frame/encode_get_packet (MPP_ENC_SET_CFG must not race) */
  struct {
    pthread_mutex_t lock;
    bool            pending;
    int32_t         bitrate;
    int32_t         gop;
  } rc;
};

struct mpp_enc {
  MppCtx ctx; MppApi* api; MppEncCfg cfg;
  struct slab_pool* pool;
  int w, h, gop, bitrate;
  int intra_refresh_mode, intra_refresh_arg;
  int frames_out;  /* completed frames emitted to the ring */

  struct enc_ring ring;
  pthread_t collector;
  atomic_bool running;
  atomic_ullong last_output_ns; /* hang-guard bookkeeping */
};

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---- bounded frame ring ---- */

static void ring_init(struct enc_ring* r, struct slab_pool* pool) {
  memset(r, 0, sizeof(*r));
  r->pool = pool;
  pthread_mutex_init(&r->lock, NULL);
  pthread_cond_init(&r->not_empty, NULL);
  pthread_mutex_init(&r->rc.lock, NULL);
  r->rc.pending = false;
}

static void ring_deinit(struct enc_ring* r) {
  pthread_mutex_lock(&r->lock);
  for (int i = 0; i < r->count; i++) {
    struct enc_frame* f = &r->slot[(r->head + i) % ENC_RING_DEPTH];
    if (f->data) slab_free(r->pool, f->data);
    f->data = NULL;
  }
  r->count = 0;
  pthread_mutex_unlock(&r->lock);
  pthread_mutex_destroy(&r->lock);
  pthread_cond_destroy(&r->not_empty);
  pthread_mutex_destroy(&r->rc.lock);
}

/* Takes ownership of f->data. Returns 0 on success; on full, drops the
 * frame (frees data) and bumps full_drops — never blocks the collector. */
static int ring_push(struct enc_ring* r, struct enc_frame* f) {
  pthread_mutex_lock(&r->lock);
  if (r->count == ENC_RING_DEPTH) {
    r->full_drops++;
    pthread_mutex_unlock(&r->lock);
    slab_free(r->pool, f->data);
    return -EAGAIN;
  }
  r->slot[r->tail] = *f;
  r->tail = (r->tail + 1) % ENC_RING_DEPTH;
  r->count++;
  pthread_cond_signal(&r->not_empty);
  pthread_mutex_unlock(&r->lock);
  return 0;
}

/* Pop with bounded wait. Returns 1 with frame filled, -EAGAIN on timeout. */
static int ring_pop(struct enc_ring* r, struct enc_frame* f, int timeout_us) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += (long)timeout_us * 1000;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&r->lock);
  while (r->count == 0) {
    int rc = pthread_cond_timedwait(&r->not_empty, &r->lock, &ts);
    if (rc != 0) {
      pthread_mutex_unlock(&r->lock);
      return -EAGAIN;
    }
  }
  *f = r->slot[r->head];
  r->head = (r->head + 1) % ENC_RING_DEPTH;
  r->count--;
  pthread_mutex_unlock(&r->lock);
  return 1;
}

/* ---- collector ---- */

static int collect_packet(struct mpp_enc* e, MppPacket* pkt_out) {
  MppPacket pkt = NULL;
  MPP_RET r = e->api->encode_get_packet(e->ctx, &pkt);
  if (r == MPP_OK && pkt) {
    *pkt_out = pkt;
    return 0;
  }
  return -EAGAIN;
}

/* idle sleep between empty get_packet polls: encode_get_packet returns
 * immediately when no packet is ready, so a bare loop would busy-spin */
#define COLLECT_IDLE_SLEEP_US 500

/* Apply pending RC reconfig from the mailbox — collector thread only, so
 * MPP_ENC_SET_CFG never races encode_put_frame/encode_get_packet. */
static void apply_pending_rc(struct mpp_enc* e) {
  pthread_mutex_lock(&e->ring.rc.lock);
  if (!e->ring.rc.pending) {
    pthread_mutex_unlock(&e->ring.rc.lock);
    return;
  }
  int32_t bitrate = e->ring.rc.bitrate;
  int32_t gop     = e->ring.rc.gop;
  e->ring.rc.pending = false;
  pthread_mutex_unlock(&e->ring.rc.lock);

  /* e->cfg keeps the full initial config; only the RC fields change */
  mpp_enc_cfg_set_s32(e->cfg, "rc:bps_target", bitrate);
  mpp_enc_cfg_set_s32(e->cfg, "rc:bps_min", (int32_t)(bitrate * 0.9f));
  mpp_enc_cfg_set_s32(e->cfg, "rc:bps_max", (int32_t)(bitrate * 1.1f));
  mpp_enc_cfg_set_s32(e->cfg, "rc:gop", gop);
  MPP_RET r = e->api->control(e->ctx, MPP_ENC_SET_CFG, e->cfg);
  if (r)
    fprintf(stderr, "mpp_enc: RC reconfig failed %d\n", r);
  else
    printf("mpp_enc: RC reconfig %dbps gop=%d\n", bitrate, gop);
}

static void* collector_thread(void* arg) {
  struct mpp_enc* e = (struct mpp_enc*)arg;

  while (atomic_load(&e->running)) {
    apply_pending_rc(e);

    MppPacket pkt = NULL;
    if (collect_packet(e, &pkt) != 0) {
      /* no packet ready: bounded idle sleep, loop re-checks running */
      usleep(COLLECT_IDLE_SLEEP_US);
      continue;
    }

    /* assemble packets until EOI into one slab buffer */
    size_t cap = slab_blk_size(e->pool);
    uint8_t* out = slab_alloc(e->pool);
    if (!out) {
      mpp_packet_deinit(&pkt);
      continue;
    }
    size_t total = 0;
    int is_key = 0;
    MPP_RET r = MPP_OK;
    do {
      size_t sz = mpp_packet_get_length(pkt);
      void*  pd = mpp_packet_get_pos(pkt);
      if (!pd) pd = mpp_packet_get_data(pkt);
      if (pd && sz) {
        if (mpp_packet_has_meta(pkt)) {
          MppMeta meta = mpp_packet_get_meta(pkt);
          RK_S32 intra = 0;
          if (mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &intra) == MPP_OK && intra)
            is_key = 1;
        }
        if (total + sz <= cap) {
          memcpy(out + total, pd, sz);
          total += sz;
        } else {
          fprintf(stderr, "mpp_enc: frame exceeds slab block, dropped\n");
          total = 0;
        }
      }
      MPP_RET eoi = mpp_packet_is_partition(pkt) ? mpp_packet_is_eoi(pkt) : 1;
      mpp_packet_deinit(&pkt);
      pkt = NULL;
      if (total == 0) break; /* overflow — discard this frame */
      if (eoi) break;
      r = collect_packet(e, &pkt);
      if (r != 0) break; /* stream hiccup; emit what we have */
    } while (1);

    if (total == 0) {
      slab_free(e->pool, out);
      continue;
    }

    struct enc_frame f = {.data = out, .size = total, .is_key = is_key};
    if (ring_push(&e->ring, &f) == 0) {
      e->frames_out++;
      atomic_store(&e->last_output_ns, now_ns());
    }
  }
  return NULL;
}

/* ---- plugin callbacks ---- */

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
  if (r) {
    fprintf(stderr, "mpp_enc: cfg err %d\n", r);
    mpp_enc_cfg_deinit(cfg);
    mpp_destroy(e->ctx);
    free(e);
    return -EIO;
  }
  /* keep the full initial config for RC hot-reconfig (apply_pending_rc) */
  e->cfg = cfg;

  {
    MppEncHeaderMode hdr_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    e->api->control(e->ctx, MPP_ENC_SET_HEADER_MODE, &hdr_mode);
  }

  ring_init(&e->ring, NULL); /* pool wired in mpp_encoder_create */
  atomic_init(&e->running, false);
  atomic_store(&e->last_output_ns, now_ns());
  *out = e;
  printf("mpp_enc: %dx%d gop=%d %dbps ready\n", e->w, e->h, e->gop, e->bitrate);
  return 0;
}

static int enc_start(void* ctx) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  if (!e) return 0;
  if (atomic_exchange(&e->running, true)) return 0;
  return pthread_create(&e->collector, NULL, collector_thread, e);
}

static void enc_stop(void* ctx) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  if (!e) return;
  if (!atomic_exchange(&e->running, false)) return;
  pthread_join(e->collector, NULL);
  e->collector = 0;
}

static void enc_free(void* ctx) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  if (e) {
    enc_stop(e);
    ring_deinit(&e->ring);
    if (e->cfg) mpp_enc_cfg_deinit(e->cfg);
    e->api->reset(e->ctx);
    mpp_destroy(e->ctx);
    free(e);
  }
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

static int enc_recv(void* ctx, uint8_t** data, size_t* size, int* is_key) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  struct enc_frame f;
  int r = ring_pop(&e->ring, &f, 1000);
  if (r != 1) return r;
  *data = f.data;
  *size = f.size;
  *is_key = f.is_key;
  return 1;
}

static int enc_recv_into(void* ctx, uint8_t* dst, size_t cap, size_t* size,
                         int* is_key) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  struct enc_frame f;
  int r = ring_pop(&e->ring, &f, 1000);
  if (r != 1) return r;
  if (f.size > cap) {
    slab_free(e->pool, f.data);
    return -ENOSPC;
  }
  memcpy(dst, f.data, f.size);
  slab_free(e->pool, f.data);
  *size = f.size;
  *is_key = f.is_key;
  return 1;
}

static int enc_set_param(void* ctx, enum codec_param k, int v) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  if (!e) return -EINVAL;
  switch (k) {
  case CODEC_PARAM_BITRATE:
    if (v <= 0) return -EINVAL;
    pthread_mutex_lock(&e->ring.rc.lock);
    e->ring.rc.bitrate = v;
    e->ring.rc.pending = true;
    pthread_mutex_unlock(&e->ring.rc.lock);
    return 0;
  case CODEC_PARAM_GOP_SIZE:
    if (v <= 0) return -EINVAL;
    pthread_mutex_lock(&e->ring.rc.lock);
    e->ring.rc.gop = v;
    e->ring.rc.pending = true;
    pthread_mutex_unlock(&e->ring.rc.lock);
    return 0;
  default:
    return -ENOSYS;
  }
}

static int enc_get_last_output_ns(void* ctx, uint64_t* ns) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  if (!e || !ns) return -EINVAL;
  *ns = atomic_load(&e->last_output_ns);
  return 0;
}

static int enc_get_full_drops(void* ctx, uint64_t* count) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  if (!e || !count) return -EINVAL;
  *count = (uint64_t)atomic_load(&e->ring.full_drops);
  return 0;
}

struct codec_plugin* mpp_encoder_create(struct codec_config* cfg,
                                         struct slab_pool* coded_pool) {
  struct codec_plugin* p = calloc(1, sizeof(*p));
  struct mpp_enc* e = NULL;
  if (!p || enc_init((void**)&e, cfg)) { free(p); return NULL; }
  e->pool = coded_pool;
  e->ring.pool = coded_pool; /* ring uses the coded slab for drops */
  p->priv  = e;
  p->name  = "mpp_encoder";
  p->init  = enc_init;
  p->start = enc_start; p->stop = enc_stop; p->free = enc_free;
  p->send  = enc_send;
  p->recv  = enc_recv;
  p->recv_into = enc_recv_into;
  p->set_param = enc_set_param;
  p->get_last_output_ns = enc_get_last_output_ns;
  p->get_full_drops = enc_get_full_drops;
  p->caps  = (struct codec_caps){ .fmts={CODEC_FMT_H264}, .min_w=64,.max_w=4096,
      .min_h=64,.max_h=2160, .supports_dmabuf=true };
  return p;
}
