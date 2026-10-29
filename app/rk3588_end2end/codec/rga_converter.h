/* SPDX-License-Identifier: BSD-3-Clause
 *
 * RGA hardware BGR3→NV12 converter.
 * Zero-copy: imports src dma-buf, writes to pre-allocated dst dma-buf.
 *
 * Pool semantics: output buffers are ACQUIRED per frame and must be
 * RELEASED by the consumer once the encoder has finished with the fd
 * (MPP import is async — a blind ring rotation could hand the same buffer
 * to a new blit while MPP still reads the old one). Pool exhaustion drops
 * the new frame with a counter instead of silently reusing a busy buffer.
 *
 * Fallback chain: fd→fd blit failure alone drops the frame; 3 consecutive
 * failures on the fd path degrade it (sticky) to an mmap + CPU copy
 * conversion. DMA32 heap allocation guarantees RGA 32-bit MMU mapping.
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
 * @param dst_idx   [out] pool slot handle for rga_converter_release()
 * @return 0 on success; -EAGAIN if the pool is exhausted (caller drops the
 *         frame — no buffer was consumed)
 */
int rga_convert(struct rga_converter* c, int src_fd, uint32_t src_stride,
                int* dst_fd, uint32_t* dst_stride, int* dst_idx);

/** Return a converted buffer to the pool after the encoder is done with it. */
void rga_converter_release(struct rga_converter* c, int pool_idx);

/** Cumulative counters for stats reporting. */
struct rga_stats {
  uint64_t calls;        /* successful conversions */
  uint64_t fails;        /* blit failures (frame dropped) */
  uint64_t pool_exhausted; /* frames dropped because the pool was full */
  uint64_t cpu_fallbacks;  /* frames converted on the degraded CPU path */
};

void rga_converter_stats(struct rga_converter* c, struct rga_stats* out);

#endif
