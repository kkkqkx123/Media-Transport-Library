#include "stream_validate.h"

#include <stdio.h>
#include <string.h>

int dvs_stream_shape_valid(const char* sip, uint16_t udp_port, int width,
                           int height, float fps, char* err, size_t errlen) {
    if (!sip || !sip[0]) {
        if (err && errlen > 0) snprintf(err, errlen, "stream missing sip");
        return -1;
    }
    if (udp_port == 0) {
        if (err && errlen > 0) snprintf(err, errlen, "stream missing udp_port");
        return -1;
    }
    if (width <= 0 || height <= 0) {
        if (err && errlen > 0)
            snprintf(err, errlen, "stream invalid width/height");
        return -1;
    }
    if (fps <= 0.0f) {
        if (err && errlen > 0) snprintf(err, errlen, "stream invalid fps");
        return -1;
    }
    return 0;
}

int dvs_stream_src_valid(const char* src_ip, char* err, size_t errlen) {
    if (!src_ip || !src_ip[0]) return 0;
    int parts = 0;
    const char* p = src_ip;
    while (1) {
        if (*p < '0' || *p > '9') break;
        int v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            digits++;
            p++;
            if (digits > 3) break;
        }
        if (digits == 0 || digits > 3 || v > 255) break;
        parts++;
        if (*p == '\0') {
            if (parts == 4) return 0;
            break;
        }
        if (*p != '.') break;
        p++;
        if (*p == '\0') break;
    }
    if (err && errlen > 0)
        snprintf(err, errlen, "stream bad source filter '%s'", src_ip);
    return -1;
}

int dvs_stream_crop_valid(int width, int height, int src_x, int src_y,
                          int src_w, int src_h, char* err, size_t errlen) {
    int cw = src_w > 0 ? src_w : width;
    int ch = src_h > 0 ? src_h : height;
    if (src_x < 0 || src_y < 0 || cw <= 0 || ch <= 0 || src_x + cw > width ||
        src_y + ch > height) {
        if (err && errlen > 0) {
            snprintf(err, errlen,
                     "bad source crop (%d,%d %dx%d of %dx%d)", src_x, src_y,
                     src_w, src_h, width, height);
        }
        return -1;
    }
    return 0;
}
