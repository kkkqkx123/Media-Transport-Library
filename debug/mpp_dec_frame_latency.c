/* MPP decode single-frame latency probe.
 *
 * Splits a H.264 Annex B file into access units, feeds them one by one,
 * and records put-to-first-frame latency per AU. This is the RX-side
 * counterpart of mpp_enc_frame_latency and fills the decode budget gap.
 *
 * Build on board:
 *   gcc debug/mpp_dec_frame_latency.c -o ~/code/mtl/bin/mpp_dec_latency \
 *     -lrockchip_mpp -I/usr/include/rockchip
 */
#include <getopt.h>
#include <pthread.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_vdec_cfg.h>
#include <rockchip/rk_vdec_cmd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* find next Annex B start code at or after pos; returns file size when none */
static size_t next_sc(const uint8_t* d, size_t n, size_t pos) {
  for (size_t i = pos; i + 3 < n; i++) {
    if (d[i] == 0 && d[i + 1] == 0 &&
        (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1)))
      return i;
  }
  return n;
}

/* ---- async ceiling mode: feeder/collector pipeline ----
 * Same measurement idea as the encoder async mode: feed and pull run on
 * separate threads so wall fps is the decoder throughput ceiling, not the
 * synchronous put-then-wait round-trip. Packets carry the AU index as PTS
 * (mpp_packet_set_pts) and output frames pair back by frame PTS; when the
 * frame PTS is unusable the k-th output pairs with the k-th fed AU
 * (exact after priming for baseline streams without B-frames). no_output =
 * fed - framed uses the same allowance as rx_decode_perf.py (SPS/PPS/SEI
 * non-frame AUs plus priming). */
#define DEC_ASYNC_DRAIN_IDLE_MAX 5000 /* ~5s at 1ms after feeder done */
#define DEC_ASYNC_STALL_MAX 60000    /* ~60s global stall guard */

struct dec_async_shared {
  MppCtx ctx;
  MppApi* api;
  const uint8_t* data;
  size_t* au_off;
  size_t* au_len;
  int au_total;
  uint64_t* submit; /* per-AU submit ns, under slock */
  pthread_mutex_t slock;
  volatile int feeder_done;
  volatile int stop;
  volatile int fail;
  int fed;
  int fr;   /* frame (non-header) AUs fed; gate counts these, since header
               * AUs (SPS/PPS/SEI/AUD) never yield frames and would occupy
               * a fed-minus-framed gate permanently. For baseline streams
               * without B-frames each non-header AU yields exactly one
               * frame, so (fr - framed) is the true in-flight depth. */
  int depth; /* max frame-AUs in flight (gate); --depth, default 16 */
  int put_fail;
  int framed;
  int err_frames;
  uint32_t* cost;
  int n_cost;
  uint64_t t_start;
};

/* Default max frame-AUs in flight. Bounds the MPP input queue so a flood
 * of puts never pushes the decoder into its full-queue blocking regime;
 * the feeder then runs at exactly the decoder drain rate and wall fps is
 * the true pipelined ceiling. */
#define DEC_ASYNC_MAX_OUTSTANDING 16

/* NAL unit type of the AU at p (p points at the start code). Header AUs
 * (SEI/SPS/PPS/AUD) never produce frames; the gate must not count them. */
static int au_is_header(const uint8_t* p, size_t len) {
  size_t sc = 0;
  if (len >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 1)
    sc = 3;
  else if (len >= 5 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
    sc = 4;
  else
    return 0;
  if (sc >= len) return 0;
  int nal = p[sc] & 0x1F;
  return (nal == 6 || nal == 7 || nal == 8 || nal == 9);
}

static void* dec_async_feeder(void* arg) {
  struct dec_async_shared* a = (struct dec_async_shared*)arg;
  for (int i = 0; i < a->au_total && !a->stop; i++) {
    int gate_spins = 0;
    for (;;) {
      int outstanding;
      pthread_mutex_lock(&a->slock);
      /* Gate on frame-AUs in flight, not all AUs: header AUs never
       * yield frames, so a fed-minus-framed gate would wedge permanently
       * once depth <= header count. */
      outstanding = a->fr - a->framed;
      pthread_mutex_unlock(&a->slock);
      if (outstanding < a->depth || a->stop) break;
      struct timespec rq = {.tv_sec = 0, .tv_nsec = 200000};
      nanosleep(&rq, NULL);
      if (++gate_spins > 50000) { /* ~10s without drain: HW stall */
        a->fail = 1;
        a->stop = 1;
        break;
      }
    }
    if (a->stop) break;
    MppPacket pkt = NULL;
    mpp_packet_init(&pkt, NULL, 0);
    mpp_packet_set_data(pkt, (void*)(a->data + a->au_off[i]));
    mpp_packet_set_size(pkt, a->au_len[i]);
    mpp_packet_set_pos(pkt, (void*)(a->data + a->au_off[i]));
    mpp_packet_set_length(pkt, a->au_len[i]);
    mpp_packet_set_pts(pkt, (RK_S64)(i + 1));
    int attempts = 0;
    for (;;) {
      MPP_RET r = a->api->decode_put_packet(a->ctx, pkt);
      if (r == MPP_OK) break;
      struct timespec rq = {.tv_sec = 0, .tv_nsec = 2000000};
      nanosleep(&rq, NULL);
      if (++attempts > 200 || a->stop) {
        a->put_fail++;
        break;
      }
    }
    /* Header-ness is known from content before the put; only frame AUs
     * advance the gate counter (see struct dec_async_shared.fr). */
    int is_hdr = au_is_header(a->data + a->au_off[i], a->au_len[i]);
    mpp_packet_deinit(&pkt);
    if (a->stop) break;
    if (attempts <= 200) {
      pthread_mutex_lock(&a->slock);
      a->submit[i] = now_ns();
      a->fed++;
      if (!is_hdr) a->fr++;
      pthread_mutex_unlock(&a->slock);
    }
  }
  if (!a->stop) { /* EOS so the tail flushes; retry: the collector is
     * draining concurrently, but a full input queue drops a single shot. */
    MppPacket eos = NULL;
    mpp_packet_init(&eos, NULL, 0);
    mpp_packet_set_eos(eos);
    int eos_tries = 0;
    while (a->api->decode_put_packet(a->ctx, eos) != MPP_OK &&
           !a->stop && eos_tries++ < 1000) {
      struct timespec rq = {.tv_sec = 0, .tv_nsec = 2000000};
      nanosleep(&rq, NULL);
    }
    mpp_packet_deinit(&eos);
    if (eos_tries >= 1000) a->fail = 1;
  }
  a->feeder_done = 1;
  return NULL;
}

/* Completion: the output count can never equal the fed count (SPS/PPS/SEI
 * non-frame AUs plus priming never yield frames, same allowance as the
 * verdict). After the feeder is done, framed >= fed - allowance with a
 * short quiet window means drained; the 5s cap stays as a hard guard. */
#define DEC_ASYNC_NO_OUTPUT_ALLOWANCE 12
#define DEC_ASYNC_DRAIN_QUIET_MS 100

static void* dec_async_collector(void* arg) {
  struct dec_async_shared* a = (struct dec_async_shared*)arg;
  int idle = 0;
  while (!a->stop) {
    MppFrame frame = NULL;
    if (a->api->decode_get_frame(a->ctx, &frame) != MPP_OK || !frame) {
      struct timespec rq = {.tv_sec = 0, .tv_nsec = 1000000};
      nanosleep(&rq, NULL);
      idle++;
      if (idle > DEC_ASYNC_STALL_MAX)
        break;
      if (a->feeder_done) {
        int fed, framed;
        pthread_mutex_lock(&a->slock);
        fed = a->fed;
        framed = a->framed;
        pthread_mutex_unlock(&a->slock);
        if ((framed >= fed - DEC_ASYNC_NO_OUTPUT_ALLOWANCE &&
             idle > DEC_ASYNC_DRAIN_QUIET_MS) ||
            idle > DEC_ASYNC_DRAIN_IDLE_MAX)
          break;
      }
      continue;
    }
    idle = 0;
    if (mpp_frame_get_info_change(frame)) {
      MppBufferGroup grp = NULL;
      mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
      a->api->control(a->ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp);
      a->api->control(a->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
      mpp_frame_deinit(&frame);
      continue;
    }
    if (mpp_frame_get_discard(frame) || mpp_frame_get_errinfo(frame)) {
      a->err_frames++;
      mpp_frame_deinit(&frame);
      continue;
    }
    uint64_t t1 = now_ns();
    RK_S64 pts = mpp_frame_get_pts(frame);
    int idx = a->framed; /* fallback: k-th output pairs k-th fed AU */
    if (pts >= 1 && pts <= a->au_total) idx = (int)(pts - 1);
    pthread_mutex_lock(&a->slock);
    uint64_t ts = (idx < a->au_total) ? a->submit[idx] : 0;
    pthread_mutex_unlock(&a->slock);
    if (ts > 0 && t1 > ts && a->n_cost < a->au_total)
      a->cost[a->n_cost++] = (uint32_t)((t1 - ts) / 1000ULL);
    pthread_mutex_lock(&a->slock);
    a->framed++;
    pthread_mutex_unlock(&a->slock);
    mpp_frame_deinit(&frame);
  }
  return NULL;
}

/* Splits the whole file into AU offset/length tables once (same start-code
 * rule as the sync path), then runs feeder/collector. Prints the legacy
 * summary line plus wall_fps and a RESULT line for scripts. */
static int run_dec_async(MppCtx ctx, MppApi* api, const uint8_t* data,
                         size_t fsize, int max_au, int warmup, const char* csv,
                         int depth) {
  size_t* offs = malloc(((size_t)max_au + 1) * sizeof(size_t));
  size_t* lens = malloc(((size_t)max_au + 1) * sizeof(size_t));
  uint64_t* submit = calloc((size_t)max_au + 1, sizeof(uint64_t));
  uint32_t* cost = calloc((size_t)max_au + 1, sizeof(uint32_t));
  if (!offs || !lens || !submit || !cost) {
    fprintf(stderr, "async: oom\n");
    free(offs);
    free(lens);
    free(submit);
    free(cost);
    return 1;
  }
  int total = 0;
  size_t pos = next_sc(data, fsize, 0);
  while (pos < fsize && total < max_au) {
    size_t end = next_sc(data, fsize, pos + 3);
    if (end - pos == 0) {
      pos = end;
      continue;
    }
    offs[total] = pos;
    lens[total] = end - pos;
    total++;
    pos = end;
  }
  if (total == 0) {
    fprintf(stderr, "async: no AUs found\n");
    free(offs);
    free(lens);
    free(submit);
    free(cost);
    return 1;
  }
  struct dec_async_shared a;
  memset(&a, 0, sizeof(a));
  a.ctx = ctx;
  a.api = api;
  a.data = data;
  a.au_off = offs;
  a.au_len = lens;
  a.au_total = total;
  a.submit = submit;
  a.cost = cost;
  a.depth = depth > 0 ? depth : DEC_ASYNC_MAX_OUTSTANDING;
  pthread_mutex_init(&a.slock, NULL);
  a.t_start = now_ns();
  pthread_t th_feed, th_collect;
  pthread_create(&th_feed, NULL, dec_async_feeder, &a);
  pthread_create(&th_collect, NULL, dec_async_collector, &a);
  pthread_join(th_feed, NULL);
  pthread_join(th_collect, NULL);
  uint64_t t_end = now_ns();
  pthread_mutex_destroy(&a.slock);
  int no_output = a.fed - a.framed;
  if (no_output < 0) no_output = 0;
  long skip = warmup < a.n_cost ? warmup : 0;
  long n = (long)a.n_cost - skip;
  int rc = 1;
  double wall_ms = (double)(t_end - a.t_start) / 1000000.0;
  double wall_fps = wall_ms > 0 ? (double)a.framed * 1000.0 / wall_ms : 0.0;
  if (n > 0) {
    uint32_t* s = malloc((size_t)n * sizeof(uint32_t));
    memcpy(s, cost + skip, (size_t)n * sizeof(uint32_t));
    uint64_t sum = 0;
    for (long i = 0; i < n; i++) sum += cost[skip + i];
    qsort(s, (size_t)n, sizeof(uint32_t), cmp_u32);
    /* Sojourn percentiles include pipeline queueing; fps is the ceiling. */
    printf("au_fed=%d framed=%d no_output=%d put_fail=%d "
           "mean_us=%.1f p50=%u p90=%u p99=%u wall_fps=%.1f mode=async "
           "depth=%d\n",
           a.fed, a.framed, no_output, a.put_fail, (double)sum / n,
           pct_u32(s, (int)n, 50), pct_u32(s, (int)n, 90),
           pct_u32(s, (int)n, 99), wall_fps, a.depth);
    free(s);
    if (csv) {
      FILE* cf = fopen(csv, "w");
      if (cf) {
        fprintf(cf, "frame,cost_us\n");
        for (long i = 0; i < n; i++) fprintf(cf, "%ld,%u\n", i, cost[skip + i]);
        fclose(cf);
        printf("written: %s\n", csv);
      }
    }
    rc = 0;
  } else {
    fprintf(stderr, "async: FAIL fed=%d framed=%d put_fail=%d\n", a.fed,
            a.framed, a.put_fail);
  }
  printf("RESULT async dec au_fed=%d framed=%d no_output=%d put_fail=%d "
         "wall_ms=%.0f wall_fps=%.1f err_frames=%d depth=%d\n",
         a.fed, a.framed, no_output, a.put_fail, wall_ms, wall_fps,
         a.err_frames, a.depth);
  free(offs);
  free(lens);
  free(submit);
  free(cost);
  return (rc == 0 && !a.fail) ? 0 : 1;
}

int main(int argc, char** argv) {
  const char* input = NULL;
  int max_au = 600;
  const char* csv = NULL;
  /* per-AU pull deadline. Must exceed the slowest real frame or the probe
   * misclassifies slow frames as no_output (was a hard ~25ms cap before). */
  int wait_ms = 50;

  const char* mode = "sync"; /* sync: legacy put-wait; async: feeder/collector */
  int warmup = 10; /* async: skipped priming output samples */
  /* Max frame-AUs in flight (async gate). Default preserves the legacy
   * depth; small values (2~4) bound queueing delay for latency budgets. */
  int depth = DEC_ASYNC_MAX_OUTSTANDING;
  static struct option opts[] = {
    {"input", required_argument, 0, 'i'},
    {"max-au", required_argument, 0, 'n'},
    {"csv", required_argument, 0, 'c'},
    {"wait-ms", required_argument, 0, 'm'},
    {"mode", required_argument, 0, 'M'},
    {"warmup", required_argument, 0, 'W'},
    {"depth", required_argument, 0, 'd'},
    {0, 0, 0, 0},
  };
  int o;
  while ((o = getopt_long(argc, argv, "i:n:c:m:M:W:d:", opts, NULL)) != -1) {
    switch (o) {
      case 'i': input = optarg; break;
      case 'n': max_au = atoi(optarg); break;
      case 'c': csv = optarg; break;
      case 'm': wait_ms = atoi(optarg); break;
      case 'M': mode = optarg; break;
      case 'W': warmup = atoi(optarg); break;
      case 'd': depth = atoi(optarg); break;
      default:
        fprintf(stderr, "usage: %s --input H264 [--max-au N] [--csv FILE] [--wait-ms MS] [--mode sync|async] [--warmup N] [--depth N]\n", argv[0]);
        return 1;
    }
  }
  if (!input) { fprintf(stderr, "missing --input H264 file\n"); return 1; }
  int async_mode = (strcmp(mode, "async") == 0);
  if (strcmp(mode, "sync") != 0 && !async_mode) {
    fprintf(stderr, "unknown --mode %s (sync|async)\n", mode);
    return 1;
  }
  if (warmup < 0) warmup = 0;

  FILE* f = fopen(input, "rb");
  if (!f) { perror("fopen input"); return 1; }
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (fsize <= 0) { fprintf(stderr, "empty input\n"); return 1; }
  uint8_t* data = malloc((size_t)fsize);
  if (!data) { fprintf(stderr, "oom\n"); return 1; }
  if (fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
    fprintf(stderr, "short read\n");
    return 1;
  }
  fclose(f);

  MppCtx ctx = NULL;
  MppApi* api = NULL;
  if (mpp_create(&ctx, &api)) { fprintf(stderr, "mpp_create failed\n"); return 1; }
  if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC)) {
    fprintf(stderr, "mpp_init dec failed\n");
    return 1;
  }
  MppDecCfg cfg = NULL;
  mpp_dec_cfg_init(&cfg);
  mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
  mpp_dec_cfg_set_u32(cfg, "base:fast_out", 1);
  if (api->control(ctx, MPP_DEC_SET_CFG, cfg)) {
    fprintf(stderr, "SET_CFG failed\n");
    return 1;
  }

  uint32_t* cost = calloc((size_t)(max_au > 0 ? max_au : 1), sizeof(uint32_t));
  if (!cost) return 1;

  int exit_code = 0;
  if (async_mode) {
    exit_code = run_dec_async(ctx, api, data, (size_t)fsize, max_au, warmup,
                              csv, depth);
  } else {
  size_t pos = next_sc(data, (size_t)fsize, 0);
  int fed = 0, framed = 0;
  int no_output = 0, put_fail = 0;
  while (pos < (size_t)fsize && fed < max_au) {
    size_t end = next_sc(data, (size_t)fsize, pos + 3);
    size_t au_len = end - pos;
    if (au_len == 0) { pos = end; continue; }

    MppPacket pkt = NULL;
    mpp_packet_init(&pkt, data + pos, au_len);
    mpp_packet_set_pos(pkt, data + pos);
    mpp_packet_set_length(pkt, au_len);

    uint64_t t0 = now_ns();
    MPP_RET r = api->decode_put_packet(ctx, pkt);
    mpp_packet_deinit(&pkt);
    if (r != MPP_OK) {
      put_fail++;
      /* brief backpressure wait, then drop this AU */
      struct timespec rq = {.tv_sec = 0, .tv_nsec = 2000000};
      nanosleep(&rq, NULL);
      pos = end;
      continue;
    }
    fed++;

    /* pull until one displayable frame appears for this AU (deadline-bounded;
     * 100us poll keeps quantization well below the measured p50) */
    uint64_t deadline = t0 + (uint64_t)wait_ms * 1000000ULL;
    int ok = 0;
    while (now_ns() < deadline) {
      MppFrame frame = NULL;
      if (api->decode_get_frame(ctx, &frame) != MPP_OK || !frame) {
        struct timespec rq = {.tv_sec = 0, .tv_nsec = 100000};
        nanosleep(&rq, NULL);
        continue;
      }
      if (mpp_frame_get_info_change(frame)) {
        /* adopt new geometry, keep draining */
        MppBufferGroup grp = NULL;
        mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
        api->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp);
        api->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
      } else if (mpp_frame_get_discard(frame) || mpp_frame_get_errinfo(frame)) {
        /* error frame carries no display content */
      } else {
        ok = 1;
      }
      /* return the buffer slot */
      mpp_frame_deinit(&frame);
      if (ok) break;
    }
    uint64_t t1 = now_ns();
    if (ok) {
      cost[framed++] = (uint32_t)((t1 - t0) / 1000ULL);
    } else {
      no_output++;
    }
    pos = end;
  }

  /* EOS flush for the tail */
  {
    MppPacket eos = NULL;
    mpp_packet_init(&eos, NULL, 0);
    mpp_packet_set_eos(eos);
    api->decode_put_packet(ctx, eos);
    mpp_packet_deinit(&eos);
    for (int i = 0; i < 40; i++) {
      MppFrame frame = NULL;
      if (api->decode_get_frame(ctx, &frame) != MPP_OK || !frame) break;
      mpp_frame_deinit(&frame);
    }
  }

  if (framed > 0) {
    uint32_t* s = malloc((size_t)framed * sizeof(uint32_t));
    memcpy(s, cost, (size_t)framed * sizeof(uint32_t));
    qsort(s, (size_t)framed, sizeof(uint32_t), cmp_u32);
    uint64_t sum = 0;
    for (int i = 0; i < framed; i++) sum += cost[i];
    printf("au_fed=%d framed=%d no_output=%d put_fail=%d "
           "mean_us=%.1f p50=%u p90=%u p99=%u\n",
           fed, framed, no_output, put_fail, (double)sum / framed,
           pct_u32(s, framed, 50), pct_u32(s, framed, 90),
           pct_u32(s, framed, 99));
    free(s);
    if (csv) {
      FILE* cf = fopen(csv, "w");
      if (cf) {
        fprintf(cf, "frame,cost_us\n");
        for (int i = 0; i < framed; i++) fprintf(cf, "%d,%u\n", i, cost[i]);
        fclose(cf);
        printf("written: %s\n", csv);
      }
    }
  } else {
    fprintf(stderr, "FAIL: no frames decoded (fed=%d put_fail=%d)\n", fed, put_fail);
  }

  exit_code = framed > 0 ? 0 : 1;
  } /* sync */
  mpp_dec_cfg_deinit(cfg);
  mpp_destroy(ctx);
  free(data);
  free(cost);
  return exit_code;
}
