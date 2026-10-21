/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Playout buffer — fixed-size queue of decoded frames with PTS ordering.
 * Decode thread pushes, render thread pops at scheduled PTS.
 */

#ifndef _PLAYOUT_BUFFER_H_
#define _PLAYOUT_BUFFER_H_

#include "../shared/pipeline_common.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define PLAYOUT_QUEUE_DEPTH 16

/** Thread-safe playout queue of decoded frames. */
typedef struct playout_buffer {
    struct decoded_frame frames[PLAYOUT_QUEUE_DEPTH];
    int                  write_idx;
    int                  read_idx;
    int                  count;           /* number of ready frames */
    uint64_t             last_dma_fd;     /* last rendered dma_fd (for repeat) */
    uint32_t             last_h_stride, last_v_stride;
    bool                 have_last;       /* valid last frame for repeat */
    uint64_t             stat_overrun_drop;
    uint64_t             last_capture_tai_ns;
    pthread_mutex_t      lock;
    pthread_cond_t       data_ready;
} playout_buffer;

/** Initialize the buffer. */
void playout_buffer_init(playout_buffer* pb);

/**
 * Push a decoded frame into the buffer.
 * If full, drops the oldest frame (overrun protection).
 */
void playout_buffer_push(playout_buffer* pb, int dma_fd,
                         uint32_t hor_stride, uint32_t ver_stride,
                         uint64_t pts_ns, uint64_t capture_tai_ns);

/**
 * Pop the next frame at or before target_pts_ns.
 * If no frame is ready, waits up to timeout_ms.
 * Returns: 1 = frame popped, 0 = timeout (no frame), -1 = last frame repeated
 */
int playout_buffer_pop_at(playout_buffer* pb, uint64_t target_pts_ns,
                          int timeout_ms,
                          int* dma_fd_out, uint32_t* hor_stride_out,
                          uint32_t* ver_stride_out, uint64_t* capture_tai_out);

/**
 * Drain all frames at or before target_pts_ns; return the newest.
 * Returns: 1 = new frame, -1 = repeat last, 0 = none ready.
 */
int playout_buffer_pop_newest(playout_buffer* pb, uint64_t target_pts_ns,
                              int* dma_fd_out, uint32_t* hor_stride_out,
                              uint32_t* ver_stride_out, uint64_t* capture_tai_out);

/** Returns true if the queue has at least one decoded frame ready. */
bool playout_buffer_has_frames(const playout_buffer* pb);

/** Peek at the PTS of the next queued frame (0 if empty). */
uint64_t playout_buffer_peek_pts(const playout_buffer* pb);

/**
 * Block until the queue has at least one frame or timeout_ms elapses.
 * Returns 1 if a frame is available, 0 on timeout / shutdown.
 */
int playout_buffer_wait_data(playout_buffer* pb, int timeout_ms);

/** Wake all waiters (for shutdown). */
void playout_buffer_wake_all(playout_buffer* pb);

/** Overrun drops (oldest frame evicted when queue full). */
uint64_t playout_buffer_overrun_drops(const playout_buffer* pb);

/** Destroy the buffer. */
void playout_buffer_destroy(playout_buffer* pb);

#endif /* _PLAYOUT_BUFFER_H_ */
