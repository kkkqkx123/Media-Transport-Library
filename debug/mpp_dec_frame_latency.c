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

int main(int argc, char** argv) {
  const char* input = NULL;
  int max_au = 600;
  const char* csv = NULL;
  /* per-AU pull deadline. Must exceed the slowest real frame or the probe
   * misclassifies slow frames as no_output (was a hard ~25ms cap before). */
  int wait_ms = 50;

  static struct option opts[] = {
    {"input", required_argument, 0, 'i'},
    {"max-au", required_argument, 0, 'n'},
    {"csv", required_argument, 0, 'c'},
    {"wait-ms", required_argument, 0, 'm'},
    {0, 0, 0, 0},
  };
  int o;
  while ((o = getopt_long(argc, argv, "i:n:c:m:", opts, NULL)) != -1) {
    switch (o) {
      case 'i': input = optarg; break;
      case 'n': max_au = atoi(optarg); break;
      case 'c': csv = optarg; break;
      case 'm': wait_ms = atoi(optarg); break;
      default:
        fprintf(stderr, "usage: %s --input H264 [--max-au N] [--csv FILE] [--wait-ms MS]\n", argv[0]);
        return 1;
    }
  }
  if (!input) { fprintf(stderr, "missing --input H264 file\n"); return 1; }

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

  mpp_dec_cfg_deinit(cfg);
  mpp_destroy(ctx);
  free(data);
  free(cost);
  return framed > 0 ? 0 : 1;
}
