#include "display_layout.h"
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
    file_playout_display_viewport vp = {.enabled = true, .x = 10};
    file_playout_viewport_defaults(&vp);
    CHECK(!vp.enabled, "defaults disabled");
    CHECK(vp.x == 0, "defaults x=0");
}

static void test_fullscreen(void) {
    file_playout_display_viewport vp = {0};
    display_layout lay = display_layout_resolve(&vp, 1920, 1080);
    CHECK(lay.fullscreen, "disabled → fullscreen");
    CHECK(lay.layers[0].dst_w == 1920 && lay.layers[0].dst_h == 1080, "full rect");
}

static void test_viewport(void) {
    file_playout_display_viewport vp = {
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
    file_playout_display_viewport vp = {
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
    file_playout_pip_stream streams[2] = {
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
    file_playout_pip_stream streams[1] = {
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
    CHECK(file_playout_pip_validate(streams, 1, 1920, 1080) < 0, "reject no main");
}

static void test_tx_streams_dual(void) {
    file_playout_tx_stream streams[2] = {
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
    CHECK(file_playout_tx_streams_validate(streams, 2) == 0, "tx dual valid");
    streams[1].udp_port = 20000;
    CHECK(file_playout_tx_streams_validate(streams, 2) < 0, "tx duplicate port");
}

static void test_pip_srccrop(void) {
    file_playout_pip_stream streams[1] = {
        {
            .main = true,
            .sip = "239.0.0.1",
            .udp_port = 5000,
            .width = 3840,
            .height = 1080,
            .fps = 60.0f,
            .x = 0,
            .y = 0,
            .w = 960,
            .h = 540,
            .src_x = 1920,
            .src_y = 0,
            .src_w = 1920,
            .src_h = 1080,
        },
    };
    CHECK(file_playout_pip_validate(streams, 1, 1920, 1080) == 0,
          "partial crop validates");
    display_layout lay;
    CHECK(display_layout_from_pip_streams(streams, 1, 1920, 1080, &lay) == 1,
          "cropped single builds one layer");
    CHECK(lay.layers[0].src_x == 1920 && lay.layers[0].src_w == 1920,
          "crop reaches the plane source rect");
    CHECK(lay.layers[0].src_y == 0 && lay.layers[0].src_h == 1080,
          "crop height passes through");
    CHECK(lay.layers[0].dst_w == 960 && lay.layers[0].dst_h == 540,
          "target rect untouched by crop");
    CHECK(!lay.fullscreen && lay.compositor, "cropped single uses compositor");
}

static void test_pip_reject(void) {
    display_layout lay;
    CHECK(display_layout_from_pip_streams(NULL, 1, 1920, 1080, &lay) < 0,
          "null streams rejected");
    CHECK(display_layout_from_pip_streams(NULL, 0, 1920, 1080, NULL) < 0,
          "null out rejected");
    file_playout_pip_stream base = {
        .main = true,
        .sip = "239.0.0.1",
        .udp_port = 5000,
        .width = 1920,
        .height = 1080,
        .fps = 60.0f,
        .x = 0,
        .y = 0,
        .w = 1920,
        .h = 1080,
    };
    CHECK(file_playout_pip_validate(NULL, 1, 1920, 1080) < 0, "null rejected");
    CHECK(file_playout_pip_validate(&base, 0, 1920, 1080) < 0, "empty rejected");
    CHECK(file_playout_pip_validate(&base, MP4_PIP_MAX_LANES + 1, 1920, 1080) < 0,
          "over lane cap rejected");
    {
        file_playout_pip_stream s = base;
        s.sip[0] = '\0';
        CHECK(file_playout_pip_validate(&s, 1, 1920, 1080) < 0, "missing sip rejected");
    }
    {
        file_playout_pip_stream s = base;
        s.udp_port = 0;
        CHECK(file_playout_pip_validate(&s, 1, 1920, 1080) < 0, "missing port rejected");
    }
    {
        file_playout_pip_stream s = base;
        s.width = 0;
        CHECK(file_playout_pip_validate(&s, 1, 1920, 1080) < 0, "zero width rejected");
    }
    {
        file_playout_pip_stream s = base;
        s.fps = 0.0f;
        CHECK(file_playout_pip_validate(&s, 1, 1920, 1080) < 0, "zero fps rejected");
    }
    {
        file_playout_pip_stream s = base;
        s.w = 200;
        s.h = 200;
        s.x = 1800;
        s.y = 900;
        CHECK(file_playout_pip_validate(&s, 1, 1920, 1080) < 0,
              "rect outside mode rejected");
    }
    {
        file_playout_pip_stream s = base;
        s.src_x = 1800;
        s.src_w = 200;
        CHECK(file_playout_pip_validate(&s, 1, 1920, 1080) < 0,
              "crop overflowing the frame rejected");
    }
    {
        file_playout_pip_stream s = base;
        s.src_y = -1;
        CHECK(file_playout_pip_validate(&s, 1, 1920, 1080) < 0,
              "negative crop origin rejected");
    }
    {
        file_playout_pip_stream pair[2] = {base, base};
        pair[1].main = false;
        pair[1].x = 100;
        CHECK(file_playout_pip_validate(pair, 2, 1920, 1080) < 0,
              "duplicate sip:port rejected");
    }
    {
        file_playout_pip_stream pair[2] = {base, base};
        CHECK(file_playout_pip_validate(pair, 2, 1920, 1080) < 0,
              "two mains rejected");
    }
    {
        /* Zero w/h crop falls back to the full frame per axis. */
        file_playout_pip_stream s = base;
        s.src_w = 0;
        s.src_h = 0;
        display_layout l2;
        CHECK(display_layout_from_pip_streams(&s, 1, 1920, 1080, &l2) == 1,
              "zero crop builds");
        CHECK(l2.layers[0].src_w == 1920 && l2.layers[0].src_h == 1080,
              "zero crop resolves to full frame");
    }
}

static void test_pip_fullscreen_single(void) {
    file_playout_pip_stream streams[1] = {
        {
            .main = true,
            .sip = "239.0.0.1",
            .udp_port = 5000,
            .width = 1920,
            .height = 1080,
            .fps = 60.0f,
            .x = 0,
            .y = 0,
            .w = 1920,
            .h = 1080,
        },
    };
    display_layout lay;
    CHECK(display_layout_from_pip_streams(streams, 1, 1920, 1080, &lay) == 1,
          "fullscreen single builds");
    CHECK(lay.fullscreen && !lay.compositor, "fullscreen bypasses compositor");
    CHECK(lay.layers[0].plane_id == 57, "fullscreen keeps plane 57");
}

static void test_tx_streams_reject(void) {
    CHECK(file_playout_tx_streams_validate(NULL, 1) < 0, "tx null rejected");
    file_playout_tx_stream base = {
        .main = true,
        .mp4_file = "a.mp4",
        .udp_port = 20000,
        .width = 1920,
        .height = 1080,
        .fps = 24.0f,
    };
    CHECK(file_playout_tx_streams_validate(&base, 0) < 0, "tx empty rejected");
    CHECK(file_playout_tx_streams_validate(&base, MP4_PIP_MAX_LANES + 1) < 0,
          "tx over lane cap rejected");
    {
        file_playout_tx_stream s = base;
        s.mp4_file[0] = '\0';
        CHECK(file_playout_tx_streams_validate(&s, 1) < 0, "tx missing file rejected");
    }
    {
        file_playout_tx_stream s = base;
        s.main = false;
        CHECK(file_playout_tx_streams_validate(&s, 1) < 0, "tx zero mains rejected");
    }
    {
        file_playout_tx_stream pair[2] = {base, base};
        pair[1].udp_port = 20010;
        CHECK(file_playout_tx_streams_validate(pair, 2) < 0, "tx two mains rejected");
    }
    {
        file_playout_tx_stream s = base;
        s.height = 0;
        CHECK(file_playout_tx_streams_validate(&s, 1) < 0, "tx zero height rejected");
    }
}

int main(void) {
    printf("test_display_layout\n");
    test_defaults();
    test_fullscreen();
    test_viewport();
    test_invalid_fallback();
    test_pip_dual();
    test_pip_no_main();
    test_pip_srccrop();
    test_pip_reject();
    test_pip_fullscreen_single();
    test_tx_streams_dual();
    test_tx_streams_reject();
    printf("result: %d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
