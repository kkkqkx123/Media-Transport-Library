#ifndef _LIVE_INGEST_DRM_RENDERER_H_
#define _LIVE_INGEST_DRM_RENDERER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct drm_renderer;

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

#endif
