/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Unit tests for media_clk unwrap + PTS timeline.
 *
 * Usage: test_media_clk
 */

#include "../rx/media_clk.h"
#include <stdio.h>
#include <stdlib.h>

static int g_failed;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        g_failed++; \
    } else { \
        printf("  ok: %s\n", msg); \
    } \
} while (0)

static void test_unwrap_wraparound(void) {
    media_clk_anchor a;
    media_clk_anchor_init(&a);

    uint64_t u0 = media_clk_unwrap_test(&a, 0xFFFFFF00u);
    CHECK(u0 == 0xFFFFFF00u, "unwrap first");

    uint64_t u1 = media_clk_unwrap_test(&a, 0x00000100u);
    CHECK(u1 == 0x100000100u, "unwrap +512 across 32-bit boundary");

    uint64_t u2 = media_clk_unwrap_test(&a, 0x00000200u);
    CHECK(u2 == u1 + 256, "unwrap +256 linear");
}

static void test_media_clk_pts_monotonic(void) {
    media_clk_anchor a;
    media_clk_anchor_init(&a);

    uint64_t p0 = media_clk_frame_pts(&a, ST10_TIMESTAMP_FMT_MEDIA_CLK, 1000);
    uint64_t p1 = media_clk_frame_pts(&a, ST10_TIMESTAMP_FMT_MEDIA_CLK, 1000 + 3753);
    uint64_t p2 = media_clk_frame_pts(&a, ST10_TIMESTAMP_FMT_MEDIA_CLK, 1000 + 7506);

    CHECK(p0 > 0, "first PTS non-zero");
    CHECK(p1 > p0, "PTS monotonic +1 step");
    CHECK(p2 > p1, "PTS monotonic +2 step");

    uint64_t step = p1 - p0;
    uint64_t expect = 1000000000ULL * 3753 / 90000;
    uint64_t diff = (step > expect) ? (step - expect) : (expect - step);
    CHECK(diff <= 2, "step ~3753/90k ticks in ns");
}

static void test_tai_passthrough(void) {
    media_clk_anchor a;
    media_clk_anchor_init(&a);

    uint64_t tai = 123456789012345ULL;
    uint64_t p = media_clk_frame_pts(&a, ST10_TIMESTAMP_FMT_TAI, tai);
    CHECK(p == tai, "TAI passthrough");
}

int main(void) {
    printf("test_media_clk\n");
    test_unwrap_wraparound();
    test_media_clk_pts_monotonic();
    test_tai_passthrough();
    printf("result: %d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
