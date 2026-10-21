/* SPDX-License-Identifier: BSD-3-Clause */

#include "display_layout.h"
#include <stdio.h>
#include <string.h>

/* RK3588 Esmart NV12 planes (Phase B v1 direct display). */
static const uint32_t k_nv12_plane_ids[MP4_PIP_V1_NV12_LANES] = {57, 79, 101, 123};

void mp4_dist_viewport_defaults(mp4_dist_display_viewport* vp) {
    if (!vp)
        return;
    vp->enabled = false;
    vp->x       = 0;
    vp->y       = 0;
    vp->width   = 0;
    vp->height  = 0;
}

static display_layout layout_fullscreen(int mode_w, int mode_h) {
    display_layout lay = {
        .fullscreen  = true,
        .compositor  = false,
        .mode_w      = mode_w,
        .mode_h      = mode_h,
        .layer_count = 1,
    };
    lay.layers[0].dst_x = 0;
    lay.layers[0].dst_y = 0;
    lay.layers[0].dst_w = mode_w;
    lay.layers[0].dst_h = mode_h;
    lay.layers[0].zpos  = 0;
    lay.layers[0].lane  = 0;
    lay.layers[0].main  = true;
    lay.layers[0].plane_id = k_nv12_plane_ids[0];
    return lay;
}

static int clamp_mode(int* mode_w, int* mode_h) {
    if (*mode_w <= 0)
        *mode_w = 1920;
    if (*mode_h <= 0)
        *mode_h = 1080;
    return 0;
}

static bool rect_valid(int x, int y, int w, int h, int mode_w, int mode_h) {
    return w > 0 && h > 0 && x >= 0 && y >= 0 && w <= mode_w && h <= mode_h &&
           x + w <= mode_w && y + h <= mode_h;
}

display_layout display_layout_resolve(const mp4_dist_display_viewport* vp,
                                      int mode_w, int mode_h) {
    clamp_mode(&mode_w, &mode_h);

    if (!vp || !vp->enabled)
        return layout_fullscreen(mode_w, mode_h);

    int w = vp->width > 0 ? vp->width : mode_w;
    int h = vp->height > 0 ? vp->height : mode_h;
    int x = vp->x;
    int y = vp->y;

    if (!rect_valid(x, y, w, h, mode_w, mode_h)) {
        fprintf(stderr,
                "display_viewport: invalid rect (%d,%d %dx%d) for mode %dx%d — "
                "fallback fullscreen\n",
                x, y, w, h, mode_w, mode_h);
        return layout_fullscreen(mode_w, mode_h);
    }

    display_layout lay = {
        .fullscreen  = (w == mode_w && h == mode_h && x == 0 && y == 0),
        .compositor  = true,
        .mode_w      = mode_w,
        .mode_h      = mode_h,
        .layer_count = 1,
    };
    lay.layers[0].dst_x    = x;
    lay.layers[0].dst_y    = y;
    lay.layers[0].dst_w    = w;
    lay.layers[0].dst_h    = h;
    lay.layers[0].zpos     = 1;
    lay.layers[0].lane     = 0;
    lay.layers[0].main     = true;
    lay.layers[0].plane_id = k_nv12_plane_ids[0];
    if (lay.fullscreen)
        lay.compositor = false;
    return lay;
}

int mp4_dist_pip_validate(const mp4_dist_pip_stream* streams, int count,
                          int mode_w, int mode_h) {
    if (!streams || count <= 0) {
        fprintf(stderr, "pip_streams: empty\n");
        return -1;
    }
    if (count > MP4_PIP_MAX_LANES) {
        fprintf(stderr, "pip_streams: count %d > max %d\n", count,
                MP4_PIP_MAX_LANES);
        return -1;
    }
    if (count > MP4_PIP_V1_NV12_LANES) {
        fprintf(stderr,
                "pip_streams: WARN %d lanes > v1 NV12 limit %d — only first %d enabled\n",
                count, MP4_PIP_V1_NV12_LANES, MP4_PIP_V1_NV12_LANES);
    }

    clamp_mode(&mode_w, &mode_h);

    int main_count = 0;
    for (int i = 0; i < count; i++) {
        const mp4_dist_pip_stream* s = &streams[i];
        if (!s->sip[0]) {
            fprintf(stderr, "pip_streams[%d]: missing sip\n", i);
            return -1;
        }
        if (!s->udp_port) {
            fprintf(stderr, "pip_streams[%d]: missing udp_port\n", i);
            return -1;
        }
        if (s->width <= 0 || s->height <= 0 || s->fps <= 0.0f) {
            fprintf(stderr, "pip_streams[%d]: invalid width/height/fps\n", i);
            return -1;
        }
        if (!rect_valid(s->x, s->y, s->w, s->h, mode_w, mode_h)) {
            fprintf(stderr,
                    "pip_streams[%d]: invalid rect (%d,%d %dx%d) for mode %dx%d\n",
                    i, s->x, s->y, s->w, s->h, mode_w, mode_h);
            return -1;
        }
        if (s->main)
            main_count++;

        for (int j = i + 1; j < count; j++) {
            if (streams[j].udp_port == s->udp_port &&
                !strcmp(streams[j].sip, s->sip)) {
                fprintf(stderr, "pip_streams: duplicate sip:port %s:%u\n",
                        s->sip, s->udp_port);
                return -1;
            }
        }
    }

    if (main_count != 1) {
        fprintf(stderr, "pip_streams: require exactly one main=true (got %d)\n",
                main_count);
        return -1;
    }
    return 0;
}

int display_layout_from_pip_streams(const mp4_dist_pip_stream* streams, int count,
                                    int mode_w, int mode_h, display_layout* out) {
    if (!out || !streams || count <= 0)
        return -1;
    if (mp4_dist_pip_validate(streams, count, mode_w, mode_h) < 0)
        return -1;

    clamp_mode(&mode_w, &mode_h);

    int n = count;
    if (n > MP4_PIP_V1_NV12_LANES)
        n = MP4_PIP_V1_NV12_LANES;

    memset(out, 0, sizeof(*out));
    out->mode_w      = mode_w;
    out->mode_h      = mode_h;
    out->layer_count = n;
    out->compositor  = true;
    out->fullscreen  = (n == 1 && streams[0].x == 0 && streams[0].y == 0 &&
                        streams[0].w == mode_w && streams[0].h == mode_h);

    for (int i = 0; i < n; i++) {
        const mp4_dist_pip_stream* s = &streams[i];
        display_layer* lay           = &out->layers[i];
        lay->dst_x                   = s->x;
        lay->dst_y                   = s->y;
        lay->dst_w                   = s->w;
        lay->dst_h                   = s->h;
        lay->zpos                    = s->main ? 0 : (i + 1);
        lay->lane                    = s->lane >= 0 ? s->lane : i;
        lay->main                    = s->main;
        lay->plane_id                = k_nv12_plane_ids[i];
    }

    if (out->fullscreen)
        out->compositor = false;

    return n;
}

int mp4_dist_tx_streams_validate(const mp4_dist_tx_stream* streams, int count) {
    if (!streams || count <= 0) {
        fprintf(stderr, "tx_streams: empty\n");
        return -1;
    }
    if (count > MP4_PIP_MAX_LANES) {
        fprintf(stderr, "tx_streams: count %d > max %d\n", count, MP4_PIP_MAX_LANES);
        return -1;
    }

    int main_count = 0;
    for (int i = 0; i < count; i++) {
        const mp4_dist_tx_stream* s = &streams[i];
        if (!s->mp4_file[0]) {
            fprintf(stderr, "tx_streams[%d]: missing mp4_file\n", i);
            return -1;
        }
        if (!s->udp_port) {
            fprintf(stderr, "tx_streams[%d]: missing udp_port\n", i);
            return -1;
        }
        if (s->width <= 0 || s->height <= 0 || s->fps <= 0.0f) {
            fprintf(stderr, "tx_streams[%d]: invalid width/height/fps\n", i);
            return -1;
        }
        if (s->main)
            main_count++;

        for (int j = i + 1; j < count; j++) {
            if (streams[j].udp_port == s->udp_port) {
                fprintf(stderr, "tx_streams: duplicate udp_port %u\n", s->udp_port);
                return -1;
            }
        }
    }

    if (main_count != 1) {
        fprintf(stderr, "tx_streams: require exactly one main=true (got %d)\n",
                main_count);
        return -1;
    }
    return 0;
}
