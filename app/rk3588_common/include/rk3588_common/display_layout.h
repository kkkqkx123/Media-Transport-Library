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
typedef struct file_playout_display_viewport {
    bool enabled;
    int  x;
    int  y;
    int  width;
    int  height;
} file_playout_display_viewport;

/** One ST22 receive lane (Phase B pip_streams[] entry). */
typedef struct file_playout_pip_stream {
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
    /* Source crop inside the decoded frame (forward-compatible fields; the
     * compositor renders full-frame today — see
     * DVS_WINDOWS_TRANSLATION_GAPS_DESIGN.md 源裁剪分两步走). Zero w/h means
     * the full frame. The agent rejects cross-node windows, so in practice
     * these always describe the full frame. */
    int      src_x;
    int      src_y;
    int      src_w;
    int      src_h;
    size_t   codestream_size;
} file_playout_pip_stream;

/** One ST22 transmit lane (TX tx_streams[] entry). */
typedef struct file_playout_tx_stream {
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
} file_playout_tx_stream;

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

void file_playout_viewport_defaults(file_playout_display_viewport* vp);

display_layout display_layout_resolve(const file_playout_display_viewport* vp,
                                      int mode_w, int mode_h);

/** Build layout from pip_streams[]; returns layer_count or -1 on error. */
int display_layout_from_pip_streams(const file_playout_pip_stream* streams, int count,
                                    int mode_w, int mode_h, display_layout* out);

/** Validate pip config (one main, unique ports, rects in mode). Returns 0 or -1. */
int file_playout_pip_validate(const file_playout_pip_stream* streams, int count,
                          int mode_w, int mode_h);

/** Validate TX tx_streams[] (one main, unique udp_port, mp4 + video params). */
int file_playout_tx_streams_validate(const file_playout_tx_stream* streams, int count);

#endif /* _MP4_DISPLAY_LAYOUT_H_ */
