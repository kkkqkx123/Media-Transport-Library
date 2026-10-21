/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#ifndef _E2E_DRM_RENDERER_H_
#define _E2E_DRM_RENDERER_H_

#include "display_layout.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct drm_renderer;

typedef struct drm_renderer_params {
    const char* device;
    const char* connector;
    const char* mode_str;
    int         content_width;
    int         content_height;
    uint32_t    fourcc;
    int         buf_count;
    /** Phase A single-lane viewport (ignored when compositor_layout set). */
    const mp4_dist_display_viewport* viewport;
    /** Phase B multi-lane layout; compositor_layout->compositor selects path. */
    const display_layout* compositor_layout;
} drm_renderer_params;

/** One decoded NV12 layer for compositor commit. */
typedef struct drm_layer_frame {
    int      layer_idx;
    int      dma_fd;
    uint32_t hor_stride;
    uint32_t ver_stride;
    int      content_w;
    int      content_h;
} drm_layer_frame;

struct drm_renderer* drm_renderer_create(const drm_renderer_params* params);
void drm_renderer_free(struct drm_renderer* r);

/** Single-lane legacy (fullscreen / Phase A viewport). */
int drm_renderer_display(struct drm_renderer* r, int dma_fd, uint32_t hor_stride,
                         uint32_t ver_stride);

/** Phase B: commit up to layer_count planes in one vsync generation. */
int drm_renderer_display_layers(struct drm_renderer* r,
                                const drm_layer_frame* frames, int frame_count);

const display_layout* drm_renderer_layout(const struct drm_renderer* r);

#endif
