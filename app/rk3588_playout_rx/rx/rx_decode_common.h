#ifndef _RX_DECODE_COMMON_H_
#define _RX_DECODE_COMMON_H_

/* Shared decode-lane core for rx_pipeline (single stream) and rx_compositor
 * (one instance per lane). Covers the identical part of both paths: AU feed
 * (h264_prep + whole-AU or chunked put) plus the pull drain loop, and the
 * windowed feed->first-frame latency. Thread loops, playout push and stats
 * stay with the callers via the on_frame callback. */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "h264_prep.h"

/* Sliding decode-latency window (us). Single writer (owning decode thread);
 * readers take the lock for percentiles. */
#define RX_DEC_LAT_WIN 300

typedef struct rx_dec_lat {
    pthread_mutex_t lock;
    uint32_t        win[RX_DEC_LAT_WIN];
    int             count;
} rx_dec_lat;

void rx_dec_lat_init(rx_dec_lat* lat);
void rx_dec_lat_destroy(rx_dec_lat* lat);
/* Record one sample (now - feed_start_ns in us); ignored when start is 0. */
void rx_dec_lat_record(rx_dec_lat* lat, uint64_t feed_start_ns);
/* Insertion-sorted percentiles over the current window (0/0 when empty). */
void rx_dec_lat_percentiles(const rx_dec_lat* lat, uint32_t* p50_us,
                            uint32_t* p99_us);

/** Per-frame sink: drain calls back once per displayable frame. */
typedef void (*rx_dec_frame_cb)(int dma_fd, uint32_t hor_stride,
                                uint32_t ver_stride, uint64_t frame_pts,
                                uint64_t capture_tai_ns, void* arg);

/* Drain displayable frames. Each MPP output gets au_pts + n*frame_period_ns,
 * or fallback_pts when au_pts is 0. Returns the frame count. */
int rx_decode_drain(void* dec_priv, uint64_t au_pts, uint64_t frame_period_ns,
                    uint64_t fallback_pts, uint64_t capture_tai_ns,
                    int idle_rounds, rx_dec_frame_cb on_frame, void* cb_arg);

/* Feed one AU (h264_prep + whole-AU or chunked put) then drain.
 * prep_fail/put_fail count 0/1 each (may be NULL). Returns the frame count,
 * -1 on prep failure, -2 on put failure. */
int rx_decode_feed_and_drain(void* dec_priv, h264_prep* prep, uint8_t* data,
                             size_t size, uint64_t au_pts,
                             uint64_t frame_period_ns, uint64_t fallback_pts,
                             uint64_t capture_tai_ns, int idle_rounds,
                             rx_dec_frame_cb on_frame, void* cb_arg,
                             int* prep_fail, int* put_fail);

#endif /* _RX_DECODE_COMMON_H_ */
