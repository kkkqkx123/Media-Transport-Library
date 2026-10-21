/* SPDX-License-Identifier: BSD-3-Clause */

#include "../shared/display_layout.h"
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

static void test_defaults(void) {
    mp4_dist_display_viewport vp = {.enabled = true, .x = 10};
    mp4_dist_viewport_defaults(&vp);
    CHECK(!vp.enabled, "defaults disabled");
    CHECK(vp.x == 0, "defaults x=0");
}

static void test_fullscreen(void) {
    mp4_dist_display_viewport vp = {0};
    display_layout lay = display_layout_resolve(&vp, 1920, 1080);
    CHECK(lay.fullscreen, "disabled → fullscreen");
    CHECK(lay.layers[0].dst_w == 1920 && lay.layers[0].dst_h == 1080, "full rect");
}

static void test_viewport(void) {
    mp4_dist_display_viewport vp = {
        .enabled = true,
        .x = 1420,
        .y = 770,
        .width = 480,
        .height = 270,
    };
    display_layout lay = display_layout_resolve(&vp, 1920, 1080);
    CHECK(!lay.fullscreen, "enabled → viewport");
    CHECK(lay.layers[0].dst_x == 1420 && lay.layers[0].dst_y == 770, "position");
    CHECK(lay.layers[0].dst_w == 480 && lay.layers[0].dst_h == 270, "size");
}

static void test_invalid_fallback(void) {
    mp4_dist_display_viewport vp = {
        .enabled = true,
        .x = 1800,
        .y = 0,
        .width = 480,
        .height = 270,
    };
    display_layout lay = display_layout_resolve(&vp, 1920, 1080);
    CHECK(lay.fullscreen, "invalid rect → fullscreen fallback");
}

static void test_pip_dual(void) {
    mp4_dist_pip_stream streams[2] = {
        {
            .main = true,
            .sip = "192.168.80.213",
            .udp_port = 20000,
            .width = 1920,
            .height = 1080,
            .fps = 23.976f,
            .x = 0,
            .y = 0,
            .w = 1920,
            .h = 1080,
        },
        {
            .main = false,
            .sip = "192.168.80.214",
            .udp_port = 20010,
            .width = 1280,
            .height = 720,
            .fps = 23.976f,
            .x = 1420,
            .y = 770,
            .w = 480,
            .h = 270,
        },
    };

    display_layout lay;
    int n = display_layout_from_pip_streams(streams, 2, 1920, 1080, &lay);
    CHECK(n == 2, "pip dual layer count");
    CHECK(lay.compositor, "pip dual compositor");
    CHECK(lay.layers[0].plane_id == 57, "lane0 plane 57");
    CHECK(lay.layers[1].plane_id == 79, "lane1 plane 79");
    CHECK(lay.layers[0].main && !lay.layers[1].main, "main flag");
    CHECK(lay.layers[1].dst_x == 1420 && lay.layers[1].dst_w == 480, "sub rect");
}

static void test_pip_no_main(void) {
    mp4_dist_pip_stream streams[1] = {
        {
            .main = false,
            .sip = "10.0.0.1",
            .udp_port = 20000,
            .width = 1920,
            .height = 1080,
            .fps = 24.0f,
            .x = 0,
            .y = 0,
            .w = 1920,
            .h = 1080,
        },
    };
    CHECK(mp4_dist_pip_validate(streams, 1, 1920, 1080) < 0, "reject no main");
}

static void test_tx_streams_dual(void) {
    mp4_dist_tx_stream streams[2] = {
        {
            .main = true,
            .mp4_file = "a.mp4",
            .udp_port = 20000,
            .width = 1920,
            .height = 1080,
            .fps = 24.0f,
        },
        {
            .main = false,
            .mp4_file = "b.mp4",
            .udp_port = 20010,
            .width = 1280,
            .height = 720,
            .fps = 24.0f,
        },
    };
    CHECK(mp4_dist_tx_streams_validate(streams, 2) == 0, "tx dual valid");
    streams[1].udp_port = 20000;
    CHECK(mp4_dist_tx_streams_validate(streams, 2) < 0, "tx duplicate port");
}

int main(void) {
    printf("test_display_layout\n");
    test_defaults();
    test_fullscreen();
    test_viewport();
    test_invalid_fallback();
    test_pip_dual();
    test_pip_no_main();
    test_tx_streams_dual();
    printf("result: %d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
