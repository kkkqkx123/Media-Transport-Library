#include "codec_plugin.h"
#include "slab_pool.h"
#include <errno.h>
#include <pthread.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>
#include <stdatomic.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define ENC_RING_DEPTH 8
#define ENC_LAT_WIN 300
#define ENC_SUBMIT_Q 64

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
   * encode_put_frame/encode_get_packet (MPP_ENC_SET_CFG must not race).
   * Fields always hold the last applied values so a partial update still
   * reapplies a fully valid config. */
  struct {
    pthread_mutex_t lock;
    bool            pending;
    int32_t         bitrate;
    int32_t         gop;
    int32_t         qp;
  } rc;
};

struct mpp_enc {
  MppCtx ctx; MppApi* api; MppEncCfg cfg;
  struct slab_pool* pool;
  int w, h, gop, bitrate, qp_init;
  int intra_refresh_mode, intra_refresh_arg;
  bool input_bgr; /* V4L2 BGR3 input: encode BGR888 directly, no RGA */
  int frames_out;  /* completed frames emitted to the ring */

  struct enc_ring ring;
  pthread_t collector;
  atomic_bool running;
  atomic_ullong last_output_ns; /* hang-guard bookkeeping */
  /* observability (atomics for lock-free reads from supervisor) */
  atomic_uint encode_fps;
  atomic_uint encode_cost_us;
  atomic_int last_avg_qp;
  atomic_int last_start_qp;
  atomic_int last_bps_rt;
  atomic_long last_sse;
  atomic_int last_pskip;
  atomic_uint mpp_buf_now;
  atomic_uint mpp_buf_peak;
  atomic_ullong packets_in_window;
  atomic_ullong packets_total;
  atomic_ullong oversize_drops;
  atomic_ullong meta_missing;
  uint64_t fps_win_start_ms;
  uint32_t fps_win_count;
  pthread_mutex_t lat_lock;
  uint32_t lat_win[ENC_LAT_WIN];
  int lat_count;
  pthread_mutex_t submit_lock;
  int64_t submit_q[ENC_SUBMIT_Q];
  int submit_head, submit_tail, submit_count;
};;

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline int64_t now_us(void) { return (int64_t)(now_ns() / 1000ULL); }

static inline uint64_t now_ms(void) { return now_ns() / 1000000ULL; }

static void submit_push(struct mpp_enc* e, int64_t t) {
  pthread_mutex_lock(&e->submit_lock);
  if (e->submit_count < ENC_SUBMIT_Q) {
    e->submit_q[e->submit_tail] = t;
    e->submit_tail = (e->submit_tail + 1) % ENC_SUBMIT_Q;
    e->submit_count++;
  } else {
    e->submit_q[e->submit_head] = t;
    e->submit_head = (e->submit_head + 1) % ENC_SUBMIT_Q;
    e->submit_tail = (e->submit_head + e->submit_count) % ENC_SUBMIT_Q;
  }
  pthread_mutex_unlock(&e->submit_lock);
}

static bool submit_pop(struct mpp_enc* e, int64_t* t) {
  bool ok = false;
  pthread_mutex_lock(&e->submit_lock);
  if (e->submit_count > 0) {
    *t = e->submit_q[e->submit_head];
    e->submit_head = (e->submit_head + 1) % ENC_SUBMIT_Q;
    e->submit_count--;
    ok = true;
  }
  pthread_mutex_unlock(&e->submit_lock);
  return ok;
}

static void latency_record(struct mpp_enc* e, uint32_t v) {
  pthread_mutex_lock(&e->lat_lock);
  if (e->lat_count < ENC_LAT_WIN) {
    e->lat_win[e->lat_count++] = v;
  } else {
    memmove(&e->lat_win[0], &e->lat_win[1],
            (size_t)(ENC_LAT_WIN - 1) * sizeof(uint32_t));
    e->lat_win[ENC_LAT_WIN - 1] = v;
  }
  pthread_mutex_unlock(&e->lat_lock);
}

static void latency_percentiles(struct mpp_enc* e, uint32_t* p50,
                                uint32_t* p90, uint32_t* p99) {
  uint32_t tmp[ENC_LAT_WIN];
  int n = 0;
  pthread_mutex_lock(&e->lat_lock);
  n = e->lat_count;
  if (n > 0) memcpy(tmp, e->lat_win, (size_t)n * sizeof(uint32_t));
  pthread_mutex_unlock(&e->lat_lock);
  if (n == 0) { *p50 = *p90 = *p99 = 0; return; }
  for (int i = 1; i < n; i++) {
    uint32_t v = tmp[i];
    int j = i;
    while (j > 0 && tmp[j - 1] > v) { tmp[j] = tmp[j - 1]; j--; }
    tmp[j] = v;
  }
  *p50 = tmp[(n * 50) / 100 >= n ? n - 1 : (n * 50) / 100];
  *p90 = tmp[(n * 90) / 100 >= n ? n - 1 : (n * 90) / 100];
  *p99 = tmp[(n * 99) / 100 >= n ? n - 1 : (n * 99) / 100];
}

static void probe_mpp_buffers(struct mpp_enc* e) {
#if defined(__has_include) && __has_include(<rockchip/mpp_buffer.h>)
  {
    extern unsigned int mpp_buffer_total_now(void);
    extern unsigned int mpp_buffer_total_max(void);
    unsigned int cur = mpp_buffer_total_now();
    unsigned int peak = mpp_buffer_total_max();
    atomic_store(&e->mpp_buf_now, cur);
    unsigned int prev = atomic_load(&e->mpp_buf_peak);
    if (peak > prev) atomic_store(&e->mpp_buf_peak, peak);
    if (cur > atomic_load(&e->mpp_buf_peak))
      atomic_store(&e->mpp_buf_peak, cur);
  }
#else
  (void)e;
#endif
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
  int32_t qp      = e->ring.rc.qp;
  e->ring.rc.pending = false;
  pthread_mutex_unlock(&e->ring.rc.lock);

  mpp_enc_cfg_set_s32(e->cfg, "rc:bps_target", bitrate);
  mpp_enc_cfg_set_s32(e->cfg, "rc:bps_min", (int32_t)(bitrate * 0.9f));
  mpp_enc_cfg_set_s32(e->cfg, "rc:bps_max", (int32_t)(bitrate * 1.1f));
  mpp_enc_cfg_set_s32(e->cfg, "rc:gop", gop);
  mpp_enc_cfg_set_s32(e->cfg, "rc:qp_init", qp);
  MPP_RET r = e->api->control(e->ctx, MPP_ENC_SET_CFG, e->cfg);
  if (r)
    fprintf(stderr, "mpp_enc: RC reconfig failed %d\n", r);
  else {
    e->bitrate = bitrate;
    e->gop = gop;
    e->qp_init = qp;
    printf("mpp_enc: RC reconfig %dbps gop=%d qp=%d\n", bitrate, gop, qp);
  }
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
    bool saw_meta = false;
    int32_t avg_qp = -1, start_qp = -1, bps_rt = -1, pskip = -1;
    int64_t sse = -1;
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
          RK_S32 v32 = 0;
          RK_S64 v64 = 0;
          saw_meta = true;
          if (mpp_meta_get_s32(meta, KEY_ENC_AVERAGE_QP, &v32) == MPP_OK)
            avg_qp = v32;
          if (mpp_meta_get_s32(meta, KEY_ENC_START_QP, &v32) == MPP_OK)
            start_qp = v32;
          if (mpp_meta_get_s32(meta, KEY_ENC_BPS_RT, &v32) == MPP_OK)
            bps_rt = v32;
          if (mpp_meta_get_s64(meta, KEY_ENC_SSE, &v64) == MPP_OK) sse = v64;
          if (mpp_meta_get_s32(meta, KEY_OUTPUT_PSKIP, &v32) == MPP_OK)
            pskip = v32;
        }
        if (total + sz <= cap) {
          memcpy(out + total, pd, sz);
          total += sz;
        } else {
          atomic_fetch_add(&e->oversize_drops, 1);
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
      /* drop consumes one submit stamp — keeps submit/output pairing aligned
       * (a stale stamp here would inflate every later frame's cost) */
      int64_t dropped;
      submit_pop(e, &dropped);
      continue;
    }

    struct enc_frame f = {.data = out, .size = total, .is_key = is_key};
    if (ring_push(&e->ring, &f) == 0) {
      int64_t submit_us = 0;
      bool have_submit = submit_pop(e, &submit_us);
      int64_t done_us = now_us();
      if (have_submit && done_us > submit_us) {
        uint32_t cost = (uint32_t)(done_us - submit_us);
        uint32_t prev = atomic_load(&e->encode_cost_us);
        uint32_t ema = prev ? (uint32_t)((prev * 9 + cost + 5) / 10) : cost;
        atomic_store(&e->encode_cost_us, ema);
        latency_record(e, cost);
      }
      if (saw_meta) {
        atomic_store(&e->last_avg_qp, avg_qp);
        atomic_store(&e->last_start_qp, start_qp);
        atomic_store(&e->last_bps_rt, bps_rt);
        atomic_store(&e->last_sse, (long)sse);
        atomic_store(&e->last_pskip, pskip);
      } else {
        atomic_fetch_add(&e->meta_missing, 1);
      }
      e->frames_out++;
      atomic_fetch_add(&e->packets_total, 1);
      atomic_fetch_add(&e->packets_in_window, 1);
      /* 1s encode-fps window (collector thread only) */
      {
        uint64_t ms = now_ms();
        if (e->fps_win_start_ms == 0) e->fps_win_start_ms = ms;
        e->fps_win_count++;
        if (ms - e->fps_win_start_ms >= 1000) {
          atomic_store(&e->encode_fps, e->fps_win_count);
          e->fps_win_start_ms = ms;
          e->fps_win_count = 0;
        }
      }
      probe_mpp_buffers(e);
      atomic_store(&e->last_output_ns, now_ns());
    } else {
      /* dropped frame consumes one submit stamp — keeps submit/output
       * pairing aligned (a stale stamp would inflate later costs) */
      int64_t dropped;
      submit_pop(e, &dropped);
      slab_free(e->pool, f.data);
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
  e->qp_init = 26;
  e->intra_refresh_mode = c->intra_refresh_mode;
  e->intra_refresh_arg  = c->intra_refresh_arg;
  e->input_bgr = (c->input_fourcc == V4L2_PIX_FMT_BGR24);

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
  /* BGR3 input encodes BGR888 natively (validated by debug/mpp_enc_frame_latency
   * --infmt bgr3); NV12 stays the default pipeline format. */
  mpp_enc_cfg_set_s32(cfg, "prep:format",
                      e->input_bgr ? MPP_FMT_BGR888 : MPP_FMT_YUV420SP);
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
  e->ring.rc.bitrate = e->bitrate;
  e->ring.rc.gop = e->gop;
  e->ring.rc.qp = e->qp_init;
  atomic_init(&e->running, false);
  atomic_store(&e->last_output_ns, now_ns());
  atomic_init(&e->encode_fps, 0);
  atomic_init(&e->encode_cost_us, 0);
  atomic_init(&e->last_avg_qp, -1);
  atomic_init(&e->last_start_qp, -1);
  atomic_init(&e->last_bps_rt, -1);
  atomic_init(&e->last_sse, -1);
  atomic_init(&e->last_pskip, -1);
  atomic_init(&e->mpp_buf_now, 0);
  atomic_init(&e->mpp_buf_peak, 0);
  atomic_init(&e->packets_in_window, 0);
  atomic_init(&e->packets_total, 0);
  atomic_init(&e->oversize_drops, 0);
  atomic_init(&e->meta_missing, 0);
  e->fps_win_start_ms = 0;
  e->fps_win_count = 0;
  e->lat_count = 0;
  e->submit_head = e->submit_tail = e->submit_count = 0;
  pthread_mutex_init(&e->lat_lock, NULL);
  pthread_mutex_init(&e->submit_lock, NULL);
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
    pthread_mutex_destroy(&e->lat_lock);
    pthread_mutex_destroy(&e->submit_lock);
    if (e->cfg) mpp_enc_cfg_deinit(e->cfg);
    e->api->reset(e->ctx);
    mpp_destroy(e->ctx);
    free(e);
  }
}

static int enc_send(void* ctx, int dma_fd, uint32_t stride) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;

  /* NV12: stride is pixels/line, buffer is stride*h*3/2.
   * BGR888: V4L2 stride is bytes/line, MPP hor_stride is pixels. */
  uint32_t hor_stride = stride;
  size_t frame_size = (size_t)stride * e->h * 3 / 2;
  MppFrameFormat fmt = MPP_FMT_YUV420SP;
  if (e->input_bgr) {
    if (stride < (uint32_t)e->w * 3 || stride % 3 != 0) return -EINVAL;
    hor_stride = stride / 3;
    frame_size = (size_t)stride * e->h;
    fmt = MPP_FMT_BGR888;
  }

  /* Import external dma-buf fd as MPP buffer (zero-copy).
   * MPP 1.5.0 + RK3588 supports MPP_BUFFER_TYPE_EXT_DMA for fd import. */
  MppBufferInfo info = {
    .type = MPP_BUFFER_TYPE_EXT_DMA,
    .size = frame_size,
    .fd   = dma_fd,
  };
  MppBuffer buf = NULL;
  MPP_RET r = mpp_buffer_import(&buf, &info);
  if (r || !buf) {
    /* fallback to mmap+copy if import fails (e.g. unsupported format) */
    void* src = mmap(NULL, frame_size, PROT_READ, MAP_SHARED, dma_fd, 0);
    if (src == MAP_FAILED) return -EIO;
    r = mpp_buffer_get(NULL, &buf, frame_size);
    if (r) { munmap(src, frame_size); return -EIO; }
    void* dst = mpp_buffer_get_ptr(buf);
    if (dst) memcpy(dst, src, frame_size);
    munmap(src, frame_size);
  }

  MppFrame f = NULL;
  mpp_frame_init(&f);
  mpp_frame_set_width(f, e->w);   mpp_frame_set_height(f, e->h);
  mpp_frame_set_hor_stride(f, hor_stride); mpp_frame_set_ver_stride(f, e->h);
  mpp_frame_set_fmt(f, fmt);
  mpp_frame_set_buffer(f, buf);
  mpp_buffer_put(buf);
  submit_push(e, now_us());

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
  case CODEC_PARAM_QP:
    if (v < 0 || v > 51) return -EINVAL;
    pthread_mutex_lock(&e->ring.rc.lock);
    e->ring.rc.qp = v;
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

static int enc_get_encode_stats(void* ctx, struct codec_encode_stats* out) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  if (!e || !out) return -EINVAL;
  out->encode_fps     = atomic_load(&e->encode_fps);
  out->encode_cost_us = atomic_load(&e->encode_cost_us);
  latency_percentiles(e, &out->latency_p50_us, &out->latency_p90_us,
                      &out->latency_p99_us);
  out->avg_qp      = atomic_load(&e->last_avg_qp);
  out->start_qp    = atomic_load(&e->last_start_qp);
  out->bps_rt      = atomic_load(&e->last_bps_rt);
  out->sse         = (int64_t)atomic_load(&e->last_sse);
  out->pskip       = atomic_load(&e->last_pskip);
  out->mpp_buf_now  = atomic_load(&e->mpp_buf_now);
  out->mpp_buf_peak = atomic_load(&e->mpp_buf_peak);
  out->packets_total  = atomic_load(&e->packets_total);
  out->oversize_drops = atomic_load(&e->oversize_drops);
  out->meta_missing   = atomic_load(&e->meta_missing);
  return 0;
}

static int enc_take_packets(void* ctx, uint64_t* n) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  if (!e || !n) return -EINVAL;
  *n = atomic_exchange(&e->packets_in_window, 0);
  return 0;
}

static int enc_get_rc_state(void* ctx, int* bitrate, int* gop, int* qp,
                            int* pending) {
  struct mpp_enc* e = (struct mpp_enc*)ctx;
  if (!e) return -EINVAL;
  pthread_mutex_lock(&e->ring.rc.lock);
  if (bitrate) *bitrate = e->ring.rc.bitrate;
  if (gop) *gop = e->ring.rc.gop;
  if (qp) *qp = e->ring.rc.qp;
  if (pending) *pending = e->ring.rc.pending ? 1 : 0;
  pthread_mutex_unlock(&e->ring.rc.lock);
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
  p->get_encode_stats = enc_get_encode_stats;
  p->take_packets_in_window = enc_take_packets;
  p->get_rc_state = enc_get_rc_state;
  p->caps  = (struct codec_caps){ .fmts={CODEC_FMT_H264}, .min_w=64,.max_w=4096,
      .min_h=64,.max_h=2160, .supports_dmabuf=true };
  return p;
}
