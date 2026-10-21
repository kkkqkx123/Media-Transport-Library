/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Display viewport / multi-layer layout (Phase A: single NV12 layer).
 * Phase B: pip_streams[] up to MP4_PIP_MAX_LANES (v1 NV12 direct: 4).
 */

#ifndef _MP4_DISPLAY_LAYOUT_H_
#define _MP4_DISPLAY_LAYOUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MP4_PIP_SIP_LEN           32
#define DISPLAY_LAYOUT_MAX_LAYERS 8
#define MP4_PIP_MAX_LANES         8
#define MP4_PIP_V1_NV12_LANES     4

/** JSON display_viewport — Phase A single window. */
typedef struct mp4_dist_display_viewport {
    bool enabled;
    int  x;
    int  y;
    int  width;
    int  height;
} mp4_dist_display_viewport;

/** One ST22 receive lane (Phase B pip_streams[] entry). */
typedef struct mp4_dist_pip_stream {
    int      lane;
    bool     main;
    char     sip[MP4_PIP_SIP_LEN];
    uint16_t udp_port;
    uint16_t audio_udp_port;
    int      width;
    int      height;
    float    fps;
    int      x;
    int      y;
    int      w;
    int      h;
    size_t   codestream_size;
} mp4_dist_pip_stream;

/** One ST22 transmit lane (TX tx_streams[] entry). */
typedef struct mp4_dist_tx_stream {
    bool     main;
    char     mp4_file[256];
    uint16_t udp_port;
    uint16_t audio_udp_port;
    int      width;
    int      height;
    float    fps;
    size_t   codestream_size;
    bool     loop;
    int64_t  seek_ms;
} mp4_dist_tx_stream;

/** Resolved scanout rectangle for one video layer. */
typedef struct display_layer {
    int      dst_x;
    int      dst_y;
    int      dst_w;
    int      dst_h;
    int      zpos;
    uint32_t plane_id;
    int      lane;
    bool     main;
} display_layer;

/** Resolved layout for the HDMI canvas. */
typedef struct display_layout {
    bool  fullscreen;
    bool  compositor;
    int   mode_w;
    int   mode_h;
    int   layer_count;
    display_layer layers[DISPLAY_LAYOUT_MAX_LAYERS];
} display_layout;

void mp4_dist_viewport_defaults(mp4_dist_display_viewport* vp);

display_layout display_layout_resolve(const mp4_dist_display_viewport* vp,
                                      int mode_w, int mode_h);

/** Build layout from pip_streams[]; returns layer_count or -1 on error. */
int display_layout_from_pip_streams(const mp4_dist_pip_stream* streams, int count,
                                    int mode_w, int mode_h, display_layout* out);

/** Validate pip config (one main, unique ports, rects in mode). Returns 0 or -1. */
int mp4_dist_pip_validate(const mp4_dist_pip_stream* streams, int count,
                          int mode_w, int mode_h);

/** Validate TX tx_streams[] (one main, unique udp_port, mp4 + video params). */
int mp4_dist_tx_streams_validate(const mp4_dist_tx_stream* streams, int count);

#endif /* _MP4_DISPLAY_LAYOUT_H_ */
