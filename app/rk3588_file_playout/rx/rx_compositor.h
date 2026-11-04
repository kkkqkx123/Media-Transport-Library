#ifndef _RX_COMPOSITOR_H_
#define _RX_COMPOSITOR_H_

#include "../file_playout_app.h"
#include "../shared/display_layout.h"
#include <mtl_api.h>
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
unsigned long long rx_compositor_lane_lost_frames(const rx_compositor* comp, int idx);
unsigned long long rx_compositor_lane_last_pts_ns(const rx_compositor* comp, int idx);

#endif /* _RX_COMPOSITOR_H_ */
