#ifndef _LIVE_INGEST_DRM_RENDERER_H_
#define _LIVE_INGEST_DRM_RENDERER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct drm_renderer;

/** Display-side observability (decode thread writes, supervisor polls). */
struct drm_renderer_stats {
  uint64_t displays;        /* successful displays */
  uint32_t display_fps;     /* windowed rate, computed at stats poll */
  uint64_t display_ema_us;  /* display call cost EMA */
  uint64_t flip_wait_ema_us;/* flip wait cost EMA */
  uint64_t flip_timeouts;   /* wait_flip_done timeouts */
  int      flip_pending_peak;
  uint64_t addfb2_fails;    /* ADDFB2 stage failures */
  uint64_t flip_fails;      /* PageFlip/SetCrtc stage failures */
  uint64_t bad_args;        /* invalid fd / missing renderer */
  int      mode_fps;        /* negotiated mode vrefresh, 0 = unknown */
};

struct drm_renderer* drm_renderer_create(const char* device,
                                          const char* connector,
                                          const char* mode_str,
                                          int width, int height,
                                          uint32_t fourcc, int buf_count);
void drm_renderer_free(struct drm_renderer* r);
/* hor_stride: NV12 row pitch in bytes.
 * ver_stride: NV12 buffer vertical stride (MPP aligns to 16; RGA uses 0 = height). */
int  drm_renderer_display(struct drm_renderer* r, int dma_fd,
                          uint32_t hor_stride, uint32_t ver_stride);

/** One-shot display snapshot (NULL-safe, never blocks). */
void drm_renderer_stats(struct drm_renderer* r, struct drm_renderer_stats* out);

#endif
