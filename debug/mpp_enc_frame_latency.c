/* MPP encode single-frame latency probe.
 *
 * Feeds NV12 frames from a file into the hardware encoder and records the
 * submit-to-output cost per frame. Mirrors the production encoder settings
 * (H.264, CBR) so numbers transfer to the TX pipeline budget.
 *
 * Build on board:
 *   gcc debug/mpp_enc_frame_latency.c -o /tmp/mpp_enc_latency \
 *     -lrockchip_mpp -I/usr/include/rockchip
 */
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdbool.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u32(const void* a, const void* b) {
  uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
  return (x > y) - (x < y);
}

static uint32_t pct_u32(uint32_t* v, int n, int p) {
  if (n <= 0) return 0;
  int i = (p * n + 50) / 100 - 1;
  if (i < 0) i = 0;
  if (i >= n) i = n - 1;
  return v[i];
}

/* ---- async ceiling mode: feeder/collector pipeline ----
 * Mirrors the production encoder threading (encode_put_frame on one thread,
 * encode_get_packet on another) and the ZL mpp_ceiling_async approach:
 * input and output fully pipelined, so wall fps is the hardware throughput
 * ceiling rather than the synchronous round-trip latency. Per-frame cost is
 * submit-to-output sojourn (includes queueing) and must NOT be compared
 * with sync-mode service latency or judged against frame-interval budgets.
 * Output order matches input order (single session, CBR baseline, no
 * B-frames), so submit stamps pair by FIFO. */
#define ENC_ASYNC_NBUF 8
#define ENC_ASYNC_PRELOAD_MAX 32

struct enc_async_shared {
  MppCtx ctx;
  MppApi* api;
  MppBuffer bufs[ENC_ASYNC_NBUF];
  const uint8_t* preload;
  int n_pre;
  size_t img_bytes;
  size_t file_row, mpp_row;
  int h, w, hor_stride, is_nv12;
  MppFrameFormat fmt;
  long total; /* warmup + measured frames */
  long sent, got;
  volatile int feeder_done;
  volatile int stop;
  volatile int fail;
  long put_retries;
  pthread_mutex_t qlock;
  int64_t* stamps;
  int qcap, qhead, qtail, qcount;
  uint32_t* cost;
  long n_cost;
  uint64_t bytes_out;
  FILE* outf;
  uint64_t t_start;
};

/* Same row packing as the sync path: file rows (tight) -> MPP rows. */
static void enc_async_pack(struct enc_async_shared* a, uint8_t* dst,
                           const uint8_t* src) {
  size_t fw = a->file_row, mw = a->mpp_row;
  size_t copy = fw < mw ? fw : mw;
  if (a->is_nv12) {
    for (int r = 0; r < a->h; r++) {
      memcpy(dst + (size_t)r * mw, src + (size_t)r * fw, copy);
      if (mw > copy) memset(dst + (size_t)r * mw + copy, 0, mw - copy);
    }
    uint8_t* uv_dst = dst + mw * (size_t)a->h;
    const uint8_t* uv_src = src + fw * (size_t)a->h;
    for (int r = 0; r < a->h / 2; r++) {
      memcpy(uv_dst + (size_t)r * mw, uv_src + (size_t)r * fw, copy);
      if (mw > copy) memset(uv_dst + (size_t)r * mw + copy, 0, mw - copy);
    }
  } else {
    for (int r = 0; r < a->h; r++) {
      memcpy(dst + (size_t)r * mw, src + (size_t)r * fw, copy);
      if (mw > copy) memset(dst + (size_t)r * mw + copy, 0, mw - copy);
    }
  }
}

static void enc_async_qpush(struct enc_async_shared* a, int64_t ts) {
  pthread_mutex_lock(&a->qlock);
  if (a->qcount < a->qcap) {
    a->stamps[a->qtail] = ts;
    a->qtail = (a->qtail + 1) % a->qcap;
    a->qcount++;
  }
  pthread_mutex_unlock(&a->qlock);
}

static bool enc_async_qpop(struct enc_async_shared* a, int64_t* ts) {
  bool ok = false;
  pthread_mutex_lock(&a->qlock);
  if (a->qcount > 0) {
    *ts = a->stamps[a->qhead];
    a->qhead = (a->qhead + 1) % a->qcap;
    a->qcount--;
    ok = true;
  }
  pthread_mutex_unlock(&a->qlock);
  return ok;
}

static void* enc_async_feeder(void* arg) {
  struct enc_async_shared* a = (struct enc_async_shared*)arg;
  for (long seq = 0; seq < a->total && !a->stop; seq++) {
    int spins = 0;
    for (;;) {
      bool full;
      pthread_mutex_lock(&a->qlock);
      full = (a->sent - a->got) >= ENC_ASYNC_NBUF;
      pthread_mutex_unlock(&a->qlock);
      if (!full || a->stop) break;
      struct timespec rq = {.tv_sec = 0, .tv_nsec = 200000};
      nanosleep(&rq, NULL);
      if (++spins > 25000) { /* ~5s without a free slot: HW stall */
        a->fail = 1;
        a->stop = 1;
        break;
      }
    }
    if (a->stop) break;
    MppBuffer dst = a->bufs[seq % ENC_ASYNC_NBUF];
    enc_async_pack(a, mpp_buffer_get_ptr(dst),
                   a->preload + (size_t)(seq % a->n_pre) * a->img_bytes);
    MppFrame frame = NULL;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, a->w);
    mpp_frame_set_height(frame, a->h);
    mpp_frame_set_hor_stride(frame, a->hor_stride);
    mpp_frame_set_ver_stride(frame, a->h);
    mpp_frame_set_fmt(frame, a->fmt);
    mpp_frame_set_buffer(frame, dst);
    int attempts = 0;
    for (;;) {
      MPP_RET r = a->api->encode_put_frame(a->ctx, frame);
      if (r == MPP_OK) break;
      a->put_retries++;
      struct timespec rq = {.tv_sec = 0, .tv_nsec = 2000000};
      nanosleep(&rq, NULL);
      if (++attempts > 1000 || a->stop) {
        a->fail = 1;
        a->stop = 1;
        break;
      }
    }
    mpp_frame_deinit(&frame);
    if (a->stop) break;
    enc_async_qpush(a, (int64_t)now_ns());
    pthread_mutex_lock(&a->qlock);
    a->sent++;
    pthread_mutex_unlock(&a->qlock);
  }
  a->feeder_done = 1;
  return NULL;
}

static void* enc_async_collector(void* arg) {
  struct enc_async_shared* a = (struct enc_async_shared*)arg;
  int idle = 0;
  while (!a->stop) {
    bool done;
    pthread_mutex_lock(&a->qlock);
    done = (a->got >= a->total);
    pthread_mutex_unlock(&a->qlock);
    if (done) break;
    MppPacket pkt = NULL;
    if (a->api->encode_get_packet(a->ctx, &pkt) == MPP_OK && pkt) {
      uint64_t t1 = now_ns();
      int64_t ts = 0;
      if (enc_async_qpop(a, &ts) && t1 > (uint64_t)ts &&
          a->n_cost < a->total) {
        a->cost[a->n_cost++] = (uint32_t)((t1 - (uint64_t)ts) / 1000ULL);
      }
      size_t len = mpp_packet_get_length(pkt);
      a->bytes_out += (uint64_t)len;
      if (a->outf) {
        void* p = mpp_packet_get_pos(pkt);
        if (p && len) fwrite(p, 1, len, a->outf);
      }
      mpp_packet_deinit(&pkt);
      pthread_mutex_lock(&a->qlock);
      a->got++;
      pthread_mutex_unlock(&a->qlock);
      idle = 0;
      continue;
    }
    struct timespec rq = {.tv_sec = 0, .tv_nsec = 100000};
    nanosleep(&rq, NULL);
    if (++idle > 600000 || (a->feeder_done && idle > 50000)) {
      a->fail = 1; /* 60s stall, or 5s silence after feeder done */
      a->stop = 1;
    }
  }
  return NULL;
}

/* Runs the async pipeline reusing the already-configured ctx/api. Returns
 * process exit code. Prints both the legacy summary line (plus wall_fps)
 * and a RESULT line for scripts. */
static int run_enc_async(MppCtx ctx, MppApi* api, FILE* f, FILE* outf, int w,
                         int h, int frames, int fps, int bps, int level,
                         int hor_stride, size_t file_row, size_t mpp_row,
                         size_t mpp_buf_size, size_t file_bytes, int is_nv12,
                         MppFrameFormat mpp_fmt, const char* csv, int warmup) {
  (void)bps;
  struct enc_async_shared a;
  memset(&a, 0, sizeof(a));
  a.ctx = ctx;
  a.api = api;
  a.outf = outf;
  a.w = w;
  a.h = h;
  a.hor_stride = hor_stride;
  a.fmt = mpp_fmt;
  a.is_nv12 = is_nv12;
  a.file_row = file_row;
  a.mpp_row = mpp_row;
  a.img_bytes = file_bytes;
  a.total = (long)warmup + frames;
  a.n_pre = a.total < ENC_ASYNC_PRELOAD_MAX ? (int)a.total
                                             : ENC_ASYNC_PRELOAD_MAX;
  a.preload = NULL;
  uint8_t* pre = malloc((size_t)a.n_pre * file_bytes);
  if (!pre) {
    fprintf(stderr, "async: preload oom\n");
    return 1;
  }
  for (int i = 0; i < a.n_pre; i++) {
    if (fread(pre + (size_t)i * file_bytes, 1, file_bytes, f) != file_bytes) {
      fseek(f, 0, SEEK_SET);
      if (fread(pre + (size_t)i * file_bytes, 1, file_bytes, f) != file_bytes) {
        fprintf(stderr, "async: preload read failed at %d\n", i);
        free(pre);
        return 1;
      }
    }
  }
  a.preload = pre;
  for (int i = 0; i < ENC_ASYNC_NBUF; i++) {
    if (mpp_buffer_get(NULL, &a.bufs[i], mpp_buf_size)) {
      fprintf(stderr, "async: mpp_buffer_get %d failed\n", i);
      free(pre);
      return 1;
    }
  }
  a.qcap = (int)a.total + 16;
  a.stamps = calloc((size_t)a.qcap, sizeof(int64_t));
  a.cost = calloc((size_t)a.total > 0 ? (size_t)a.total : 1, sizeof(uint32_t));
  if (!a.stamps || !a.cost) {
    fprintf(stderr, "async: oom\n");
    free(pre);
    free(a.stamps);
    free(a.cost);
    return 1;
  }
  pthread_mutex_init(&a.qlock, NULL);
  a.t_start = now_ns();
  pthread_t th_feed, th_collect;
  pthread_create(&th_feed, NULL, enc_async_feeder, &a);
  pthread_create(&th_collect, NULL, enc_async_collector, &a);
  pthread_join(th_feed, NULL);
  pthread_join(th_collect, NULL);
  uint64_t t_end = now_ns();
  pthread_mutex_destroy(&a.qlock);

  long skip = warmup < a.n_cost ? warmup : 0;
  long n = a.n_cost - skip;
  int rc = 1;
  if (n > 0 && !a.fail) {
    uint32_t* s = malloc((size_t)n * sizeof(uint32_t));
    memcpy(s, a.cost + skip, (size_t)n * sizeof(uint32_t));
    uint64_t sum = 0;
    for (long i = 0; i < n; i++) sum += a.cost[skip + i];
    qsort(s, (size_t)n, sizeof(uint32_t), cmp_u32);
    double wall_ms = (double)(t_end - a.t_start) / 1000000.0;
    double wall_fps = wall_ms > 0 ? (double)a.got * 1000.0 / wall_ms : 0.0;
    double out_kbps = wall_ms > 0 ? (double)a.bytes_out * 8.0 / wall_ms : 0.0;
    /* Legacy-shaped line plus wall_fps; sojourn mean/p50/p90/p99 include
     * pipeline queueing and are NOT comparable to sync-mode numbers. */
    printf("frames=%ld %dx%d@%dfps level=%d mean_us=%.1f p50=%u p90=%u p99=%u "
           "out_kbps=%.0f wall_fps=%.1f mode=async\n",
           n, w, h, fps, level, (double)sum / n, pct_u32(s, (int)n, 50),
           pct_u32(s, (int)n, 90), pct_u32(s, (int)n, 99), out_kbps, wall_fps);
    printf("RESULT async enc %dx%d frames=%ld wall_ms=%.0f wall_fps=%.1f "
           "mean_us=%.1f p50=%u p90=%u p99=%u out_kbps=%.0f put_retries=%ld\n",
           w, h, n, wall_ms, wall_fps, (double)sum / n,
           pct_u32(s, (int)n, 50), pct_u32(s, (int)n, 90),
           pct_u32(s, (int)n, 99), out_kbps, a.put_retries);
    free(s);
    if (csv) {
      FILE* cf = fopen(csv, "w");
      if (cf) {
        fprintf(cf, "frame,cost_us\n");
        for (long i = 0; i < n; i++)
          fprintf(cf, "%ld,%u\n", i, a.cost[skip + i]);
        fclose(cf);
        printf("written: %s\n", csv);
      }
    }
    rc = 0;
  } else {
    fprintf(stderr, "async: FAIL sent=%ld got=%ld fail=%d\n", a.sent, a.got,
            a.fail);
  }
  for (int i = 0; i < ENC_ASYNC_NBUF; i++) mpp_buffer_put(a.bufs[i]);
  free(pre);
  free(a.stamps);
  free(a.cost);
  return rc;
}

int main(int argc, char** argv) {
  const char* input = NULL;
  int w = 1920, h = 1080, frames = 120, bps = 8000000, fps = 60;
  const char* csv = NULL;
  const char* infmt = "nv12";
  const char* outpath = NULL;
  int level = 0; /* 0 = auto by resolution (42 for <=1080p, 51/52 for 4K) */
  int hor_stride_arg = 0;  /* MPP hor_stride in pixels, 0 = w */
  int file_stride = 0;     /* row bytes in input file, 0 = tight */
  const char* mode = "sync"; /* sync: legacy submit-wait; async: feeder/collector */
  int warmup = 10; /* async: extra fed frames + skipped priming samples */
  FILE* outf = NULL;

  static struct option opts[] = {
    {"input", required_argument, 0, 'i'},
    {"w", required_argument, 0, 'w'},
    {"h", required_argument, 0, 'H'},
    {"frames", required_argument, 0, 'n'},
    {"bps", required_argument, 0, 'b'},
    {"fps", required_argument, 0, 'f'},
    {"csv", required_argument, 0, 'c'},
    {"infmt", required_argument, 0, 'F'},
    {"out", required_argument, 0, 'o'},
    {"level", required_argument, 0, 'L'},
    {"hor-stride", required_argument, 0, 'S'},
    {"file-stride", required_argument, 0, 'T'},
    {"mode", required_argument, 0, 'm'},
    {"warmup", required_argument, 0, 'W'},
    {0, 0, 0, 0},
  };
  int o;
  while ((o = getopt_long(argc, argv, "i:w:H:n:b:f:c:F:o:L:S:T:m:W:", opts, NULL)) != -1) {
    switch (o) {
      case 'i': input = optarg; break;
      case 'w': w = atoi(optarg); break;
      case 'H': h = atoi(optarg); break;
      case 'n': frames = atoi(optarg); break;
      case 'b': bps = atoi(optarg); break;
      case 'f': fps = atoi(optarg); break;
      case 'c': csv = optarg; break;
      case 'F': infmt = optarg; break;
      case 'o': outpath = optarg; break;
      case 'L': level = atoi(optarg); break;
      case 'S': hor_stride_arg = atoi(optarg); break;
      case 'T': file_stride = atoi(optarg); break;
      case 'm': mode = optarg; break;
      case 'W': warmup = atoi(optarg); break;
      default:
        fprintf(stderr, "usage: %s --input FILE --w W --h H [--frames N] [--bps B] [--fps F] [--csv FILE] [--infmt nv12|bgr3|rgb3] [--level N] [--hor-stride PX] [--file-stride BYTES] [--mode sync|async] [--warmup N]\n", argv[0]);
        return 1;
    }
  }
  if (!input) { fprintf(stderr, "missing --input file\n"); return 1; }
  if (strcmp(mode, "sync") != 0 && strcmp(mode, "async") != 0) {
    fprintf(stderr, "unknown --mode %s (sync|async)\n", mode);
    return 1;
  }

  MppFrameFormat mpp_fmt = MPP_FMT_YUV420SP;
  int px_bytes = 1; /* bytes per pixel of one input row unit */
  size_t fbytes = (size_t)w * (size_t)h * 3 / 2;
  if (strcmp(infmt, "bgr3") == 0) {
    mpp_fmt = MPP_FMT_BGR888;
    px_bytes = 3;
    fbytes = (size_t)w * (size_t)h * 3;
  } else if (strcmp(infmt, "rgb3") == 0) {
    mpp_fmt = MPP_FMT_RGB888;
    px_bytes = 3;
    fbytes = (size_t)w * (size_t)h * 3;
  } else if (strcmp(infmt, "nv12") != 0) {
    fprintf(stderr, "unknown --infmt %s\n", infmt);
    return 1;
  }
  /* Stride matrix knobs (BGR stride-unit investigation): file rows are
   * file_stride bytes (default tight); MPP buffer rows are hs_px pixels. */
  int hs_px = hor_stride_arg > 0 ? hor_stride_arg : w;
  size_t file_row = file_stride > 0 ? (size_t)file_stride : (size_t)w * px_bytes;
  size_t mpp_row = (size_t)hs_px * (mpp_fmt == MPP_FMT_YUV420SP ? 1 : 3);
  size_t mpp_buf_size = mpp_row * (size_t)h * (mpp_fmt == MPP_FMT_YUV420SP ? 3 : 2) / 2;
  size_t file_bytes = file_row * (size_t)h;
  if (mpp_fmt == MPP_FMT_YUV420SP) file_bytes = file_bytes * 3 / 2;
  (void)fbytes;
  uint8_t* yuv = malloc(file_bytes);
  uint32_t* cost = calloc((size_t)(frames > 0 ? frames : 1), sizeof(uint32_t));
  if (!yuv || !cost) { fprintf(stderr, "oom\n"); return 1; }

  FILE* f = fopen(input, "rb");
  if (!f) { perror("fopen input"); return 1; }
  if (outpath) {
    outf = fopen(outpath, "wb");
    if (!outf) { perror("fopen out"); return 1; }
  }

  MppCtx ctx = NULL;
  MppApi* api = NULL;
  if (mpp_create(&ctx, &api)) { fprintf(stderr, "mpp_create failed\n"); return 1; }
  if (mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC)) {
    fprintf(stderr, "mpp_init enc failed\n");
    return 1;
  }
  MppEncCfg cfg = NULL;
  mpp_enc_cfg_init(&cfg);
  mpp_enc_cfg_set_s32(cfg, "prep:width", w);
  mpp_enc_cfg_set_s32(cfg, "prep:height", h);
  mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", w);
  mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", h);
  mpp_enc_cfg_set_s32(cfg, "prep:format", mpp_fmt);
  mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
  mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
  mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
  mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
  mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
  mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
  mpp_enc_cfg_set_s32(cfg, "rc:bps_min", (int)(bps * 0.9f));
  mpp_enc_cfg_set_s32(cfg, "rc:bps_max", (int)(bps * 1.1f));
  mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 26);
  mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 10);
  mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 38);
  mpp_enc_cfg_set_s32(cfg, "rc:gop", 60);
  mpp_enc_cfg_set_s32(cfg, "h264:profile", 66);
  /* level 42 caps at 1080p60 (MaxFS 8192 MBs); 4K needs 5.1/5.2.
   * Auto: <=1080p -> 42 (matches production), 4K -> 51 (<=30fps) / 52 (>30fps). */
  if (level == 0) {
    long pixels = (long)w * (long)h;
    if (pixels > 2073600L)
      level = (fps > 30) ? 52 : 51;
    else
      level = 42;
  }
  mpp_enc_cfg_set_s32(cfg, "h264:level", level);
  mpp_enc_cfg_set_s32(cfg, "h264:stream_type", 0);
  if (api->control(ctx, MPP_ENC_SET_CFG, cfg)) {
    fprintf(stderr, "SET_CFG failed\n");
    return 1;
  }

  MppBuffer frm_buf = NULL;
  if (mpp_buffer_get(NULL, &frm_buf, mpp_buf_size)) {
    fprintf(stderr, "mpp_buffer_get failed\n");
    return 1;
  }
  printf("stride: file_row=%zu mpp_row=%zu (hs_px=%d)\n", file_row, mpp_row,
         hs_px);

  int async_mode = (strcmp(mode, "async") == 0);
  if (warmup < 0) warmup = 0;
  int exit_code = 0;
  if (async_mode) {
    int is_nv12 = (mpp_fmt == MPP_FMT_YUV420SP);
    exit_code = run_enc_async(ctx, api, f, outf, w, h, frames, fps, bps, level,
                              hs_px, file_row, mpp_row, mpp_buf_size, file_bytes,
                              is_nv12, mpp_fmt, csv, warmup);
  } else {
  int done = 0;
  uint64_t total_out = 0;
  for (int i = 0; i < frames; i++) {
    if (fread(yuv, 1, file_bytes, f) != file_bytes) {
      fseek(f, 0, SEEK_SET);
      if (fread(yuv, 1, file_bytes, f) != file_bytes) break;
    }
    /* file rows -> MPP rows (stride matrix); tight default == flat copy */
    {
      uint8_t* dst = mpp_buffer_get_ptr(frm_buf);
      if (mpp_fmt == MPP_FMT_YUV420SP) {
        /* Y: h rows; UV: h/2 rows, same row width */
        size_t fw = file_row, mw = mpp_row;
        size_t copy = fw < mw ? fw : mw;
        for (int r = 0; r < h; r++) {
          memcpy(dst + (size_t)r * mw, yuv + (size_t)r * fw, copy);
          if (mw > copy) memset(dst + (size_t)r * mw + copy, 0, mw - copy);
        }
        uint8_t* uv_dst = dst + mw * (size_t)h;
        const uint8_t* uv_src = yuv + fw * (size_t)h;
        for (int r = 0; r < h / 2; r++) {
          memcpy(uv_dst + (size_t)r * mw, uv_src + (size_t)r * fw, copy);
          if (mw > copy)
            memset(uv_dst + (size_t)r * mw + copy, 0, mw - copy);
        }
      } else {
        size_t copy = file_row < mpp_row ? file_row : mpp_row;
        for (int r = 0; r < h; r++) {
          memcpy(dst + (size_t)r * mpp_row, yuv + (size_t)r * file_row,
                 copy);
          if (mpp_row > copy)
            memset(dst + (size_t)r * mpp_row + copy, 0, mpp_row - copy);
        }
      }
    }

    MppFrame frame = NULL;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, w);
    mpp_frame_set_height(frame, h);
    mpp_frame_set_hor_stride(frame, hs_px);
    mpp_frame_set_ver_stride(frame, h);
    mpp_frame_set_fmt(frame, mpp_fmt);
    mpp_frame_set_buffer(frame, frm_buf);
    mpp_frame_set_eos(frame, 0);

    uint64_t t0 = now_ns();
    if (api->encode_put_frame(ctx, frame)) {
      fprintf(stderr, "encode_put_frame failed at %d\n", i);
      break;
    }
    MppPacket pkt = NULL;
    /* poll briefly; hardware typically returns within a frame budget */
    int waited = 0;
    while (waited < 200) {
      if (api->encode_get_packet(ctx, &pkt) == MPP_OK && pkt) break;
      struct timespec rq = {.tv_sec = 0, .tv_nsec = 1000000};
      nanosleep(&rq, NULL);
      waited++;
    }
    if (!pkt) {
      fprintf(stderr, "encode_get_packet timeout at %d\n", i);
      break;
    }
    uint64_t t1 = now_ns();
    /* frame 0 primes the pipe (SPS/PPS path, HW wakeup) — encode it but
     * exclude it from the steady-state sample set */
    if (i > 0) {
      cost[done++] = (uint32_t)((t1 - t0) / 1000ULL);
    }
    total_out += mpp_packet_get_length(pkt);
    if (outf) {
      void* p = mpp_packet_get_pos(pkt);
      size_t len = mpp_packet_get_length(pkt);
      if (p && len) fwrite(p, 1, len, outf);
    }
    mpp_packet_deinit(&pkt);
  }

  if (done > 0) {
    uint32_t* s = malloc((size_t)done * sizeof(uint32_t));
    memcpy(s, cost, (size_t)done * sizeof(uint32_t));
    qsort(s, (size_t)done, sizeof(uint32_t), cmp_u32);
    uint64_t sum = 0;
    for (int i = 0; i < done; i++) sum += cost[i];
    printf("frames=%d %dx%d@%dfps level=%d mean_us=%.1f p50=%u p90=%u p99=%u out_kbps=%.0f\n",
           done, w, h, fps, level, (double)sum / done,
           pct_u32(s, done, 50), pct_u32(s, done, 90), pct_u32(s, done, 99),
           (double)total_out * 8 / 1000);
    free(s);
    if (csv) {
      FILE* cf = fopen(csv, "w");
      if (cf) {
        fprintf(cf, "frame,cost_us\n");
        for (int i = 0; i < done; i++) fprintf(cf, "%d,%u\n", i, cost[i]);
        fclose(cf);
        printf("written: %s\n", csv);
      }
    }
  } else {
    fprintf(stderr, "FAIL: no frames encoded\n");
  }

  exit_code = done > 0 ? 0 : 1;
  } /* sync */
  mpp_buffer_put(frm_buf);
  mpp_enc_cfg_deinit(cfg);
  mpp_destroy(ctx);
  fclose(f);
  if (outf) fclose(outf);
  free(yuv);
  free(cost);
  return exit_code;
}
