#ifndef _LIVE_INGEST_RGA_CONVERTER_H_
#define _LIVE_INGEST_RGA_CONVERTER_H_

#include <stdint.h>

struct rga_converter;

/** Create RGA converter. Allocates internal NV12 output buffer pool. */
struct rga_converter* rga_converter_create(int width, int height, int pool_size);
void rga_converter_free(struct rga_converter* c);

/**
 * Convert capture dma-buf → NV12 dma-buf (hardware, ~1ms).
 * @param src_fd     source dma-buf fd (single-plane contiguous)
 * @param src_stride source row stride in bytes
 * @param src_fourcc V4L2 fourcc of the source (BGR24/UYVY/YUYV/NV12)
 * @param dst_fd    [out] NV12 dma-buf fd (from internal pool)
 * @param dst_stride[out] NV12 row stride
 * @param dst_idx   [out] pool slot handle for rga_converter_release()
 * @return 0 on success; -EAGAIN if the pool is exhausted (caller drops the
 *         frame — no buffer was consumed)
 */
int rga_convert(struct rga_converter* c, int src_fd, uint32_t src_stride,
                uint32_t src_fourcc,
                int* dst_fd, uint32_t* dst_stride, int* dst_idx);

/** Return a converted buffer to the pool after the encoder is done with it. */
void rga_converter_release(struct rga_converter* c, int pool_idx);

/** Cumulative counters for stats reporting. */
struct rga_stats {
  uint64_t calls;          /* successful conversions (fd hardware path) */
  uint64_t fails;          /* hardware blit failures (frame dropped) */
  uint64_t pool_exhausted; /* frames dropped because the pool was full */
  uint64_t cpu_fallbacks;  /* frames converted on the degraded CPU path */
  uint64_t cpu_fails;      /* CPU fallback conversion failures */
  uint64_t time_ema_us;    /* hardware blit cost EMA in microseconds */
  uint32_t pool_in_use;    /* buffers currently checked out */
  uint32_t pool_peak;      /* session peak of checked-out buffers */
  uint32_t pool_size;      /* total pool depth */
  int      degraded;       /* non-zero once the fd path degraded to CPU */
};

void rga_converter_stats(struct rga_converter* c, struct rga_stats* out);

/** Buffers currently checked out of the pool (pressure indicator). */
uint32_t rga_converter_in_use(struct rga_converter* c);
/** Session peak of checked-out buffers. */
uint32_t rga_converter_peak(struct rga_converter* c);
/** Non-zero when the fd path has degraded to the CPU fallback. */
int rga_converter_degraded(struct rga_converter* c);
/** Startup RGA driver version string (never NULL, "unknown" when unreadable). */
const char* rga_converter_version(struct rga_converter* c);

#endif
