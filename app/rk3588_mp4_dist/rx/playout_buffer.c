/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Playout buffer implementation — thread-safe decoded frame queue.
 */

#include "playout_buffer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

void playout_buffer_init(playout_buffer* pb) {
    memset(pb, 0, sizeof(*pb));
    pthread_mutex_init(&pb->lock, NULL);
    pthread_cond_init(&pb->data_ready, NULL);
}

void playout_buffer_push(playout_buffer* pb, int dma_fd,
                         uint32_t hor_stride, uint32_t ver_stride,
                         uint64_t pts_ns, uint64_t capture_tai_ns) {
    pthread_mutex_lock(&pb->lock);

    if (pb->count >= PLAYOUT_QUEUE_DEPTH) {
        /* Overrun — drop oldest frame */
        pb->stat_overrun_drop++;
        pb->read_idx = (pb->read_idx + 1) % PLAYOUT_QUEUE_DEPTH;
        pb->count--;
    }

    struct decoded_frame* df = &pb->frames[pb->write_idx];
    df->dma_fd     = dma_fd;
    df->hor_stride = hor_stride;
    df->ver_stride = ver_stride;
    df->pts             = pts_ns;
    df->capture_tai_ns  = capture_tai_ns;
    df->ready           = 1;

    pb->write_idx = (pb->write_idx + 1) % PLAYOUT_QUEUE_DEPTH;
    pb->count++;

    pthread_cond_signal(&pb->data_ready);
    pthread_mutex_unlock(&pb->lock);
}

int playout_buffer_pop_at(playout_buffer* pb, uint64_t target_pts_ns,
                          int timeout_ms,
                          int* dma_fd_out, uint32_t* hor_stride_out,
                          uint32_t* ver_stride_out, uint64_t* capture_tai_out) {
    pthread_mutex_lock(&pb->lock);

    /* wait for a frame */
    if (pb->count == 0) {
        if (timeout_ms <= 0) {
            pthread_mutex_unlock(&pb->lock);
            return 0;  /* no data, non-blocking */
        }

        struct timeval now;
        struct timespec wait_until;
        gettimeofday(&now, NULL);
        wait_until.tv_sec  = now.tv_sec + timeout_ms / 1000;
        wait_until.tv_nsec = (now.tv_usec * 1000) + ((timeout_ms % 1000) * 1000000L);
        if (wait_until.tv_nsec >= 1000000000L) {
            wait_until.tv_sec++;
            wait_until.tv_nsec -= 1000000000L;
        }

        int ret = pthread_cond_timedwait(&pb->data_ready, &pb->lock, &wait_until);
        if (ret == ETIMEDOUT || pb->count == 0) {
            pthread_mutex_unlock(&pb->lock);
            return 0;  /* timeout, no frame */
        }
    }

    struct decoded_frame* df = &pb->frames[pb->read_idx];

    if (df->pts <= target_pts_ns) {
        /* Frame is due — render it */
        *dma_fd_out     = df->dma_fd;
        *hor_stride_out = df->hor_stride;
        *ver_stride_out = df->ver_stride;
        if (capture_tai_out)
            *capture_tai_out = df->capture_tai_ns;
        df->ready = 0;

        pb->last_dma_fd     = df->dma_fd;
        pb->last_h_stride   = df->hor_stride;
        pb->last_v_stride   = df->ver_stride;
        pb->last_capture_tai_ns = df->capture_tai_ns;
        pb->have_last       = true;

        pb->read_idx = (pb->read_idx + 1) % PLAYOUT_QUEUE_DEPTH;
        pb->count--;
        pthread_mutex_unlock(&pb->lock);
        return 1;  /* rendered */
    } else {
        /* Frame is not due yet — repeat last frame if available */
        if (pb->have_last) {
            *dma_fd_out     = pb->last_dma_fd;
            *hor_stride_out = pb->last_h_stride;
            *ver_stride_out = pb->last_v_stride;
            if (capture_tai_out)
                *capture_tai_out = pb->last_capture_tai_ns;
            pthread_mutex_unlock(&pb->lock);
            return -1;  /* repeat */
        }
        pthread_mutex_unlock(&pb->lock);
        return 0;  /* nothing to show yet */
    }
}

int playout_buffer_pop_newest(playout_buffer* pb, uint64_t target_pts_ns,
                              int* dma_fd_out, uint32_t* hor_stride_out,
                              uint32_t* ver_stride_out, uint64_t* capture_tai_out) {
    uint64_t last_capture = 0;
    int got = 0;
    int r;
    do {
        uint64_t cap = 0;
        r = playout_buffer_pop_at(pb, target_pts_ns, 0, dma_fd_out, hor_stride_out,
                                  ver_stride_out, &cap);
        if (r == 1) {
            got = 1;
            if (cap)
                last_capture = cap;
        }
    } while (r == 1);

    if (capture_tai_out)
        *capture_tai_out = last_capture;

    if (got)
        return 1;
    return r;
}

bool playout_buffer_has_frames(const playout_buffer* pb) {
    if (!pb) return false;
    playout_buffer* mut = (playout_buffer*)pb;
    pthread_mutex_lock(&mut->lock);
    bool has = mut->count > 0;
    pthread_mutex_unlock(&mut->lock);
    return has;
}

uint64_t playout_buffer_peek_pts(const playout_buffer* pb) {
    if (!pb) return 0;
    playout_buffer* mut = (playout_buffer*)pb;
    pthread_mutex_lock(&mut->lock);
    uint64_t pts = 0;
    if (mut->count > 0)
        pts = mut->frames[mut->read_idx].pts;
    pthread_mutex_unlock(&mut->lock);
    return pts;
}

int playout_buffer_wait_data(playout_buffer* pb, int timeout_ms) {
    if (!pb) return 0;

    pthread_mutex_lock(&pb->lock);
    if (pb->count > 0) {
        pthread_mutex_unlock(&pb->lock);
        return 1;
    }
    if (timeout_ms <= 0) {
        pthread_mutex_unlock(&pb->lock);
        return 0;
    }

    struct timeval now;
    struct timespec wait_until;
    gettimeofday(&now, NULL);
    wait_until.tv_sec  = now.tv_sec + timeout_ms / 1000;
    wait_until.tv_nsec = (now.tv_usec * 1000) + ((timeout_ms % 1000) * 1000000L);
    if (wait_until.tv_nsec >= 1000000000L) {
        wait_until.tv_sec++;
        wait_until.tv_nsec -= 1000000000L;
    }

    int ret = pthread_cond_timedwait(&pb->data_ready, &pb->lock, &wait_until);
    int has = pb->count > 0;
    pthread_mutex_unlock(&pb->lock);
    return (ret == 0 || has) && has;
}

void playout_buffer_wake_all(playout_buffer* pb) {
    pthread_mutex_lock(&pb->lock);
    pthread_cond_broadcast(&pb->data_ready);
    pthread_mutex_unlock(&pb->lock);
}

uint64_t playout_buffer_overrun_drops(const playout_buffer* pb) {
    if (!pb) return 0;
    playout_buffer* mut = (playout_buffer*)pb;
    pthread_mutex_lock(&mut->lock);
    uint64_t n = mut->stat_overrun_drop;
    pthread_mutex_unlock(&mut->lock);
    return n;
}

void playout_buffer_destroy(playout_buffer* pb) {
    pthread_mutex_destroy(&pb->lock);
    pthread_cond_destroy(&pb->data_ready);
}
