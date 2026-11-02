#ifndef _E2E_DMA32_ALLOCATOR_H_
#define _E2E_DMA32_ALLOCATOR_H_

#include <stddef.h>
#include <stdint.h>

struct dma32_allocator;

/**
 * Open a DMA32 dma-heap. Prefers "system-uncached-dma32" (fast CPU access
 * paths are unnecessary for pure hw pipelines); falls back to the cached
 * variant with a warning.
 * @return allocator handle, NULL if no DMA32 heap exists on this kernel
 */
struct dma32_allocator* dma32_allocator_create(void);
void dma32_allocator_free(struct dma32_allocator* a);

/**
 * Allocate a buffer from the DMA32 heap and export it as dmabuf.
 * @param size      bytes to allocate
 * @param fd_out    [out] exported dmabuf fd (owned by allocator until free)
 * @return 0 on success, -errno on failure
 */
int dma32_allocator_alloc(struct dma32_allocator* a, size_t size, int* fd_out);

/** Release a buffer previously returned by dma32_allocator_alloc. */
void dma32_allocator_free_buf(struct dma32_allocator* a, int fd);

#endif /* _E2E_DMA32_ALLOCATOR_H_ */
