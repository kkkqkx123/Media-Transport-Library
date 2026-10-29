/* SPDX-License-Identifier: BSD-3-Clause
 * RGA hardware BGR3→NV12 converter.
 *
 * Pool semantics: NV12 output buffers carry an occupancy flag; rga_convert()
 * acquires a free slot, and the consumer must rga_converter_release() it once
 * the encoder is done (MPP import is async). Exhaustion drops the frame.
 *
 * Allocation: DMA32 dma-heap first (RGA's 32-bit MMU cannot map buffers in
 * the >4GiB physical window); falls back to the legacy RGA allocator.
 *
 * Fallback chain: fd→fd hardware blit → after 3 consecutive fd-path failures,
 * a sticky CPU (mmap + software) conversion path; single failures just drop
 * the frame and count.
 */

#include "rga_converter.h"
#include "../hal/dma32_allocator.h"
#include <errno.h>
#include <rga/RgaApi.h>
#ifndef ALIGN
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

  /* stats (atomic for lock-free cross-thread reporting) */
  atomic_ullong calls, fails, pool_exhausted, cpu_fallbacks;

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
  atomic_store(&c->bufs[pool_idx].in_use, false);
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
}

/* ---- degraded CPU path: mmap BGR3 → software convert → NV12 ---- */

static int cpu_convert(struct rga_converter* c, int src_fd, uint32_t src_stride,
                       struct nv12_buf* dst) {
  size_t src_size = (size_t)src_stride * c->h;
  uint8_t* src = mmap(NULL, src_size, PROT_READ, MAP_SHARED, src_fd, 0);
  if (src == MAP_FAILED) return -EIO;
  void* dva = c_RkRgaGetMmap(&dst->bo);
  uint8_t* d = dva ? (uint8_t*)dva : NULL;
  if (!d) { munmap(src, src_size); return -EIO; }

  uint32_t w = (uint32_t)c->w, h = (uint32_t)c->h;
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* srow = src + (size_t)y * src_stride;
    uint8_t* yrow = d + (size_t)y * dst->stride;
    for (uint32_t x = 0; x < w; x++) {
      const uint8_t* px = srow + x * 3;      /* BGR888 */
      int b = px[0], g = px[1], r = px[2];
      yrow[x] = (uint8_t)((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
      if ((y & 1) == 0 && (x & 1) == 0) {
        uint8_t* uv = d + dst->stride * h + (size_t)(y / 2) * dst->stride + (x / 2) * 2;
        int cb = (-38 * r - 74 * g + 112 * b + 128) >> 8;
        int cr = (112 * r - 94 * g - 18 * b + 128) >> 8;
        uv[0] = (uint8_t)(cb + 128);
        uv[1] = (uint8_t)(cr + 128);
      }
    }
  }
  munmap(src, src_size);
  return 0;
}

int rga_convert(struct rga_converter* c, int src_fd, uint32_t src_stride,
                int* dst_fd, uint32_t* dst_stride, int* dst_idx) {
  if (!c || src_fd < 0) return -EINVAL;

  /* acquire a free pool slot (drop the frame if all busy) */
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

  /* degraded CPU path */
  if (atomic_load(&c->fd_degraded)) {
    int rc = -EIO;
    if (b->via_rga) rc = cpu_convert(c, src_fd, src_stride, b);
    if (rc == 0) {
      atomic_fetch_add(&c->cpu_fallbacks, 1);
      atomic_fetch_add(&c->calls, 1);
      *dst_fd = b->fd; *dst_stride = b->stride; *dst_idx = idx;
      return 0;
    }
    atomic_store(&b->in_use, false);
    atomic_fetch_add(&c->fails, 1);
    return rc;
  }

  /* src_stride is V4L2 bytesperline; RGA wstride for BGR888 is in pixels */
  uint32_t src_wstride_px = src_stride / 3;
  if (src_wstride_px < (uint32_t)c->w) src_wstride_px = (uint32_t)c->w;

  rga_info_t src = {0}, dst = {0};
  src.fd      = src_fd;
  src.format  = RK_FORMAT_BGR_888;
  src.mmuFlag = 1;
  src.color_space_mode = 2; /* BT.709 input */
  rga_set_rect(&src.rect, 0, 0, c->w, c->h,
               (int)src_wstride_px, c->h, RK_FORMAT_BGR_888);

  dst.fd      = b->fd;
  dst.format  = RK_FORMAT_YCbCr_420_SP;
  dst.mmuFlag = 1;
  dst.color_space_mode = 2; /* BT.709 output — HD standard for 1080p */
  rga_set_rect(&dst.rect, 0, 0, c->w, c->h,
               (int)b->stride, c->h, RK_FORMAT_YCbCr_420_SP);

  int ret = c_RkRgaBlit(&src, &dst, NULL);
  if (ret < 0) {
    atomic_fetch_add(&c->fails, 1);
    int streak = atomic_fetch_add(&c->fd_fail_streak, 1) + 1;
    atomic_store(&b->in_use, false);
    if (streak >= RGA_FD_FAIL_LIMIT &&
        !atomic_exchange(&c->fd_degraded, true)) {
      fprintf(stderr,
              "rga: %d consecutive blit failures, degrading to CPU path\n",
              streak);
    }
    return ret;
  }
  atomic_store(&c->fd_fail_streak, 0);
  atomic_fetch_add(&c->calls, 1);
  *dst_fd = b->fd; *dst_stride = b->stride; *dst_idx = idx;
  return 0;
}
