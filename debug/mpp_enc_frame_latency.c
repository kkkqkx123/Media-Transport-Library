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

int main(int argc, char** argv) {
  const char* input = NULL;
  int w = 1920, h = 1080, frames = 120, bps = 8000000, fps = 60;
  const char* csv = NULL;
  const char* infmt = "nv12";
  const char* outpath = NULL;
  int level = 0; /* 0 = auto by resolution (42 for <=1080p, 51/52 for 4K) */
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
    {0, 0, 0, 0},
  };
  int o;
  while ((o = getopt_long(argc, argv, "i:w:H:n:b:f:c:F:o:L:", opts, NULL)) != -1) {
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
      default:
        fprintf(stderr, "usage: %s --input FILE --w W --h H [--frames N] [--bps B] [--fps F] [--csv FILE] [--infmt nv12|bgr3] [--level N]\n", argv[0]);
        return 1;
    }
  }
  if (!input) { fprintf(stderr, "missing --input file\n"); return 1; }

  MppFrameFormat mpp_fmt = MPP_FMT_YUV420SP;
  size_t fbytes = (size_t)w * (size_t)h * 3 / 2;
  if (strcmp(infmt, "bgr3") == 0) {
    mpp_fmt = MPP_FMT_BGR888;
    fbytes = (size_t)w * (size_t)h * 3;
  } else if (strcmp(infmt, "nv12") != 0) {
    fprintf(stderr, "unknown --infmt %s\n", infmt);
    return 1;
  }
  uint8_t* yuv = malloc(fbytes);
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
  if (mpp_buffer_get(NULL, &frm_buf, fbytes)) {
    fprintf(stderr, "mpp_buffer_get failed\n");
    return 1;
  }

  int done = 0;
  uint64_t total_out = 0;
  for (int i = 0; i < frames; i++) {
    if (fread(yuv, 1, fbytes, f) != fbytes) {
      fseek(f, 0, SEEK_SET);
      if (fread(yuv, 1, fbytes, f) != fbytes) break;
    }
    memcpy(mpp_buffer_get_ptr(frm_buf), yuv, fbytes);

    MppFrame frame = NULL;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, w);
    mpp_frame_set_height(frame, h);
    mpp_frame_set_hor_stride(frame, w);
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

  mpp_buffer_put(frm_buf);
  mpp_enc_cfg_deinit(cfg);
  mpp_destroy(ctx);
  fclose(f);
  if (outf) fclose(outf);
  free(yuv);
  free(cost);
  return done > 0 ? 0 : 1;
}
