/* SPDX-License-Identifier: BSD-3-Clause */

#include "../shared/drm_mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        g_failed++; \
    } else { \
        printf("  ok: %s\n", msg); \
    } \
} while (0)

static void test_refresh(void) {
    CHECK(mp4_drm_suggest_refresh(23.976f) == 24, "23.976 → 24");
    CHECK(mp4_drm_suggest_refresh(24.0f) == 24, "24 → 24");
    CHECK(mp4_drm_suggest_refresh(29.97f) == 30, "29.97 → 30");
    CHECK(mp4_drm_suggest_refresh(30.0f) == 30, "30 → 30");
    CHECK(mp4_drm_suggest_refresh(59.94f) == 60, "59.94 → 60");
    CHECK(mp4_drm_suggest_refresh(50.0f) == 50, "50 → 50");
}

static void test_build(void) {
    char buf[64];
    CHECK(mp4_drm_build_mode_string(1920, 1080, 23.976f, buf, sizeof(buf)) == 0,
          "build ok");
    CHECK(!strcmp(buf, "1920x1080@24"), buf);
    CHECK(mp4_drm_build_mode_string(1280, 720, 60.0f, buf, sizeof(buf)) == 0,
          "720p60 build");
    CHECK(!strcmp(buf, "1280x720@60"), buf);
}

static void test_auto(void) {
    CHECK(mp4_drm_mode_is_auto("auto"), "auto string");
    CHECK(mp4_drm_mode_is_auto("AUTO"), "AUTO string");
    CHECK(mp4_drm_mode_is_auto(""), "empty");
    CHECK(!mp4_drm_mode_is_auto("1920x1080@30"), "explicit not auto");
}

int main(void) {
    printf("test_drm_mode\n");
    test_refresh();
    test_build();
    test_auto();
    printf("result: %d failed\n", g_failed);
    return g_failed ? 1 : 0;
}
