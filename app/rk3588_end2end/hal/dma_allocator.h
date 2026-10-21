/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * dma-buf memory allocator using DRM dumb buffers.
 * Provides dma-buf fd export for zero-copy sharing between V4L2, MPP, DRM.
 */

#ifndef _E2E_DMA_ALLOCATOR_H_
#define _E2E_DMA_ALLOCATOR_H_

#include <stddef.h>
#include <stdint.h>

struct dma_allocator;

/**
 * Create a dma-buf allocator using DRM dumb buffers.
 * @param card_path  DRM device path, e.g. "/dev/dri/card0"
 * @return allocator handle, NULL on failure
 */
struct dma_allocator* dma_allocator_create(const char* card_path);
void dma_allocator_free(struct dma_allocator* a);

/**
 * Allocate a DRM dumb buffer suitable for NV12 frames.
 * @param a         allocator
 * @param width     frame width in pixels
 * @param height    frame height in pixels
 * @param fourcc    pixel format (DRM_FORMAT_NV12)
 * @param fd_out    [out] exported dma-buf fd
 * @param stride_out[out] row stride in bytes
 * @param size_out  [out] total buffer size
 * @param handle_out[out] GEM handle (for internal use)
 * @return 0 on success, -errno on failure
 */
int dma_allocator_alloc(struct dma_allocator* a, int width, int height,
                        uint32_t fourcc, int* fd_out, uint32_t* stride_out,
                        size_t* size_out, uint32_t* handle_out);

/**
 * Free a previously allocated buffer.
 */
void dma_allocator_free_buf(struct dma_allocator* a, uint32_t handle);

#endif /* _E2E_DMA_ALLOCATOR_H_ */
