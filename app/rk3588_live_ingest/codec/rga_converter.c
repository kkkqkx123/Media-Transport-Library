#include "rga_converter.h"
#include "../hal/dma32_allocator.h"
#include <errno.h>
#include <linux/videodev2.h>
#include <rga/RgaApi.h>
#ifndef ALIGN
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* consecutive fd-path failures before sticky degradation to CPU copy */
#define RGA_FD_FAIL_LIMIT 3

struct nv12_buf {
  int      fd;
  uint32_t stride;
  size_t   size;
  bo_t     bo;          /* valid only when allocated via the RGA allocator */
  bool     via_rga;     /* true: c_RkRgaFree needed; false: dma32 heap */
  atomic_bool in_use;
};

struct rga_converter {
  int w, h, pool_size;
  struct nv12_buf* bufs;
  struct dma32_allocator* dma32;
  char version[128];

  /* stats (atomic for lock-free cross-thread reporting) */
  atomic_ullong calls, fails, pool_exhausted, cpu_fallbacks, cpu_fails;
  atomic_ullong time_ema_us;
  atomic_uint pool_in_use, pool_peak;

  /* fd-path degradation state */
  atomic_int fd_fail_streak;
  atomic_bool fd_degraded;
};

struct rga_converter* rga_converter_create(int w, int h, int pool_size) {
  struct rga_converter* c = calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->w = w; c->h = h; c->pool_size = pool_size;

  if (c_RkRgaInit() < 0) { perror("rga init"); free(c); return NULL; }

  atomic_init(&c->calls, 0);
  atomic_init(&c->fails, 0);
  atomic_init(&c->pool_exhausted, 0);
  atomic_init(&c->cpu_fallbacks, 0);
  atomic_init(&c->cpu_fails, 0);
  atomic_init(&c->time_ema_us, 0);
  atomic_init(&c->pool_in_use, 0);
  atomic_init(&c->pool_peak, 0);
  atomic_init(&c->fd_fail_streak, 0);
  atomic_init(&c->fd_degraded, false);

  c->dma32 = dma32_allocator_create(); /* may be NULL on old kernels */

  c->bufs = calloc((size_t)pool_size, sizeof(*c->bufs));
  size_t nv12_size = (size_t)ALIGN(w, 16) * h * 3 / 2;
  for (int i = 0; i < pool_size; i++) {
    struct nv12_buf* b = &c->bufs[i];
    atomic_init(&b->in_use, false);
    b->fd     = -1;
    b->stride = (uint32_t)ALIGN(w, 16);
    b->size   = nv12_size;
    b->via_rga = false;

    if (c->dma32 &&
        dma32_allocator_alloc(c->dma32, nv12_size, &b->fd) == 0) {
      /* DMA32 heap buffer: dmabuf fd feeds both RGA and MPP zero-copy */
      continue;
    }
    /* fallback: legacy RGA allocator (may land above 4GiB on some boards) */
    bo_t* bo = &b->bo;
    if (c_RkRgaGetAllocBufferCache(bo, w, h, 12) < 0) {
      fprintf(stderr, "rga: alloc buf[%d] failed\n", i);
      goto fail;
    }
    if (c_RkRgaGetMmap(bo) < 0) perror("rga mmap");
    if (c_RkRgaGetBufferFd(bo, &b->fd) < 0) {
      fprintf(stderr, "rga: get fd failed\n");
      goto fail;
    }
    b->via_rga = true;
  }
  printf("rga: ready, %d NV12 bufs (%dx%d) %s\n", pool_size, w, h,
         c->dma32 ? "dma32-heap" : "rga-alloc");
  /* one-shot driver version record, informational only */
  snprintf(c->version, sizeof(c->version), "unknown");
#if defined(__has_include)
#if __has_include(<rga/im2d_common.h>)
  {
    extern const char* querystring(int name);
    const char* vendor = querystring(0);   /* RGA_VENDOR */
    const char* ver = querystring(1);      /* RGA_VERSION */
    if (vendor || ver)
      snprintf(c->version, sizeof(c->version), "%s %s",
               vendor ? vendor : "", ver ? ver : "");
  }
#endif
#endif
  return c;

fail:
  for (int j = 0; j < pool_size; j++) {
    if (c->bufs[j].via_rga) c_RkRgaFree(&c->bufs[j].bo);
    else if (c->bufs[j].fd >= 0 && c->dma32)
      dma32_allocator_free_buf(c->dma32, c->bufs[j].fd);
  }
  free(c->bufs);
  if (c->dma32) dma32_allocator_free(c->dma32);
  c_RkRgaDeInit();
  free(c);
  return NULL;
}

void rga_converter_free(struct rga_converter* c) {
  if (!c) return;
  for (int i = 0; i < c->pool_size; i++) {
    struct nv12_buf* b = &c->bufs[i];
    if (b->via_rga) {
      if (b->fd >= 0) close(b->fd);
      c_RkRgaFree(&b->bo);
    } else if (b->fd >= 0 && c->dma32) {
      dma32_allocator_free_buf(c->dma32, b->fd);
    }
  }
  free(c->bufs);
  if (c->dma32) dma32_allocator_free(c->dma32);
  c_RkRgaDeInit();
  free(c);
}

void rga_converter_release(struct rga_converter* c, int pool_idx) {
  if (!c || pool_idx < 0 || pool_idx >= c->pool_size) return;
  if (atomic_exchange(&c->bufs[pool_idx].in_use, false)) {
    unsigned cur = atomic_load(&c->pool_in_use);
    if (cur > 0) atomic_fetch_sub(&c->pool_in_use, 1);
  }
}

void rga_converter_stats(struct rga_converter* c, struct rga_stats* out) {
  if (!out) return;
  if (!c) {
    memset(out, 0, sizeof(*out));
    return;
  }
  out->calls          = atomic_load(&c->calls);
  out->fails          = atomic_load(&c->fails);
  out->pool_exhausted = atomic_load(&c->pool_exhausted);
  out->cpu_fallbacks  = atomic_load(&c->cpu_fallbacks);
  out->cpu_fails      = atomic_load(&c->cpu_fails);
  out->time_ema_us    = atomic_load(&c->time_ema_us);
  out->pool_in_use    = atomic_load(&c->pool_in_use);
  out->pool_peak      = atomic_load(&c->pool_peak);
  out->pool_size      = (uint32_t)c->pool_size;
  out->degraded       = atomic_load(&c->fd_degraded) ? 1 : 0;
}

uint32_t rga_converter_in_use(struct rga_converter* c) {
  return c ? atomic_load(&c->pool_in_use) : 0;
}

uint32_t rga_converter_peak(struct rga_converter* c) {
  return c ? atomic_load(&c->pool_peak) : 0;
}

int rga_converter_degraded(struct rga_converter* c) {
  return (c && atomic_load(&c->fd_degraded)) ? 1 : 0;
}

const char* rga_converter_version(struct rga_converter* c) {
  return (c && c->version[0]) ? c->version : "unknown";
}

/* ---- degraded CPU path: packed source → NV12, works for both RGA and
 * DMA32 destination buffers. DMA sync windows guard cached-heap coherence. */

static uint8_t* cpu_map_dst(struct rga_converter* c, struct nv12_buf* dst,
                            bool* mapped) {
  if (dst->via_rga) {
    void* dva = c_RkRgaGetMmap(&dst->bo);
    *mapped = false;
    return dva ? (uint8_t*)dva : NULL;
  }
  uint8_t* d = mmap(NULL, dst->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    dst->fd, 0);
  if (d == MAP_FAILED) return NULL;
  *mapped = true;
  return d;
}

static void cpu_unmap_dst(struct rga_converter* c, struct nv12_buf* dst,
                          uint8_t* d, bool mapped) {
  (void)c; (void)dst;
  if (mapped && d) munmap(d, dst->size);
}

static void bgr_to_nv12_row(const uint8_t* srow, uint8_t* yrow, uint8_t* uvrow,
                            uint32_t w, uint32_t y) {
  for (uint32_t x = 0; x < w; x++) {
    const uint8_t* px = srow + x * 3;
    int b = px[0], g = px[1], r = px[2];
    yrow[x] = (uint8_t)((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    if ((y & 1) == 0 && (x & 1) == 0) {
      int cb = (-38 * r - 74 * g + 112 * b + 128) >> 8;
      int cr = (112 * r - 94 * g - 18 * b + 128) >> 8;
      uvrow[(x / 2) * 2]     = (uint8_t)(cb + 128);
      uvrow[(x / 2) * 2 + 1] = (uint8_t)(cr + 128);
    }
  }
}

static void packed422_to_nv12_row(const uint8_t* srow, uint8_t* yrow,
                                 uint8_t* uvrow, uint32_t w, bool uyvy) {
  for (uint32_t x = 0; x < w; x += 2) {
    int u, y0, v, y1;
    if (uyvy) { u = srow[0]; y0 = srow[1]; v = srow[2]; y1 = srow[3]; }
    else      { y0 = srow[0]; u = srow[1]; y1 = srow[2]; v = srow[3]; }
    yrow[x] = (uint8_t)y0;
    if (x + 1 < w) yrow[x + 1] = (uint8_t)y1;
    uvrow[(x / 2) * 2]     = (uint8_t)u;
    uvrow[(x / 2) * 2 + 1] = (uint8_t)v;
    srow += 4;
  }
}

static int cpu_convert(struct rga_converter* c, int src_fd, uint32_t src_stride,
                       uint32_t src_fourcc, struct nv12_buf* dst) {
  size_t src_size = (size_t)src_stride * (size_t)c->h;
  dma32_sync_start(src_fd);
  uint8_t* src = mmap(NULL, src_size, PROT_READ, MAP_SHARED, src_fd, 0);
  if (src == MAP_FAILED) { dma32_sync_end(src_fd); return -EIO; }
  bool dst_mapped = false;
  dma32_sync_start(dst->fd);
  uint8_t* d = cpu_map_dst(c, dst, &dst_mapped);
  if (!d) { munmap(src, src_size); dma32_sync_end(src_fd); return -EIO; }

  uint32_t w = (uint32_t)c->w, h = (uint32_t)c->h;
  bool is_nv12 = (src_fourcc == V4L2_PIX_FMT_NV12);
  bool is_uyvy = (src_fourcc == V4L2_PIX_FMT_UYVY);
  bool is_yuyv = (src_fourcc == V4L2_PIX_FMT_YUYV);
  if (is_nv12) {
    size_t y_size = (size_t)src_stride * h;
    for (uint32_t y = 0; y < h; y++) {
      uint8_t* yrow = d + (size_t)y * dst->stride;
      const uint8_t* srow = src + (size_t)y * src_stride;
      size_t copy = w < src_stride ? w : src_stride;
      memcpy(yrow, srow, copy);
    }
    size_t uv_src_off = y_size;
    uint8_t* uv_dst = d + (size_t)dst->stride * h;
    for (uint32_t y = 0; y < h / 2; y++) {
      uint8_t* uvrow = uv_dst + (size_t)y * dst->stride;
      const uint8_t* srow = src + uv_src_off + (size_t)y * src_stride;
      size_t copy = w < src_stride ? w : src_stride;
      memcpy(uvrow, srow, copy);
    }
  } else if (is_uyvy || is_yuyv) {
    for (uint32_t y = 0; y < h; y++) {
      const uint8_t* srow = src + (size_t)y * src_stride;
      uint8_t* yrow = d + (size_t)y * dst->stride;
      uint8_t* uvrow = d + (size_t)dst->stride * h +
                       (size_t)(y / 2) * dst->stride;
      if ((y & 1) == 0)
        packed422_to_nv12_row(srow, yrow, uvrow, w, is_uyvy);
      else
        packed422_to_nv12_row(srow, yrow, uvrow, w, is_uyvy);
    }
  } else {
    for (uint32_t y = 0; y < h; y++) {
      const uint8_t* srow = src + (size_t)y * src_stride;
      uint8_t* yrow = d + (size_t)y * dst->stride;
      uint8_t* uvrow = d + (size_t)dst->stride * h +
                       (size_t)(y / 2) * dst->stride;
      bgr_to_nv12_row(srow, yrow, (y & 1) == 0 ? uvrow : uvrow, w, y);
    }
  }
  cpu_unmap_dst(c, dst, d, dst_mapped);
  dma32_sync_end(dst->fd);
  munmap(src, src_size);
  dma32_sync_end(src_fd);
  return 0;
}

int rga_convert(struct rga_converter* c, int src_fd, uint32_t src_stride,
                uint32_t src_fourcc,
                int* dst_fd, uint32_t* dst_stride, int* dst_idx) {
  if (!c || src_fd < 0) return -EINVAL;

  int idx = -1;
  for (int i = 0; i < c->pool_size; i++) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&c->bufs[i].in_use, &expected, true)) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    atomic_fetch_add(&c->pool_exhausted, 1);
    return -EAGAIN;
  }
  struct nv12_buf* b = &c->bufs[idx];
  unsigned in_use = atomic_fetch_add(&c->pool_in_use, 1) + 1;
  unsigned peak = atomic_load(&c->pool_peak);
  if (in_use > peak) atomic_store(&c->pool_peak, in_use);

  if (atomic_load(&c->fd_degraded)) {
    int rc = cpu_convert(c, src_fd, src_stride, src_fourcc, b);
    if (rc == 0) {
      atomic_fetch_add(&c->cpu_fallbacks, 1);
      atomic_fetch_add(&c->calls, 1);
      *dst_fd = b->fd; *dst_stride = b->stride; *dst_idx = idx;
      return 0;
    }
    atomic_store(&b->in_use, false);
    atomic_fetch_sub(&c->pool_in_use, 1);
    atomic_fetch_add(&c->cpu_fails, 1);
    atomic_fetch_add(&c->fails, 1);
    return rc;
  }

  int src_fmt = RK_FORMAT_BGR_888;
  uint32_t src_wstride_px = src_stride / 3;
  if (src_fourcc == V4L2_PIX_FMT_UYVY) {
    src_fmt = RK_FORMAT_UYVY_422;
    src_wstride_px = src_stride / 2;
  } else if (src_fourcc == V4L2_PIX_FMT_YUYV) {
    src_fmt = RK_FORMAT_YUYV_422;
    src_wstride_px = src_stride / 2;
  } else if (src_fourcc == V4L2_PIX_FMT_NV12) {
    src_fmt = RK_FORMAT_YCbCr_420_SP;
    src_wstride_px = src_stride;
  }
  if (src_wstride_px < (uint32_t)c->w) src_wstride_px = (uint32_t)c->w;

  rga_info_t src = {0}, dst = {0};
  src.fd      = src_fd;
  src.format  = src_fmt;
  src.mmuFlag = 1;
  src.color_space_mode = 2;
  rga_set_rect(&src.rect, 0, 0, c->w, c->h,
               (int)src_wstride_px, c->h, src_fmt);

  dst.fd      = b->fd;
  dst.format  = RK_FORMAT_YCbCr_420_SP;
  dst.mmuFlag = 1;
  dst.color_space_mode = 2;
  rga_set_rect(&dst.rect, 0, 0, c->w, c->h,
               (int)b->stride, c->h, RK_FORMAT_YCbCr_420_SP);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  int ret = c_RkRgaBlit(&src, &dst, NULL);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (ret < 0) {
    atomic_fetch_add(&c->fails, 1);
    int streak = atomic_fetch_add(&c->fd_fail_streak, 1) + 1;
    atomic_store(&b->in_use, false);
    atomic_fetch_sub(&c->pool_in_use, 1);
    if (streak >= RGA_FD_FAIL_LIMIT &&
        !atomic_exchange(&c->fd_degraded, true)) {
      fprintf(stderr,
              "rga: %d consecutive blit failures, degrading to CPU path\n",
              streak);
    }
    return ret;
  }
  uint64_t cost_us = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000ULL +
                     (uint64_t)(t1.tv_nsec - t0.tv_nsec) / 1000ULL;
  uint64_t prev = atomic_load(&c->time_ema_us);
  uint64_t ema = prev ? (prev * 9 + cost_us + 5) / 10 : cost_us;
  atomic_store(&c->time_ema_us, ema);
  atomic_store(&c->fd_fail_streak, 0);
  atomic_fetch_add(&c->calls, 1);
  *dst_fd = b->fd; *dst_stride = b->stride; *dst_idx = idx;
  return 0;
}
