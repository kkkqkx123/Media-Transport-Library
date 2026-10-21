/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * V4L2 HDMI capture using DMABUF memory mode.
 * Captures NV12 frames and exports dma-buf fds for zero-copy to MPP.
 */

#ifndef _E2E_V4L2_CAPTURER_H_
#define _E2E_V4L2_CAPTURER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Opaque capturer handle */
struct v4l2_capturer;

/** Callback when a captured frame is ready.
 *  For multiplanar NV12: dma_fd = Y plane, dma_fd_uv = UV plane.
 *  For single-plane BGR3: dma_fd = frame data, dma_fd_uv = -1. */
typedef void (*v4l2_frame_cb)(void* user_data, int dma_fd, int dma_fd_uv,
                              uint32_t stride, uint32_t size,
                              uint64_t timestamp_ns, uint64_t frame_seq);

/**
 * Create V4L2 capturer.
 * @param device    e.g. "/dev/video0"
 * @param width     frame width
 * @param height    frame height
 * @param fps       target framerate
 * @param buf_count number of V4L2 buffers (4-6 recommended for 60fps)
 * @param cb        callback for each captured frame
 * @param user_data opaque user pointer passed to callback
 */
struct v4l2_capturer* v4l2_capturer_create(const char* device,
                                            int width, int height, float fps,
                                            int buf_count,
                                            v4l2_frame_cb cb, void* user_data);
void v4l2_capturer_free(struct v4l2_capturer* c);

/** Start capture loop (blocking — runs until stop is called) */
int v4l2_capturer_start(struct v4l2_capturer* c);

/** Signal capture loop to stop and return */
void v4l2_capturer_stop(struct v4l2_capturer* c);

/** Get the actual fourcc used */
uint32_t v4l2_capturer_fourcc(struct v4l2_capturer* c);
/** Get number of planes (1 for BGR3, 2 for NV12) */
int v4l2_capturer_num_planes(struct v4l2_capturer* c);

#endif /* _E2E_V4L2_CAPTURER_H_ */
