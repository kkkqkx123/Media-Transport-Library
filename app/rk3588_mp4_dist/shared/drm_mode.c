/* SPDX-License-Identifier: BSD-3-Clause */

#include "drm_mode.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

bool mp4_drm_mode_is_auto(const char* mode_str) {
    if (!mode_str || !mode_str[0])
        return true;
    return strcasecmp(mode_str, "auto") == 0;
}

int mp4_drm_suggest_refresh(float fps) {
    if (fps <= 0.0f)
        return 60;

    static const struct {
        float content_fps;
        int   refresh_hz;
    } map[] = {
        {23.976f, 24},
        {24.0f,   24},
        {25.0f,   25},
        {29.97f,  30},
        {30.0f,   30},
        {50.0f,   50},
        {59.94f,  60},
        {60.0f,   60},
        {119.88f, 120},
        {120.0f,  120},
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        float d = fps - map[i].content_fps;
        if (d < 0.0f)
            d = -d;
        if (d < 0.06f)
            return map[i].refresh_hz;
    }

    return (int)(fps + 0.5f);
}

int mp4_drm_build_mode_string(int width, int height, float fps,
                              char* out, size_t out_len) {
    if (!out || out_len < 8)
        return -1;

    if (width <= 0)
        width = 1920;
    if (height <= 0)
        height = 1080;

    int hz = mp4_drm_suggest_refresh(fps);
    snprintf(out, out_len, "%dx%d@%d", width, height, hz);
    return 0;
}
