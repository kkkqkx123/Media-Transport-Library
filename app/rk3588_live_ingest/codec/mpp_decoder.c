#include "codec_plugin.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_vdec_cfg.h>
#include <rockchip/rk_vdec_cmd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- internal state ---- */
#define FB_DELAY 8  /* hold 8 frames (~133ms), far exceeding DRM vblank+page flip */

/* Idle sleep between decode_get_frame polls. encode_get_packet-style MPP
 * getters return immediately when no frame is ready; the old 50us x30 tight
 * spin burned a core per frame, stacking past a frame period under high
 * slice counts. 500us x3 keeps the ~1.5ms budget but yields the CPU, the
 * same discipline as the TX collector idle sleep. */
#define DEC_POLL_IDLE_US  500
#define DEC_POLL_RETRIES  3

struct mpp_dec {
  MppCtx          ctx;
  MppApi*         api;
  int             w, h;
  MppBufferGroup  frm_grp;       /* decoder output buffer group */
  MppFrame        held[FB_DELAY]; /* delayed-release ring: keep fb alive for DRM */
  int             held_idx;       /* next write position in held[] */
  bool            info_change_done;
  int             frame_count;
  /* supervision stats (polled by the RX 1s aggregation / hang guard) */
  uint64_t        stat_input_full_drops; /* input-full drains that lost a frame */
  uint64_t        dec_ema_us;            /* EMA of successful take-frame cost */
  uint64_t        last_output_ns;        /* CLOCK_MONOTONIC of last frame out */
  /* resolution change reported by info-change, consumed by the pipeline */
  bool            resolution_pending;
  int             pending_w, pending_h;
};

/* tiny sleep helper (milliseconds) — only for backpressure, not hot path */
static inline void msleep(int ms) { usleep((unsigned)(ms) * 1000); }

static inline uint64_t dec_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---- lifecycle ---- */
static int dec_init(void** out, struct codec_config* c) {
  struct mpp_dec* d = calloc(1, sizeof(*d));
  if (!d) return -ENOMEM;
  d->w = c->width;
  d->h = c->height;

  MPP_RET r = mpp_create(&d->ctx, &d->api);
  if (r) { free(d); return -EIO; }

  /* disable deinterlace — progressive content doesn't need it,
   * and enabling it on progressive frames causes ghosting artifacts */
  {
    RK_U32 disable = 0;
    d->api->control(d->ctx, MPP_DEC_SET_ENABLE_DEINTERLACE, &disable);
  }

  r = mpp_init(d->ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
  if (r) { mpp_destroy(d->ctx); free(d); return -EIO; }

  /* configure decoder: enable internal stream splitting (split_parse).
   * Without this the decoder expects pre-split frames and may stall. */
  MppDecCfg cfg = NULL;
  mpp_dec_cfg_init(&cfg);
  r = d->api->control(d->ctx, MPP_DEC_GET_CFG, cfg);
  if (!r) {
    mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
    r = d->api->control(d->ctx, MPP_DEC_SET_CFG, cfg);
    if (r) fprintf(stderr, "mpp_dec" ": SET_CFG failed %d\n", r);
  }
  mpp_dec_cfg_deinit(cfg);

  *out = d;
  d->last_output_ns = dec_now_ns(); /* hang-guard baseline before frame 1 */
  printf("mpp_dec" ": %dx%d ready (split_parse)\n", d->w, d->h);
  return 0;
}

static void dec_free(void* ctx) {
  struct mpp_dec* d = (struct mpp_dec*)ctx;
  if (!d) return;

  /* send EOS to flush pending frames */
  MppPacket eos = NULL;
  mpp_packet_init(&eos, NULL, 0);
  mpp_packet_set_eos(eos);
  d->api->decode_put_packet(d->ctx, eos);
  mpp_packet_deinit(&eos);

  /* drain final frames */
  {
    int times = 50;
    while (times-- > 0) {
      MppFrame f = NULL;
      if (d->api->decode_get_frame(d->ctx, &f) != MPP_OK || !f) {
        msleep(1);
        continue;
      }
      if (!mpp_frame_get_info_change(f)) {
        /* count but don't keep */
        d->frame_count++;
      }
      mpp_frame_deinit(&f);
    }
  }
  printf("mpp_dec" ": flushed, total %d frames\n", d->frame_count);

  for (int i = 0; i < FB_DELAY; i++)
    if (d->held[i]) mpp_frame_deinit(&d->held[i]);
  if (d->frm_grp) {
    mpp_buffer_group_put(d->frm_grp);
    d->frm_grp = NULL;
  }
  d->api->reset(d->ctx);
  mpp_destroy(d->ctx);
  free(d);
}

static int dec_start(void* ctx) { (void)ctx; return 0; }
static void dec_stop(void* ctx) { (void)ctx; }

/* ---- internal helpers ---- */

static int dec_get_frame(struct mpp_dec* d, int* fd_out, uint32_t* hor_stride_out,
                         uint32_t* ver_stride_out);

/*
 * Handle info-change handshake: the decoder tells us what buffer size
 * it needs; we create a buffer group and signal readiness.
 * After this, decode_get_frame will return frames with real buffers.
 */
static int handle_info_change(struct mpp_dec* d, MppFrame frame) {
  RK_U32 width      = mpp_frame_get_width(frame);
  RK_U32 height     = mpp_frame_get_height(frame);
  RK_U32 buf_size   = mpp_frame_get_buf_size(frame);

  printf("mpp_dec" ": info-change: %ux%u buf_size=%u\n", width, height, buf_size);

  /* A size change is reported to the pipeline, which rebuilds the renderer
   * to match (same discipline as TX signal rebuild). Stash first; the
   * pipeline consumes it via mpp_dec_take_resolution. */
  if ((int)width != d->w || (int)height != d->h) {
    d->pending_w = (int)width;
    d->pending_h = (int)height;
    d->resolution_pending = true;
    d->w = (int)width;
    d->h = (int)height;
  }

  /* create internal buffer group (ION type — same as official mpi_dec_test default).
   * On kernel 5.10 without ION, MPP internally falls back to DMA_HEAP. */
  MppBufferGroup grp = NULL;
  MPP_RET r = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
  if (r) {
    fprintf(stderr, "mpp_dec" ": buffer_group_get_internal failed %d\n", r);
    return -EIO;
  }

  /* 24 buffers — same as official demo, enough for 1080p60 pipeline */
  r = mpp_buffer_group_limit_config(grp, buf_size, 24);
  if (r) {
    fprintf(stderr, "mpp_dec" ": limit_config failed %d\n", r);
    mpp_buffer_group_put(grp);
    return -EIO;
  }

  /* hand the buffer group to the decoder */
  r = d->api->control(d->ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp);
  if (r) {
    fprintf(stderr, "mpp_dec" ": SET_EXT_BUF_GROUP failed %d\n", r);
    mpp_buffer_group_put(grp);
    return -EIO;
  }

  /* release old group (if any) and stash new one */
  if (d->frm_grp) mpp_buffer_group_put(d->frm_grp);
  d->frm_grp = grp;

  /* signal decoder that buffers are ready */
  r = d->api->control(d->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
  if (r) {
    fprintf(stderr, "mpp_dec" ": INFO_CHANGE_READY failed %d\n", r);
    return -EIO;
  }

  d->info_change_done = true;
  printf("mpp_dec" ": info-change handshake complete\n");
  return 0;
}

static int dec_put_packet(struct mpp_dec* d, uint8_t* data, size_t size) {
  MppPacket pkt = NULL;
  mpp_packet_init(&pkt, NULL, 0);
  mpp_packet_set_data(pkt, data);
  mpp_packet_set_size(pkt, size);
  mpp_packet_set_pos(pkt, data);
  mpp_packet_set_length(pkt, size);

  MPP_RET r = d->api->decode_put_packet(d->ctx, pkt);
  mpp_packet_deinit(&pkt);

  if (r == MPP_OK) return 0;

  /* Input queue full — drain one decoded frame to make room. The drained
   * frame is decoded but never displayed, so it counts as a decode drop
   * instead of being silently lost. */
  {
    int dummy_fd;
    uint32_t dummy_h_stride, dummy_v_stride;
    if (dec_get_frame(d, &dummy_fd, &dummy_h_stride, &dummy_v_stride))
      d->stat_input_full_drops++;
  }
  msleep(1);

  mpp_packet_init(&pkt, NULL, 0);
  mpp_packet_set_data(pkt, data);
  mpp_packet_set_size(pkt, size);
  mpp_packet_set_pos(pkt, data);
  mpp_packet_set_length(pkt, size);
  r = d->api->decode_put_packet(d->ctx, pkt);
  mpp_packet_deinit(&pkt);
  return (r == MPP_OK) ? 0 : -EIO;
}

/*
 * Drain decoded frames from the decoder. Handles:
 *  - info-change frames (triggers buffer group creation)
 *  - error/discard frames (skipped)
 *  - real frames (extracts dma-buf fd + stride)
 */
static int dec_get_frame(struct mpp_dec* d, int* fd_out, uint32_t* hor_stride_out,
                         uint32_t* ver_stride_out) {
  int got_frame = 0;
  uint64_t t0_ns = dec_now_ns();

  while (1) {
    MppFrame f = NULL;
    MPP_RET r;
    int retry = DEC_POLL_RETRIES;

  try_again:
    r = d->api->decode_get_frame(d->ctx, &f);

    if (r == MPP_ERR_TIMEOUT) {
      /* Yield between polls instead of spinning; matches the TX collector
       * idle discipline (COLLECT_IDLE_SLEEP_US). */
      if (retry-- > 0) { usleep(DEC_POLL_IDLE_US); goto try_again; }
      break;
    }
    if (r != MPP_OK || !f) break; /* no more frames or real error */

    if (mpp_frame_get_info_change(f)) {
      handle_info_change(d, f);
      mpp_frame_deinit(&f);
      continue; /* info-change frame has no picture data */
    }

    RK_U32 err_info = mpp_frame_get_errinfo(f);
    RK_U32 discard  = mpp_frame_get_discard(f);
    if (err_info || discard) {
      mpp_frame_deinit(&f);
      continue; /* corrupted or discarded frame */
    }

    /* real frame — extract dma-buf fd */
    MppBuffer buf = mpp_frame_get_buffer(f);
    if (!buf) { mpp_frame_deinit(&f); continue; }

    int fd = mpp_buffer_get_fd(buf);
    if (fd < 0) { mpp_frame_deinit(&f); continue; }

    /* delayed release: only free the frame from FB_DELAY frames ago.
     * This keeps the MPP dma-buf alive until DRM's async page flip
     * has had time to complete (3 vblanks ~50ms). Immediate release
     * causes MPP to reuse the buffer while DRM still scans it → ghosting. */
    int release_idx = d->held_idx;
    if (d->held[release_idx]) {
      mpp_frame_deinit(&d->held[release_idx]);
      d->held[release_idx] = NULL;
    }

    *fd_out = fd;
    if (hor_stride_out) *hor_stride_out = mpp_frame_get_hor_stride(f);
    if (ver_stride_out) *ver_stride_out = mpp_frame_get_ver_stride(f);
    if (d->frame_count == 0) {
      printf("mpp_dec: first frame stride %ux%u (display %dx%d)\n",
             mpp_frame_get_hor_stride(f), mpp_frame_get_ver_stride(f), d->w, d->h);
    }

    d->held[d->held_idx] = f; /* store for later release */
    d->held_idx = (d->held_idx + 1) % FB_DELAY;
    d->frame_count++;
    {
      /* Successful take cost feeds the 1s-log EMA and the hang guard. */
      uint64_t sample_us = (dec_now_ns() - t0_ns) / 1000;
      if (d->dec_ema_us == 0)
        d->dec_ema_us = sample_us;
      else
        d->dec_ema_us += (sample_us - d->dec_ema_us) / 8;
      d->last_output_ns = dec_now_ns();
    }
    got_frame = 1;
    break; /* one frame per call is enough; caller re-invokes for more */
  }

  return got_frame;
}

/* ---- public API ---- */

#define FEED_CHUNK_SIZE  8192  /* 8KB — balances latency vs chunk count */

int mpp_dec_put(void* dec_priv, uint8_t* data, size_t size) {
  struct mpp_dec* d = (struct mpp_dec*)dec_priv;
  if (!d || !data || !size) return -EINVAL;

  size_t offset = 0;
  while (offset < size) {
    size_t chunk = size - offset;
    if (chunk > FEED_CHUNK_SIZE) chunk = FEED_CHUNK_SIZE;

    int r = dec_put_packet(d, data + offset, chunk);
    if (r < 0) {
      msleep(1);
      r = dec_put_packet(d, data + offset, chunk);
      if (r < 0) return r;
    }
    offset += chunk;
  }
  return 0;
}

int mpp_dec_pull(void* dec_priv, int* fd_out, uint32_t* hor_stride_out,
                 uint32_t* ver_stride_out) {
  struct mpp_dec* d = (struct mpp_dec*)dec_priv;
  if (!d || !fd_out) return -EINVAL;
  return dec_get_frame(d, fd_out, hor_stride_out, ver_stride_out) ? 1 : 0;
}

int mpp_dec_feed(void* dec_priv, uint8_t* data, size_t size, int* fd_out,
                 uint32_t* hor_stride_out, uint32_t* ver_stride_out) {
  int r = mpp_dec_put(dec_priv, data, size);
  if (r < 0) return r;
  return mpp_dec_pull(dec_priv, fd_out, hor_stride_out, ver_stride_out);
}

void mpp_dec_get_stats(void* dec_priv, uint64_t* input_full_drops,
                       uint64_t* ema_us, uint64_t* last_output_ns) {
  struct mpp_dec* d = (struct mpp_dec*)dec_priv;
  if (!d) return;
  if (input_full_drops) *input_full_drops = d->stat_input_full_drops;
  if (ema_us) *ema_us = d->dec_ema_us;
  if (last_output_ns) *last_output_ns = d->last_output_ns;
}

/* Take a pending resolution change (1 = changed, sizes stored to out). */
int mpp_dec_take_resolution(void* dec_priv, int* w_out, int* h_out) {
  struct mpp_dec* d = (struct mpp_dec*)dec_priv;
  if (!d || !d->resolution_pending) return 0;
  d->resolution_pending = false;
  if (w_out) *w_out = d->pending_w;
  if (h_out) *h_out = d->pending_h;
  return 1;
}

/* ---- codec_plugin wrapper ---- */

static int dw_send(void* ctx, int dma_fd, uint32_t stride)
  { (void)ctx; (void)dma_fd; (void)stride; return -ENOSYS; }

static int dw_recv(void* ctx, uint8_t** data, size_t* size, int* is_key)
  { (void)ctx; (void)data; (void)size; (void)is_key; return -ENOSYS; }

struct codec_plugin* mpp_decoder_create(struct codec_config* cfg) {
  struct codec_plugin* p = calloc(1, sizeof(*p));
  struct mpp_dec* d = NULL;
  if (!p || dec_init((void**)&d, cfg)) { free(p); return NULL; }
  p->priv  = d;
  p->name  = "mpp_decoder";
  p->init  = dec_init;
  p->start = dec_start;
  p->stop  = dec_stop;
  p->free  = dec_free;
  p->send  = dw_send;
  p->recv  = dw_recv;
  p->caps  = (struct codec_caps){ .fmts={CODEC_FMT_H264}, .min_w=64, .max_w=4096,
      .min_h=64, .max_h=2160, .supports_dmabuf=true };
  return p;
}
