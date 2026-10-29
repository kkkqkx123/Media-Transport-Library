/* SPDX-License-Identifier: BSD-3-Clause
 * Phase 3: Codec verification.
 * Modes:
 *   rga:     V4L2 → RGA(BGR3→NV12) → DRM  (正确颜色回环)
 *   encode:  V4L2 → RGA → MPP enc → .h264 file
 *   loopback:V4L2 → RGA → MPP enc → MPP dec → DRM  (本地编解码回环)
 *
 * Usage: ./test_codec [rga|encode|loopback] [/dev/video0] [/dev/dri/card0]
 */

#include <drm_fourcc.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../codec/rga_converter.h"
#include "../codec/mpp_encoder.h"
#include "../codec/mpp_decoder.h"
#include "../hal/slab_pool.h"
#include "../hal/sei_timestamp.h"
#include "../io/v4l2_capturer.h"
#include "../io/drm_renderer.h"

static atomic_bool g_run = true;
static void sig(int s) { (void)s; atomic_store(&g_run, false); }
static uint64_t g_frames;
static FILE* g_file = NULL;

/* ---- RGA mode: capture → RGA → display ---- */
static void rga_on_frame(void* u, const struct v4l2_frame* f) {
  void** ctx = (void**)u;
  struct drm_renderer* r = (struct drm_renderer*)ctx[0];
  struct rga_converter* rga = (struct rga_converter*)ctx[1];

  int nv12_fd, nv12_idx; uint32_t nv12_stride;
  if (rga_convert(rga, f->dma_fd, f->stride, &nv12_fd, &nv12_stride,
                  &nv12_idx) == 0) {
    drm_renderer_display(r, nv12_fd, nv12_stride, 0);
    rga_converter_release(rga, nv12_idx);
  }
  v4l2_capturer_release(ctx[2], f->buf_index, f->generation);
  g_frames++;
  if (g_frames % 100 == 0) printf("rga: %llu frames\n", (unsigned long long)g_frames);
}

/* ---- Encode mode: capture → RGA → MPP enc → file ---- */
static void enc_on_frame(void* u, const struct v4l2_frame* f) {
  void** ctx = (void**)u;
  struct codec_plugin* enc = (struct codec_plugin*)ctx[0];
  struct rga_converter* rga = (struct rga_converter*)ctx[1];
  struct slab_pool* pool = (struct slab_pool*)ctx[2];

  int nv12_fd, nv12_idx; uint32_t nv12_stride;
  if (rga_convert(rga, f->dma_fd, f->stride, &nv12_fd, &nv12_stride,
                  &nv12_idx) != 0) {
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

/* ---- Loopback mode: capture → RGA → MPP enc → SEI inject → MPP dec → DRM ---- */
struct lb_ctx {
  struct drm_renderer* renderer;
  struct rga_converter* rga;
  struct codec_plugin* enc;
  struct codec_plugin* dec;
  struct slab_pool* pool;
  struct v4l2_capturer* capture;
};

/* rolling stats for end-to-end latency */
struct latency_stats {
  uint64_t min_ns, max_ns, sum_ns;
  uint64_t count;
  uint64_t displayed;
};

static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void lb_on_frame(void* u, const struct v4l2_frame* vf) {
  struct lb_ctx* ctx = (struct lb_ctx*)u;

  /* capture timestamp — start of e2e measurement */
  uint64_t cap_ts = now_ns();

  /* RGA: BGR3 → NV12 */
  int nv12_fd, nv12_idx; uint32_t nv12_stride;
  if (rga_convert(ctx->rga, vf->dma_fd, vf->stride, &nv12_fd, &nv12_stride,
                  &nv12_idx) != 0) {
    v4l2_capturer_release(ctx->capture, vf->buf_index, vf->generation);
    return;
  }

  /* MPP encode */
  ctx->enc->send(ctx->enc->priv, nv12_fd, nv12_stride);
  uint8_t* data; size_t size; int key;
  if (ctx->enc->recv(ctx->enc->priv, &data, &size, &key) != 1) return;

  /* Inject SEI timestamp into coded stream.
   * Shift data forward 25 bytes in the slab buffer, insert SEI NAL prefix.
   * The SEI carries cap_ts and frame_seq through the codec pipeline. */
  if (size + SEI_TS_NAL_SIZE <= 512 * 1024) { /* slab block is 512KB */
    memmove(data + SEI_TS_NAL_SIZE, data, size);
    sei_timestamp_build(data, (uint32_t)vf->frame_seq, cap_ts);
    size += SEI_TS_NAL_SIZE;
  }

  /* extract SEI before feeding decoder (also strips it from the stream) */
  uint64_t extracted_ts = 0;
  uint32_t extracted_seq = 0;
  size_t clean_size = size;
  sei_timestamp_extract(data, &clean_size, &extracted_seq, &extracted_ts);

  /* MPP decode (feed clean stream without SEI) */
  int out_fd;
  uint32_t out_h_stride, out_v_stride;
  int r = mpp_dec_feed(ctx->dec->priv, data, clean_size, &out_fd, &out_h_stride,
                       &out_v_stride);
  slab_free(ctx->pool, data); /* return to slab pool */

  static struct latency_stats g_lat = {.min_ns = UINT64_MAX};
  if (r == 1) {
    drm_renderer_display(ctx->renderer, out_fd, out_h_stride, out_v_stride);
    /* e2e latency: capture → display */
    uint64_t e2e_ns = now_ns() - extracted_ts;
    if (e2e_ns < g_lat.min_ns) g_lat.min_ns = e2e_ns;
    if (e2e_ns > g_lat.max_ns) g_lat.max_ns = e2e_ns;
    g_lat.sum_ns += e2e_ns;
    g_lat.count++;
    g_lat.displayed++;
  }
  g_frames++;
  rga_converter_release(ctx->rga, nv12_idx);
  v4l2_capturer_release(ctx->capture, vf->buf_index, vf->generation);
  if (g_frames % 100 == 0) {
    uint64_t avg = g_lat.count ? g_lat.sum_ns / g_lat.count : 0;
    printf("lb: %llu frames (%llu displayed) | e2e min=%llu avg=%llu max=%llu us\n",
           (unsigned long long)g_frames, (unsigned long long)g_lat.displayed,
           (unsigned long long)(g_lat.min_ns / 1000),
           (unsigned long long)(avg / 1000),
           (unsigned long long)(g_lat.max_ns / 1000));
  }
}

/* ---- main ---- */
int main(int argc, char** argv) {
  signal(SIGINT, sig);
  const char* mode = (argc > 1) ? argv[1] : "rga";
  const char* vdev = (argc > 2) ? argv[2] : "/dev/video0";
  const char* ddev = (argc > 3) ? argv[3] : "/dev/dri/card0";
  int w = 1920, h = 1080;

  printf("=== Phase 3 Codec Test [%s] ===\n", mode);

  if (strcmp(mode, "rga") == 0) {
    struct rga_converter* rga = rga_converter_create(w, h, 4);
    if (!rga) { fprintf(stderr,"FAIL: rga\n"); return 1; }
    struct drm_renderer* drm = drm_renderer_create(ddev, "HDMI-A-1","1920x1080@60",
                                                     w,h,DRM_FORMAT_NV12,5);  /* buf_count=5 for safe fb recycling */
    if (!drm) { rga_converter_free(rga); return 1; }
    void* ctx[3] = {drm, rga, NULL};
    struct v4l2_capturer* cap = v4l2_capturer_create(vdev,w,h,60,6,
                                                       rga_on_frame, ctx);
    if (!cap) { drm_renderer_free(drm); rga_converter_free(rga); return 1; }
    ctx[2] = cap; /* release handle used inside the callback */
    printf("RGA mode: BGR3 capture → RGA → NV12 display (correct colors)\n");
    time_t t0 = time(NULL); v4l2_capturer_start(cap);
    while (atomic_load(&g_run)) usleep(100000);
    v4l2_capturer_stop(cap); int el = (int)(time(NULL)-t0);
    printf("\n%llu frames in %ds (%.1f fps)\n",(unsigned long long)g_frames,el,
           el?(double)g_frames/el:0);
    v4l2_capturer_free(cap); drm_renderer_free(drm); rga_converter_free(rga);
  }
  else if (strcmp(mode, "encode") == 0) {
    g_file = fopen("test.h264", "wb");
    if (!g_file) { fprintf(stderr,"FAIL: fopen\n"); return 1; }
    struct rga_converter* rga = rga_converter_create(w, h, 4);
    struct slab_pool* pool = slab_pool_create(16, 512*1024);
    struct codec_config cfg = {.format=CODEC_FMT_H264,.width=w,.height=h,.fps=60,
        .bitrate_bps=40000000,.gop_size=60,.low_latency=true,.num_slices=1};
    struct codec_plugin* enc = mpp_encoder_create(&cfg, pool);
    if (!rga||!pool||!enc) { fprintf(stderr,"FAIL: init\n"); return 1; }
    void* ctx[4] = {enc, rga, pool, NULL};
    struct v4l2_capturer* cap = v4l2_capturer_create(vdev,w,h,60,6,enc_on_frame,ctx);
    if (!cap) return 1;
    ctx[3] = cap;
    printf("Encode mode: → test.h264\n");
    time_t t0 = time(NULL); v4l2_capturer_start(cap);
    while (atomic_load(&g_run)) usleep(100000);
    v4l2_capturer_stop(cap); int el = (int)(time(NULL)-t0);
    printf("\n%llu frames → test.h264 (%ds)\n",(unsigned long long)g_frames,el);
    v4l2_capturer_free(cap); enc->free(enc->priv); free(enc);
    slab_pool_free(pool); rga_converter_free(rga); fclose(g_file);
  }
  else if (strcmp(mode, "loopback") == 0) {
    struct rga_converter* rga = rga_converter_create(w, h, 4);
    struct slab_pool* pool = slab_pool_create(16, 512*1024);
    struct codec_config cfg = {.format=CODEC_FMT_H264,.width=w,.height=h,.fps=60,
        .bitrate_bps=40000000,.gop_size=60,.low_latency=true,.num_slices=1};
    struct codec_plugin* enc = mpp_encoder_create(&cfg, pool);
    struct codec_plugin* dec = mpp_decoder_create(&cfg);
    struct drm_renderer* drm = drm_renderer_create(ddev,"HDMI-A-1","1920x1080@60",
                                                     w,h,DRM_FORMAT_NV12,5);  /* buf_count=5 for safe fb recycling */
    if (!rga||!pool||!enc||!dec||!drm) { fprintf(stderr,"FAIL: init\n"); return 1; }
    struct lb_ctx ctx = {drm, rga, enc, dec, pool, NULL};
    struct v4l2_capturer* cap = v4l2_capturer_create(vdev,w,h,60,6,lb_on_frame,&ctx);
    if (!cap) return 1;
    ctx.capture = cap;
    printf("Loopback: V4L2→RGA→MPPenc→MPPdec→DRM\n");
    time_t t0 = time(NULL); v4l2_capturer_start(cap);
    while (atomic_load(&g_run)) usleep(100000);
    v4l2_capturer_stop(cap); int el = (int)(time(NULL)-t0);
    printf("\n%llu frames in %ds (%.1f fps)\n",(unsigned long long)g_frames,el,
           el?(double)g_frames/el:0);
    v4l2_capturer_free(cap); enc->free(enc->priv); free(enc);
    dec->free(dec->priv); free(dec);
    drm_renderer_free(drm); slab_pool_free(pool);
    rga_converter_free(rga);
  }
  else { fprintf(stderr,"Usage: test_codec [rga|encode|loopback]\n"); return 1; }

  printf("=== PASS ===\n");
  return 0;
}
