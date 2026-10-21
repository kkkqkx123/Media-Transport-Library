/* SPDX-License-Identifier: BSD-3-Clause */

#include "tx_tai_ts.h"
#include <stdio.h>

bool tx_tai_mtl_prefill_is_relative(uint64_t mtl_prefill, uint64_t ptp_now) {
    return ptp_now > TX_TAI_NS_PER_S && mtl_prefill + TX_TAI_NS_PER_S < ptp_now;
}

bool tx_tai_needs_ptp_check(uint64_t epoch, uint64_t mtl_prefill) {
    /* Absolute wire TAI (circa 2020+) is above 1e18 ns. */
    if (mtl_prefill >= UINT64_C(1000000000000000))
        return false;
    /* ST22/ST30 with proper init_pacing_epoch use huge meta.epoch values. The stale
     * ST30 case keeps meta.epoch near 1 until the first frame re-syncs pacing. */
    return epoch <= 10;
}

void tx_tai_compute_anchor(uint64_t epoch, uint64_t mtl_prefill, uint64_t ptp_now,
                           uint64_t period, uint64_t* epoch_anchor,
                           uint64_t* stream_start_tai, bool* ptp_grid_anchor) {
    if (epoch_anchor)
        *epoch_anchor = epoch;
    if (stream_start_tai)
        *stream_start_tai = mtl_prefill;
    if (ptp_grid_anchor)
        *ptp_grid_anchor = false;

    if (!epoch_anchor || !stream_start_tai || !period)
        return;

    if (tx_tai_mtl_prefill_is_relative(mtl_prefill, ptp_now)) {
        *epoch_anchor     = (ptp_now + period / 2) / period + 1;
        *stream_start_tai = (*epoch_anchor) * period;
        if (ptp_grid_anchor)
            *ptp_grid_anchor = true;
    }
}

void tx_tai_ts_init(tx_tai_ts* t, mtl_handle st, uint64_t frame_period_ns,
                    bool enabled) {
    if (!t)
        return;
    t->st               = st;
    t->stream_start_tai = 0;
    t->epoch_anchor     = 0;
    t->frame_period_ns  = frame_period_ns;
    t->enabled          = enabled && st;
    t->anchored         = false;
}

void tx_tai_ts_reset(tx_tai_ts* t) {
    if (!t)
        return;
    t->stream_start_tai = 0;
    t->epoch_anchor     = 0;
    t->anchored         = false;
}

void tx_tai_ts_fill(tx_tai_ts* t, enum st10_timestamp_fmt* tfmt, uint64_t* timestamp,
                    uint64_t epoch, uint64_t mtl_prefill) {
    if (!t || !t->enabled || !tfmt || !timestamp)
        return;

    if (t->anchored && epoch < t->epoch_anchor) {
        fprintf(stderr,
                "tx_tai: epoch regression (%llu < %llu), re-anchor\n",
                (unsigned long long)epoch, (unsigned long long)t->epoch_anchor);
        tx_tai_ts_reset(t);
    }

    if (!t->anchored) {
        uint64_t ptp_now = 0;
        if (t->st && tx_tai_needs_ptp_check(epoch, mtl_prefill))
            ptp_now = mtl_ptp_read_time(t->st);
        bool     ptp_grid;

        tx_tai_compute_anchor(epoch, mtl_prefill, ptp_now, t->frame_period_ns,
                              &t->epoch_anchor, &t->stream_start_tai, &ptp_grid);
        t->anchored = true;
        printf("tx_tai: anchor epoch=%llu stream_start_tai=%llu period=%llu ns%s\n",
               (unsigned long long)t->epoch_anchor,
               (unsigned long long)t->stream_start_tai,
               (unsigned long long)t->frame_period_ns,
               ptp_grid ? " (ptp-grid)" : "");

        *tfmt      = ST10_TIMESTAMP_FMT_TAI;
        *timestamp = t->stream_start_tai;
        /* meta.epoch may still be 1 when ST30 pacing was not init'd to PTP yet */
        if (ptp_grid)
            return;
    }

    uint64_t rel = epoch - t->epoch_anchor;
    *tfmt          = ST10_TIMESTAMP_FMT_TAI;
    *timestamp     = t->stream_start_tai + rel * t->frame_period_ns;
}
