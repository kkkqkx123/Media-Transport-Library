/* SPDX-License-Identifier: BSD-3-Clause */

#include "media_clk.h"
#include <time.h>

#define NS_PER_S 1000000000ULL

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_S + (uint64_t)ts.tv_nsec;
}

void media_clk_anchor_init(media_clk_anchor* a) {
    if (!a) return;
    a->anchored       = false;
    a->base_unwrapped = 0;
    a->base_pts_ns    = 0;
    a->last_raw       = 0;
    a->have_last      = false;
    a->last_unwrapped = 0;
    a->stat_frames    = 0;
    a->time_fn        = NULL;
    a->time_priv      = NULL;
    a->use_ptp_anchor = false;
}

void media_clk_set_time_source(media_clk_anchor* a, media_clk_time_fn fn,
                               void* priv) {
    if (!a) return;
    a->time_fn = fn;
    a->time_priv = priv;
}

uint64_t media_clk_unwrap_test(media_clk_anchor* a, uint32_t raw) {
    if (!a) return raw;

    if (!a->have_last) {
        a->have_last      = true;
        a->last_raw       = raw;
        a->last_unwrapped = raw;
        return raw;
    }

    int32_t delta = (int32_t)(raw - a->last_raw);
    a->last_unwrapped = (uint64_t)((int64_t)a->last_unwrapped + delta);
    a->last_raw       = raw;
    return a->last_unwrapped;
}

static uint64_t delta_clk_to_ns(uint64_t delta_ticks) {
    return (delta_ticks * NS_PER_S) / (uint64_t)ST10_VIDEO_SAMPLING_RATE_90K;
}

uint64_t media_clk_frame_pts(media_clk_anchor* a, enum st10_timestamp_fmt tfmt,
                             uint64_t timestamp) {
    if (!a) return 0;

    a->stat_frames++;

    if (tfmt == ST10_TIMESTAMP_FMT_TAI && timestamp)
        return timestamp;

    uint64_t unwrapped = media_clk_unwrap_test(a, (uint32_t)timestamp);

    if (!a->anchored) {
        a->anchored       = true;
        a->base_unwrapped = unwrapped;
        if (a->time_fn) {
            a->base_pts_ns    = a->time_fn(a->time_priv);
            a->use_ptp_anchor = (a->base_pts_ns != 0);
        }
        if (!a->use_ptp_anchor)
            a->base_pts_ns = monotonic_ns();
        return a->base_pts_ns;
    }

    uint64_t delta = unwrapped - a->base_unwrapped;
    return a->base_pts_ns + delta_clk_to_ns(delta);
}
