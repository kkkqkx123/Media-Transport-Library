/* SPDX-License-Identifier: BSD-3-Clause */

#include "../shared/tx_tai_ts.h"
#include <stdio.h>
#include <stdlib.h>

static int g_failed;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL: %s\n", msg);                                \
            g_failed++;                                                        \
        } else {                                                               \
            printf("  ok: %s\n", msg);                                         \
        }                                                                      \
    } while (0)

static const mtl_handle k_fake_st = (mtl_handle)(uintptr_t)1;

static void test_large_epoch_relative_spacing(void) {
    tx_tai_ts t;
    enum st10_timestamp_fmt tfmt;
    uint64_t ts;
    const uint64_t period   = 33366667; /* ~23.976 fps */
    const uint64_t anchor_e = 1000000;
    const uint64_t t0       = anchor_e * period;

    tx_tai_ts_init(&t, k_fake_st, period, true);

    ts = 0;
    tx_tai_ts_fill(&t, &tfmt, &ts, anchor_e, t0);
    CHECK(ts == t0, "first frame keeps MTL prefill");
    CHECK(tfmt == ST10_TIMESTAMP_FMT_TAI, "tfmt TAI");

    tx_tai_ts_fill(&t, &tfmt, &ts, anchor_e + 1, t0 + period);
    CHECK(ts == t0 + period, "second frame +1 period");

    tx_tai_ts_fill(&t, &tfmt, &ts, anchor_e + 2, t0 + 2 * period);
    CHECK(ts == t0 + 2 * period, "third frame +2 period");
}

static void test_reset_reanchors(void) {
    tx_tai_ts t;
    enum st10_timestamp_fmt tfmt;
    uint64_t ts;
    const uint64_t period = 1000000;

    tx_tai_ts_init(&t, k_fake_st, period, true);

    tx_tai_ts_fill(&t, &tfmt, &ts, 500, 500 * period);
    tx_tai_ts_reset(&t);

    const uint64_t new_anchor = 9000;
    const uint64_t new_t0     = new_anchor * period;
    tx_tai_ts_fill(&t, &tfmt, &ts, new_anchor, new_t0);
    CHECK(ts == new_t0, "after reset first frame at new anchor");

    tx_tai_ts_fill(&t, &tfmt, &ts, new_anchor + 3, new_t0 + 3 * period);
    CHECK(ts == new_t0 + 3 * period, "after reset relative offset from new anchor");
}

static void test_epoch_regression_reanchors(void) {
    tx_tai_ts t;
    enum st10_timestamp_fmt tfmt;
    uint64_t ts;
    const uint64_t period = 1000000;

    tx_tai_ts_init(&t, k_fake_st, period, true);

    tx_tai_ts_fill(&t, &tfmt, &ts, 100, 100 * period);
    tx_tai_ts_fill(&t, &tfmt, &ts, 101, 101 * period);
    tx_tai_ts_fill(&t, &tfmt, &ts, 102, 102 * period);

    const uint64_t reg_e = 50;
    const uint64_t reg_t = reg_e * period;
    tx_tai_ts_fill(&t, &tfmt, &ts, reg_e, reg_t);
    CHECK(ts == reg_t, "epoch regression re-anchors to regression epoch");

    tx_tai_ts_fill(&t, &tfmt, &ts, reg_e + 1, reg_t + period);
    CHECK(ts == reg_t + period, "post-regression +1 period");
}

static void test_st30_relative_prefill_anchor(void) {
    const uint64_t period  = 1000000;
    const uint64_t ptp_now = 1782270818995333632ULL;
    uint64_t       epoch_anchor;
    uint64_t       stream_start;
    bool           ptp_grid;

    tx_tai_compute_anchor(1, 1000000, ptp_now, period, &epoch_anchor, &stream_start,
                          &ptp_grid);
    CHECK(ptp_grid, "ST30 stale prefill uses ptp-grid anchor");
    CHECK(epoch_anchor == (ptp_now + period / 2) / period + 1,
          "epoch anchor on PTP grid");
    CHECK(stream_start == epoch_anchor * period, "stream start on MTL grid");

    const uint64_t video_period = 41708375;
    const uint64_t video_e      = 42731767888ULL;
    const uint64_t video_t      = 1782270818995333632ULL;
    tx_tai_compute_anchor(video_e, video_t, ptp_now, video_period, &epoch_anchor,
                          &stream_start, &ptp_grid);
    CHECK(!ptp_grid, "video prefill is absolute TAI");
    CHECK(epoch_anchor == video_e, "video epoch anchor from meta");
    CHECK(stream_start == video_t, "video stream start from MTL prefill");
    CHECK(stream_start + video_period == video_t + video_period,
          "video +1 period spacing");

    CHECK(tx_tai_needs_ptp_check(1, 1000000), "ST30 stale meta needs PTP check");
    CHECK(!tx_tai_needs_ptp_check(1, video_t), "large absolute prefill skips PTP check");
    CHECK(!tx_tai_needs_ptp_check(video_e, video_t), "video meta skips PTP check");
}

static void test_disabled_noop(void) {
    tx_tai_ts t;
    enum st10_timestamp_fmt tfmt = ST10_TIMESTAMP_FMT_MEDIA_CLK;
    uint64_t ts                  = 42;

    tx_tai_ts_init(&t, k_fake_st, 33366667, false);
    tx_tai_ts_fill(&t, &tfmt, &ts, 1000000, 999);
    CHECK(ts == 42, "disabled leaves timestamp unchanged");
    CHECK(tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK, "disabled leaves tfmt unchanged");
}

int main(void) {
    printf("test_tx_tai_ts\n");
    test_large_epoch_relative_spacing();
    test_reset_reanchors();
    test_epoch_regression_reanchors();
    test_st30_relative_prefill_anchor();
    test_disabled_noop();

    if (g_failed) {
        fprintf(stderr, "%d test(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
