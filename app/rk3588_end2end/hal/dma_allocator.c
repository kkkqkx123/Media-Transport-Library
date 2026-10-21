/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "dma_allocator.h"
#include <drm.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct dma_allocator {
  int drm_fd;
};

struct dma_allocator* dma_allocator_create(const char* card_path) {
  struct dma_allocator* a = calloc(1, sizeof(*a));
  if (!a) return NULL;

  a->drm_fd = open(card_path, O_RDWR | O_CLOEXEC);
  if (a->drm_fd < 0) {
    perror("dma_allocator: open drm device");
    free(a);
    return NULL;
  }
  return a;
}

void dma_allocator_free(struct dma_allocator* a) {
  if (!a) return;
  if (a->drm_fd >= 0) close(a->drm_fd);
  free(a);
}

int dma_allocator_alloc(struct dma_allocator* a, int width, int height,
                        uint32_t fourcc, int* fd_out, uint32_t* stride_out,
                        size_t* size_out, uint32_t* handle_out) {
  if (!a || !fd_out || !stride_out || !size_out || !handle_out) return -EINVAL;
  (void)fourcc; /* reserved for future multi-format support */

  /* For NV12, the luma plane has one byte per pixel,
   * chroma plane has 0.5 bytes per pixel (interleaved UV).
   * Use DRM dumb buffer with bpp=8 (luma plane size), total = 1.5 * w * h */
  size_t luma_size = width * height;
  size_t total_size = luma_size + (width * height / 2);

  struct drm_mode_create_dumb create = {0};
  create.width  = width;
  create.height = height;
  create.bpp    = 8; /* bytes per pixel for luma plane */

  if (drmIoctl(a->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
    perror("dma_allocator: DRM_IOCTL_MODE_CREATE_DUMB");
    return -errno;
  }

  /* Export as dma-buf fd for cross-device sharing */
  int prime_fd = -1;
  if (drmPrimeHandleToFD(a->drm_fd, create.handle, DRM_CLOEXEC, &prime_fd) < 0) {
    perror("dma_allocator: drmPrimeHandleToFD");
    drmIoctl(a->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &create);
    return -errno;
  }

  *fd_out     = prime_fd;
  *stride_out = create.pitch;
  *size_out   = total_size;
  *handle_out = create.handle;
  return 0;
}

void dma_allocator_free_buf(struct dma_allocator* a, uint32_t handle) {
  if (!a) return;
  struct drm_mode_destroy_dumb destroy = {.handle = handle};
  drmIoctl(a->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}
