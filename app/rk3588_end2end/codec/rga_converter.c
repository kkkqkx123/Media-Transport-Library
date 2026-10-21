/* SPDX-License-Identifier: BSD-3-Clause
 * RGA hardware BGR3→NV12 converter using RGA native buffers.
 */

#include "rga_converter.h"
#include <errno.h>
#include <rga/RgaApi.h>
#ifndef ALIGN
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct rga_converter {
  int w, h, pool_size, next_buf;
  struct nv12_buf {
    int      fd;
    uint32_t stride;
    size_t   size;
    bo_t     bo;
  }* bufs;
};

struct rga_converter* rga_converter_create(int w, int h, int pool_size) {
  struct rga_converter* c = calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->w = w; c->h = h; c->pool_size = pool_size;

  if (c_RkRgaInit() < 0) { perror("rga init"); free(c); return NULL; }

  c->bufs = calloc(pool_size, sizeof(*c->bufs));
  for (int i = 0; i < pool_size; i++) {
    bo_t* bo = &c->bufs[i].bo;
    /* NV12: 1.5 bytes per pixel */
    if (c_RkRgaGetAllocBufferCache(bo, w, h, 12) < 0) {
      fprintf(stderr,"rga: alloc buf[%d] failed\n",i);
      for (int j=0;j<i;j++) c_RkRgaFree(&c->bufs[j].bo);
      free(c->bufs); free(c); return NULL;
    }
    if (c_RkRgaGetMmap(bo) < 0) { perror("rga mmap"); }
    if (c_RkRgaGetBufferFd(bo, &c->bufs[i].fd) < 0) {
      fprintf(stderr,"rga: get fd failed\n");
      for (int j=0;j<=i;j++) c_RkRgaFree(&c->bufs[j].bo);
      free(c->bufs); free(c); return NULL;
    }
    c->bufs[i].stride = ALIGN(w, 16);
    c->bufs[i].size   = c->bufs[i].stride * h * 3 / 2;
  }
  printf("rga: ready, %d NV12 bufs (%dx%d)\n", pool_size, w, h);
  return c;
}

void rga_converter_free(struct rga_converter* c) {
  if (!c) return;
  for (int i = 0; i < c->pool_size; i++) {
    if (c->bufs[i].fd >= 0) close(c->bufs[i].fd);
    c_RkRgaFree(&c->bufs[i].bo);
  }
  free(c->bufs);
  c_RkRgaDeInit();
  free(c);
}

int rga_convert(struct rga_converter* c, int src_fd, uint32_t src_stride,
                int* dst_fd, uint32_t* dst_stride) {
  if (!c || src_fd < 0) return -EINVAL;

  int idx = c->next_buf;
  c->next_buf = (c->next_buf + 1) % c->pool_size;

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

  dst.fd      = c->bufs[idx].fd;
  dst.format  = RK_FORMAT_YCbCr_420_SP;
  dst.mmuFlag = 1;
  dst.color_space_mode = 2; /* BT.709 output — HD standard for 1080p */
  rga_set_rect(&dst.rect, 0, 0, c->w, c->h,
               c->bufs[idx].stride, c->h, RK_FORMAT_YCbCr_420_SP);

  int ret = c_RkRgaBlit(&src, &dst, NULL);
  if (ret < 0) { fprintf(stderr, "rga: blit err %d\n", ret); return ret; }

  *dst_fd     = c->bufs[idx].fd;
  *dst_stride = c->bufs[idx].stride;
  return 0;
}
