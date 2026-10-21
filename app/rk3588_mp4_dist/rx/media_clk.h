/* SPDX-License-Identifier: BSD-3-Clause
 *
 * ST2110 media clock (90k RTP) → presentation timeline (PTP/TAI or monotonic).
 */

#ifndef _MEDIA_CLK_H_
#define _MEDIA_CLK_H_

#include <stdbool.h>
#include <stdint.h>
#include <st_api.h>

/** Optional wall-clock source for first-frame anchor (e.g. mtl_ptp_read_time). */
typedef uint64_t (*media_clk_time_fn)(void* priv);

/** Stateful anchor for unwrap + relative PTS. */
typedef struct media_clk_anchor {
    bool              anchored;
    uint64_t          base_unwrapped;
    uint64_t          base_pts_ns;
    uint32_t          last_raw;
    bool              have_last;
    uint64_t          last_unwrapped;
    uint64_t          stat_frames;
    media_clk_time_fn time_fn;
    void*             time_priv;
    bool              use_ptp_anchor; /* true when time_fn set at first anchor */
} media_clk_anchor;

void media_clk_anchor_init(media_clk_anchor* a);

/** Attach PTP/TAI time source; NULL fn keeps monotonic anchor (tests / fallback). */
void media_clk_set_time_source(media_clk_anchor* a, media_clk_time_fn fn, void* priv);

/**
 * Map ST22/ST30 frame meta to presentation PTS (nanoseconds).
 * TAI timestamps pass through unchanged.
 */
uint64_t media_clk_frame_pts(media_clk_anchor* a, enum st10_timestamp_fmt tfmt,
                             uint64_t timestamp);

/** Unit-test helper: unwrap 32-bit RTP timestamp to 64-bit linear counter. */
uint64_t media_clk_unwrap_test(media_clk_anchor* a, uint32_t raw);

#endif /* _MEDIA_CLK_H_ */
