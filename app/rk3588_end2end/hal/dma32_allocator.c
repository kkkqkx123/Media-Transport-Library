/* SPDX-License-Identifier: BSD-3-Clause
 * DMA32 dma-heap allocator (see dma32_allocator.h).
 */

#include "dma32_allocator.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_TRACKED_FDS 128

struct dma32_alloc_entry {
  int    fd;
  size_t size;
};

struct dma32_allocator {
  int    fd;
  struct dma32_alloc_entry entries[MAX_TRACKED_FDS];
  int    count;
};

struct dma32_allocator* dma32_allocator_create(void) {
  struct dma32_allocator* a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->fd = -1;

  static const char* heap_paths[] = {
      "/dev/dma_heap/system-uncached-dma32",
      "/dev/dma_heap/system-dma32",
  };
  for (size_t i = 0; i < sizeof(heap_paths) / sizeof(heap_paths[0]); i++) {
    a->fd = open(heap_paths[i], O_RDWR | O_CLOEXEC);
    if (a->fd >= 0) {
      printf("dma32: using heap %s\n", heap_paths[i]);
      return a;
    }
    if (errno == EACCES) {
      fprintf(stderr, "dma32: %s exists but no permission (root?)\n",
              heap_paths[i]);
    }
  }
  fprintf(stderr, "dma32: no DMA32 dma-heap available on this kernel\n");
  free(a);
  return NULL;
}

void dma32_allocator_free(struct dma32_allocator* a) {
  if (!a) return;
  for (int i = 0; i < a->count; i++) close(a->entries[i].fd);
  if (a->fd >= 0) close(a->fd);
  free(a);
}

int dma32_allocator_alloc(struct dma32_allocator* a, size_t size, int* fd_out) {
  if (!a || a->fd < 0 || !fd_out) return -EINVAL;
  if (a->count >= MAX_TRACKED_FDS) return -ENOSPC;

  struct dma_heap_allocation_data data = {
      .len       = size,
      .fd        = 0,
      .fd_flags  = O_RDWR | O_CLOEXEC,
      .heap_flags = 0,
  };
  if (ioctl(a->fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0)
    return -errno;
  a->entries[a->count].fd   = (int)data.fd;
  a->entries[a->count].size = size;
  a->count++;
  *fd_out = (int)data.fd;
  return 0;
}

void dma32_allocator_free_buf(struct dma32_allocator* a, int fd) {
  if (!a) return;
  for (int i = 0; i < a->count; i++) {
    if (a->entries[i].fd == fd) {
      close(fd);
      a->entries[i] = a->entries[a->count - 1];
      a->count--;
      return;
    }
  }
}
