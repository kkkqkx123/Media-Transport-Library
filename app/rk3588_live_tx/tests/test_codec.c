#include <drm_fourcc.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../codec/rga_converter.h"
#include "../codec/mpp_encoder.h"
#include "slab_pool.h"
#include "sei_timestamp.h"
#include "../io/v4l2_capturer.h"

static atomic_bool g_run = true;
static void sig(int s) { (void)s; atomic_store(&g_run, false); }
static uint64_t g_frames;
static FILE* g_file = NULL;

/* ---- Encode mode: capture → RGA → MPP enc → file ---- */
static void enc_on_frame(void* u, const struct v4l2_frame* f) {
  void** ctx = (void**)u;
  struct codec_plugin* enc = (struct codec_plugin*)ctx[0];
  struct rga_converter* rga = (struct rga_converter*)ctx[1];
  struct slab_pool* pool = (struct slab_pool*)ctx[2];

  int nv12_fd, nv12_idx; uint32_t nv12_stride;
  uint32_t src_fourcc = v4l2_capturer_fourcc(ctx[3]);
  if (rga_convert(rga, f->dma_fd, f->stride, src_fourcc,
                  &nv12_fd, &nv12_stride, &nv12_idx) != 0) {
    v4l2_capturer_release(ctx[3], f->buf_index, f->generation);
    return;
  }

  enc->send(enc->priv, nv12_fd, nv12_stride);

  uint8_t* data; size_t size; int key;
  if (enc->recv(enc->priv, &data, &size, &key) == 1) {
    if (g_file) { fwrite(data, 1, size, g_file); fflush(g_file); }
    /* data allocated from slab by encoder, caller must free back to pool */
    slab_free(pool, data);
  }
  rga_converter_release(rga, nv12_idx);
  v4l2_capturer_release(ctx[3], f->buf_index, f->generation);
  g_frames++;
  if (g_frames % 100 == 0) printf("enc: %llu frames\n", (unsigned long long)g_frames);
}

/* ---- main ---- */
int main(int argc, char** argv) {
  signal(SIGINT, sig);
  const char* mode = (argc > 1) ? argv[1] : "encode";
  const char* vdev = (argc > 2) ? argv[2] : "/dev/video0";
  enum codec_format venc = CODEC_FMT_H264;
  const char* vsuffix = "h264";
  if (argc > 3) {
    if (!strcmp(argv[3], "h265") || !strcmp(argv[3], "hevc") ||
        !strcmp(argv[3], "H265") || !strcmp(argv[3], "HEVC")) {
      venc = CODEC_FMT_H265;
      vsuffix = "h265";
    } else if (strcmp(argv[3], "h264") && strcmp(argv[3], "H264")) {
      fprintf(stderr, "Usage: test_codec [encode] [vdev] [h264|h265]\n");
      return 1;
    }
  }
  char out_name[64];
  snprintf(out_name, sizeof(out_name), "test.%s", vsuffix);
  int w = 1920, h = 1080;

  printf("=== Codec Test [%s] (encode-only) ===\n", mode);

  if (strcmp(mode, "encode") == 0) {
    g_file = fopen(out_name, "wb");
    if (!g_file) { fprintf(stderr,"FAIL: fopen\n"); return 1; }
    struct rga_converter* rga = rga_converter_create(w, h, 4);
    struct slab_pool* pool = slab_pool_create(16, 512*1024);
    struct codec_config cfg = {.format=venc,.width=w,.height=h,.fps=60,
        .bitrate_bps=40000000,.gop_size=60,.low_latency=true,.num_slices=1};
    struct codec_plugin* enc = mpp_encoder_create(&cfg, pool);
    if (!rga||!pool||!enc) { fprintf(stderr,"FAIL: init\n"); return 1; }
    void* ctx[4] = {enc, rga, pool, NULL};
    struct v4l2_capturer* cap = v4l2_capturer_create(vdev,w,h,60,6,enc_on_frame,ctx);
    if (!cap) return 1;
    ctx[3] = cap;
    printf("Encode mode: -> %s\n", out_name);
    time_t t0 = time(NULL); v4l2_capturer_start(cap);
    while (atomic_load(&g_run)) usleep(100000);
    v4l2_capturer_stop(cap); int el = (int)(time(NULL)-t0);
    printf("\n%llu frames -> %s (%ds)\n",(unsigned long long)g_frames,out_name,el);
    v4l2_capturer_free(cap); enc->free(enc->priv); free(enc);
    slab_pool_free(pool); rga_converter_free(rga); fclose(g_file);
  }
  else { fprintf(stderr,"Usage: test_codec [encode] [vdev] [h264|h265]\n"); return 1; }

  printf("=== PASS ===\n");
  return 0;
}
