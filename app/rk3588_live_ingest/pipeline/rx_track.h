#ifndef _LIVE_INGEST_RX_TRACK_H_
#define _LIVE_INGEST_RX_TRACK_H_

/* RX-side stream tracking helpers (pure C, no hardware deps).
 * Shared by rx_pipeline and the x86 unit test test_rx_track. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RX_LAT_MAX_NS (1000000000ULL) /* E2E latency sanity window: 1s */

/* ---- frame-sequence gap tracker (SEI frame_seq, 32-bit wrap) ---- */
struct rx_seq_tracker {
    uint32_t last;
    bool     have_last;
    uint64_t gap_events; /* frames that arrived with predecessors missing */
    uint64_t lost;       /* total missing frames implied by gaps */
    uint64_t dups;       /* duplicate / reordered-late frames */
};

static inline void rx_seq_reset(struct rx_seq_tracker* t) {
    if (!t) return;
    t->last = 0;
    t->have_last = false;
    t->gap_events = 0;
    t->lost = 0;
    t->dups = 0;
}

/* Observe one frame sequence number. First frame only establishes baseline. */
static inline void rx_seq_observe(struct rx_seq_tracker* t, uint32_t seq) {
    uint32_t expect;
    uint64_t gap;

    if (!t) return;
    if (!t->have_last) {
        t->last = seq;
        t->have_last = true;
        return;
    }
    if (seq == t->last) {
        t->dups++;
        return;
    }
    expect = t->last + 1; /* wraps mod 2^32 */
    if (seq == expect) {
        t->last = seq;
        return;
    }
    /* Forward distance mod 2^32; values in the upper half are treated as
     * late/reordered rather than a huge gap (sequence goes forward only). */
    gap = (seq >= expect) ? (uint64_t)(seq - expect)
                          : (uint64_t)(seq + (uint64_t)(0xFFFFFFFFu - expect) + 1u);
    if (gap <= 0x7FFFFFFFu) {
        t->gap_events++;
        t->lost += gap;
        t->last = seq;
    } else {
        t->dups++;
    }
}

/* ---- E2E latency validity (single clock domain, nanoseconds) ----
 * Returns true and stores now - cap_ts when the sample may enter stats.
 * cap_ts == 0, now < cap_ts (clock step back), or a delta beyond 1s
 * (clock-domain mismatch / stall) must be counted separately, not averaged. */
static inline bool rx_lat_valid(uint64_t cap_ts, uint64_t now, uint64_t* delta_out) {
    uint64_t delta;

    if (!cap_ts || now < cap_ts) return false;
    delta = now - cap_ts;
    if (delta > RX_LAT_MAX_NS) return false;
    if (delta_out) *delta_out = delta;
    return true;
}

/* ---- SEI payload offset (peek, no in-place strip) ----
 * Returns the byte offset the decoder must start from: SEI size when the
 * codestream opens with our timestamp SEI, 0 otherwise. */
static inline size_t rx_sei_payload_offset(int sei_present, size_t sei_size) {
    return sei_present ? sei_size : 0;
}

#endif /* _LIVE_INGEST_RX_TRACK_H_ */
