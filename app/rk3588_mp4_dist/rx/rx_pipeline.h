/* SPDX-License-Identifier: BSD-3-Clause
 *
 * RX Pipeline — orchestrates MTL RX → decode → playout → render + audio.
 * Extended from E2E rx_pipeline with ALSA audio and A/V sync.
 */

#ifndef _RX_PIPELINE_MP4_H_
#define _RX_PIPELINE_MP4_H_

#include "../mp4_dist_app.h"
#include "../shared/pipeline_common.h"
#include "av_sync.h"
#include "mtl_rx_mp4.h"
#include "mtl_rx_bridge.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

struct ring_buffer;
struct slab_pool;
struct codec_plugin;
struct drm_renderer;

typedef struct rx_pipeline rx_pipeline;

typedef struct rx_pipeline_params {
    /* DRM display */
    const char* drm_device;
    const char* drm_connector;
    const char* drm_mode;
    int         width;
    int         height;
    float       fps;
    int         drm_buf_count;

    /* pipeline I/O */
    struct ring_buffer* input_ring;   /* codec_packet* from bridge */
    struct slab_pool*   coded_pool;   /* for codec_packet data ownership */
    st22_rx_handle      rx_handle;    /* for framebuffer release */

    /* MTL handle (for creating audio RX session) */
    mtl_handle          mt;

    /* audio */
    const char* alsa_device;
    uint16_t    audio_udp_port;
    const char* rx_sip;            /* source filter IP */
    const char* rx_port;           /* NIC port for audio RX */

    /* sync */
    int         playout_delay_ms;
    int         test_time;     /* for decode yield vs expected fps */

    /* CPU affinity */
    int         cpu_decode;    /* MPP decode thread core */
    int         cpu_render;    /* DRM render thread core */
    int         cpu_audio;     /* ALSA audio RX thread core */

    /* display viewport (Phase A PiP); NULL/disabled in cfg → fullscreen */
    const mp4_dist_display_viewport* display_viewport;

    /* time source for latency measurement */
    uint64_t (*time_now)(void* priv);
    void*     time_priv;

    /** UDP port label for stats/E2E reporting. */
    uint16_t  udp_port;
} rx_pipeline_params;

rx_pipeline* rx_pipeline_create(const rx_pipeline_params* params);
int  rx_pipeline_start(rx_pipeline* rp);
void rx_pipeline_stop(rx_pipeline* rp);
void rx_pipeline_free(rx_pipeline* rp);

void rx_pipeline_get_latency(const rx_pipeline* rp,
                             uint64_t* min_ns, uint64_t* avg_ns, uint64_t* max_ns);

/** Number of E2E-C samples recorded (0 = not measured). */
uint64_t rx_pipeline_e2e_count(const rx_pipeline* rp);

/** Periodic or final stats block (E2E-C + RX counters). */
void rx_pipeline_report_stats(rx_pipeline* rp, const mtl_rx_mp4* mrx,
                            const mtl_rx_bridge* bridge, int period_s,
                            bool reset_period);

#endif /* _RX_PIPELINE_MP4_H_ */
