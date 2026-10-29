/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * V4L2 HDMI capture using Multiplanar API (required by RK3588 rk_hdmirx).
 * Captures NV12/BGR3 frames and exports dma-buf fds for zero-copy to MPP.
 *
 * Buffer lifetime: frames are NOT re-queued when the callback returns — the
 * consumer must call v4l2_capturer_release() with the frame's
 * (buf_index, generation) once it has finished with the dma-buf fd. Release
 * is idempotent: a stale or repeated release for an already-requeued or
 * recycled buffer is silently ignored (generation + claim-bit CAS).
 */

#ifndef _E2E_V4L2_CAPTURER_H_
#define _E2E_V4L2_CAPTURER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Opaque capturer handle */
struct v4l2_capturer;

/** Captured frame descriptor passed to the callback. */
struct v4l2_frame {
  int      dma_fd;       /* plane 0 dma-buf fd (BGR3 data / NV12 Y) */
  int      dma_fd_uv;    /* plane 1 dma-buf fd (NV12 UV), -1 if single-plane */
  uint32_t stride;       /* plane 0 row stride in bytes */
  uint32_t size;         /* total bytes used across planes */
  uint64_t timestamp_ns; /* kernel frame timestamp (CLOCK_MONOTONIC);
                            falls back to dequeue time if driver stamps 0 */
  uint64_t frame_seq;
  /* release handle — do not modify */
  uint32_t buf_index;
  uint64_t generation;
};

/** Callback when a captured frame is ready. Called on the capture thread;
 *  must not block and must not call v4l2_capturer_stop()/free(). */
typedef void (*v4l2_frame_cb)(void* user_data, const struct v4l2_frame* f);

/** Capture-side error (DQBUF/poll failure, non-transient). Called on the
 *  capture thread; must not block. The capture loop has stopped when this
 *  fires — the owner should schedule a rebuild via v4l2_capturer_start() on
 *  a fresh capturer, with anti-storm counting on its side. */
typedef void (*v4l2_error_cb)(void* user_data);

/** Source-change event (V4L2_EVENT_SOURCE_CHANGE): resolution/signal changed.
 *  Event payload is not parsed; the owner should rebuild the pipeline. */
typedef void (*v4l2_signal_change_cb)(void* user_data);

/**
 * Return a captured buffer to the driver. Call exactly once per frame after
 * the last consumer of dma_fd is finished. Idempotent.
 */
void v4l2_capturer_release(struct v4l2_capturer* c, uint32_t buf_index,
                           uint64_t generation);

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

/** Optional supervision hooks (must be set before v4l2_capturer_start). */
void v4l2_capturer_set_error_cb(struct v4l2_capturer* c,
                                v4l2_error_cb cb, void* user_data);
void v4l2_capturer_set_signal_change_cb(struct v4l2_capturer* c,
                                        v4l2_signal_change_cb cb,
                                        void* user_data);

/** Start capture loop (blocking — runs until stop is called) */
int v4l2_capturer_start(struct v4l2_capturer* c);

/** Signal capture loop to stop and return */
void v4l2_capturer_stop(struct v4l2_capturer* c);

/** Get the actual fourcc used */
uint32_t v4l2_capturer_fourcc(struct v4l2_capturer* c);
/** Get number of planes (1 for BGR3, 2 for NV12) */
int v4l2_capturer_num_planes(struct v4l2_capturer* c);

/** Buffers dequeued but not yet released (backpressure indicator). */
int v4l2_capturer_outstanding(struct v4l2_capturer* c);

#endif /* _E2E_V4L2_CAPTURER_H_ */
