/* SPDX-License-Identifier: BSD-3-Clause
 *
 * P7: map MTL absolute session epoch → stream-relative TAI ns for ST22/ST30
 * USER_TIMESTAMP pacing.
 */

#ifndef _TX_TAI_TS_H_
#define _TX_TAI_TS_H_

#include <mtl_api.h>
#include <stdbool.h>
#include <stdint.h>
#include <st_api.h>

#define TX_TAI_NS_PER_S 1000000000ULL

typedef struct tx_tai_ts {
    mtl_handle st;
    uint64_t   stream_start_tai; /* TAI at epoch_anchor (MTL grid) */
    uint64_t   epoch_anchor;     /* absolute MTL epoch at stream start */
    uint64_t   frame_period_ns;
    bool       enabled;
    bool       anchored;
} tx_tai_ts;

void tx_tai_ts_init(tx_tai_ts* t, mtl_handle st, uint64_t frame_period_ns,
                    bool enabled);

/** Clear anchor; next fill re-establishes stream timeline (playlist loop/seek). */
void tx_tai_ts_reset(tx_tai_ts* t);

/**
 * True when MTL prefill is epoch×period (ms-scale) rather than absolute TAI.
 * Happens when ST30 is created after scheduler tasklet start (cur_epochs still 0).
 */
bool tx_tai_mtl_prefill_is_relative(uint64_t mtl_prefill, uint64_t ptp_now);

/** True when meta may reflect unstale ST30 pacing (cur_epochs still 0). */
bool tx_tai_needs_ptp_check(uint64_t epoch, uint64_t mtl_prefill);

/**
 * Compute stream anchor from MTL meta and optional PTP now.
 * @param ptp_grid_anchor set when anchor used PTP grid instead of meta epoch
 */
void tx_tai_compute_anchor(uint64_t epoch, uint64_t mtl_prefill, uint64_t ptp_now,
                           uint64_t period, uint64_t* epoch_anchor,
                           uint64_t* stream_start_tai, bool* ptp_grid_anchor);

/**
 * Fill tfmt/timestamp from MTL absolute epoch.
 * @param epoch       meta.epoch from MTL (absolute pacing slot)
 * @param mtl_prefill meta.timestamp before overwrite (MTL default TAI on grid)
 */
void tx_tai_ts_fill(tx_tai_ts* t, enum st10_timestamp_fmt* tfmt, uint64_t* timestamp,
                    uint64_t epoch, uint64_t mtl_prefill);

#endif /* _TX_TAI_TS_H_ */
