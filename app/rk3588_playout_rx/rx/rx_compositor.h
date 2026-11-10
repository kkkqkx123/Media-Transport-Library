#ifndef _RX_COMPOSITOR_H_
#define _RX_COMPOSITOR_H_

#include "../file_playout_app.h"
#include "codec_plugin.h"
#include "display_layout.h"
#include <mtl_api.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct rx_compositor rx_compositor;

typedef struct rx_compositor_params {
    mtl_handle mt;

    const char* drm_device;
    const char* drm_connector;
    const char* drm_mode;
    int         drm_buf_count;

    const file_playout_pip_stream* streams;
    int                        stream_count;

    const char* rx_port;
    const char* alsa_device;
    int         playout_delay_ms;
    int         test_time;

    size_t      default_codestream_size;
    uint16_t    default_audio_udp_port;

    int cpu_bridge;
    int cpu_decode;
    int cpu_render;
    int cpu_audio;

    /* video codec for all lanes (CODEC_FMT_H264 default for zero-init). */
    enum codec_format codec;

    uint64_t (*time_now)(void* priv);
    void*     time_priv;
} rx_compositor_params;

rx_compositor* rx_compositor_create(const rx_compositor_params* params);
int  rx_compositor_start(rx_compositor* comp);
void rx_compositor_stop(rx_compositor* comp);
void rx_compositor_free(rx_compositor* comp);
int rx_compositor_try_fast_apply(rx_compositor* comp,
                                const file_playout_pip_stream* streams,
                                int count);

void rx_compositor_get_latency(const rx_compositor* comp,
                               uint64_t* min_ns, uint64_t* avg_ns, uint64_t* max_ns);

uint64_t rx_compositor_e2e_count(const rx_compositor* comp);

void rx_compositor_report_stats(rx_compositor* comp, int period_s, bool reset_period);

/* Per-lane introspection for control-plane status (lost frames / last PTS).
 * Safe to call with a NULL comp (returns 0 / NULL). */
int rx_compositor_lane_count(const rx_compositor* comp);
const file_playout_pip_stream* rx_compositor_lane_stream(const rx_compositor* comp,
                                                          int idx);
int rx_compositor_lane_running(const rx_compositor* comp, int idx);
/* Ready = decode thread alive and PTS progressed within the freshness window.
 * A stale lane still counts as a session but not as decoding (ME-2). */
int rx_compositor_lane_ready(const rx_compositor* comp, int idx);
int rx_compositor_ready_count(const rx_compositor* comp);
/* Current on-screen layer count (0 when no compositor). */
int rx_compositor_layer_count(const rx_compositor* comp);
/* Reselect the ST30 audio subscription without rebuilding video lanes.
 * Empty sip or zero port stops audio (mute). Returns 0 on success. */
int rx_compositor_audio_select(rx_compositor* comp, const char* sip,
                               uint16_t audio_port);
/* Paint the background plane with XRGB8888 pixels (stretched to the mode
 * when dimensions differ). The still survives lane swaps; clear to black is
 * the renderer's default. Returns 0 on success. */
int rx_compositor_set_background(rx_compositor* comp, const uint8_t* px,
                                  int w, int h);
int rx_compositor_audio_running(const rx_compositor* comp);
unsigned long long rx_compositor_lane_lost_frames(const rx_compositor* comp, int idx);
unsigned long long rx_compositor_lane_last_pts_ns(const rx_compositor* comp, int idx);

#endif /* _RX_COMPOSITOR_H_ */
