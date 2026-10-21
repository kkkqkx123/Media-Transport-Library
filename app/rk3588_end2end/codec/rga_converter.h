/* SPDX-License-Identifier: BSD-3-Clause
 *
 * RGA hardware BGR3→NV12 converter.
 * Zero-copy: imports src dma-buf, writes to pre-allocated dst dma-buf.
 */

#ifndef _E2E_RGA_CONVERTER_H_
#define _E2E_RGA_CONVERTER_H_

#include <stdint.h>

struct rga_converter;

/** Create RGA converter. Allocates internal NV12 output buffer pool. */
struct rga_converter* rga_converter_create(int width, int height, int pool_size);
void rga_converter_free(struct rga_converter* c);

/**
 * Convert BGR3 dma-buf → NV12 dma-buf (hardware, ~1ms).
 * @param src_fd    BGR3 dma-buf fd
 * @param src_stride BGR3 row stride (width*3)
 * @param dst_fd    [out] NV12 dma-buf fd (from internal pool)
 * @param dst_stride[out] NV12 row stride (=width)
 * @return 0 on success
 */
int rga_convert(struct rga_converter* c, int src_fd, uint32_t src_stride,
                int* dst_fd, uint32_t* dst_stride);

#endif
