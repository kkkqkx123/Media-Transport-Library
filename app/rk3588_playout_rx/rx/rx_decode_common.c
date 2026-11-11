#include "rx_decode_common.h"

#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "h264_prep.h"
#include "../shared/mpp_decoder.h"

static uint64_t dec_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void rx_dec_lat_init(rx_dec_lat* lat) {
    if (!lat)
        return;
    pthread_mutex_init(&lat->lock, NULL);
    lat->count = 0;
}

void rx_dec_lat_destroy(rx_dec_lat* lat) {
    if (!lat)
        return;
    pthread_mutex_destroy(&lat->lock);
}

void rx_dec_lat_record(rx_dec_lat* lat, uint64_t feed_start_ns) {
    uint64_t now;
    uint32_t us;
    if (!lat || !feed_start_ns)
        return;
    now = dec_now_ns();
    if (now <= feed_start_ns)
        return;
    us = (uint32_t)((now - feed_start_ns) / 1000ULL);
    pthread_mutex_lock(&lat->lock);
    if (lat->count < RX_DEC_LAT_WIN) {
        lat->win[lat->count++] = us;
    } else {
        memmove(&lat->win[0], &lat->win[1],
                (size_t)(RX_DEC_LAT_WIN - 1) * sizeof(uint32_t));
        lat->win[RX_DEC_LAT_WIN - 1] = us;
    }
    pthread_mutex_unlock(&lat->lock);
}

void rx_dec_lat_percentiles(const rx_dec_lat* lat, uint32_t* p50_us,
                            uint32_t* p99_us) {
    uint32_t tmp[RX_DEC_LAT_WIN];
    int n = 0;
    if (p50_us)
        *p50_us = 0;
    if (p99_us)
        *p99_us = 0;
    if (!lat)
        return;
    pthread_mutex_lock((pthread_mutex_t*)&lat->lock);
    n = lat->count;
    if (n > 0)
        memcpy(tmp, lat->win, (size_t)n * sizeof(uint32_t));
    pthread_mutex_unlock((pthread_mutex_t*)&lat->lock);
    if (n == 0)
        return;
    for (int i = 1; i < n; i++) {
        uint32_t v = tmp[i];
        int j = i;
        while (j > 0 && tmp[j - 1] > v) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = v;
    }
    if (p50_us)
        *p50_us = tmp[(n * 50) / 100 >= n ? n - 1 : (n * 50) / 100];
    if (p99_us)
        *p99_us = tmp[(n * 99) / 100 >= n ? n - 1 : (n * 99) / 100];
}

int rx_decode_drain(void* dec_priv, uint64_t au_pts, uint64_t frame_period_ns,
                    uint64_t fallback_pts, uint64_t capture_tai_ns,
                    int idle_rounds, rx_dec_frame_cb on_frame, void* cb_arg) {
    int outputs = 0;
    int idle = 0;
    int au_frame_idx = 0;

    for (;;) {
        int out_fd;
        uint32_t out_h_stride, out_v_stride;

        if (mpp_dec_pull(dec_priv, &out_fd, &out_h_stride, &out_v_stride) == 1) {
            uint64_t frame_pts;
            idle = 0;
            if (au_pts)
                frame_pts = au_pts + (uint64_t)au_frame_idx * frame_period_ns;
            else
                frame_pts = fallback_pts;
            if (on_frame)
                on_frame(out_fd, out_h_stride, out_v_stride, frame_pts,
                         capture_tai_ns, cb_arg);
            au_frame_idx++;
            outputs++;
            continue;
        }
        if (idle >= idle_rounds)
            break;
        idle++;
        for (int w = 0; w < 50; w++)
            sched_yield();
    }
    return outputs;
}

int rx_decode_feed_and_drain(void* dec_priv, h264_prep* prep, uint8_t* data,
                             size_t size, uint64_t au_pts,
                             uint64_t frame_period_ns, uint64_t fallback_pts,
                             uint64_t capture_tai_ns, int idle_rounds,
                             rx_dec_frame_cb on_frame, void* cb_arg,
                             int* prep_fail, int* put_fail) {
    const uint8_t* feed_data = data;
    size_t feed_size = size;
    int feed_owned = 0;
    int idle;

    if (prep_fail)
        *prep_fail = 0;
    if (put_fail)
        *put_fail = 0;

    if (prep) {
        if (h264_prep_for_mpp(prep, data, size, &feed_data, &feed_size,
                              &feed_owned) < 0) {
            if (prep_fail)
                *prep_fail = 1;
            return -1;
        }
    }

    if (mpp_dec_uses_whole_au(dec_priv)) {
        if (mpp_dec_put_whole(dec_priv, (uint8_t*)feed_data, feed_size) < 0) {
            if (put_fail)
                *put_fail = 1;
            if (feed_owned)
                free((void*)feed_data);
            return -2;
        }
    } else if (mpp_dec_put(dec_priv, (uint8_t*)feed_data, feed_size) < 0) {
        if (put_fail)
            *put_fail = 1;
        if (feed_owned)
            free((void*)feed_data);
        return -2;
    }

    idle = mpp_dec_uses_aggressive_pull(dec_priv) ? MPP_DEC_AGGRESSIVE_IDLE_ROUNDS
                                                  : idle_rounds;
    {
        int outputs = rx_decode_drain(dec_priv, au_pts, frame_period_ns,
                                      fallback_pts, capture_tai_ns, idle,
                                      on_frame, cb_arg);
        if (feed_owned)
            free((void*)feed_data);
        return outputs;
    }
}
