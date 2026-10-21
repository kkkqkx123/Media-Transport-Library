/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Common types and defines shared across all MP4 distribution modules.
 */

#ifndef _MP4_DIST_APP_H_
#define _MP4_DIST_APP_H_

#include "shared/pipeline_common.h"
#include "shared/display_layout.h"
#include <mtl_api.h>
#include <st20_api.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/** How MTL obtains PTP/TAI time (see doc/design.md §5.4). */
typedef enum mp4_dist_ptp_mode {
    MP4_DIST_PTP_EXTERNAL = 0, /**< linuxptp + ptp_get_time_fn → CLOCK_TAI (default) */
    MP4_DIST_PTP_BUILTIN,      /**< MTL_FLAG_PTP_ENABLE (needs HW timesync) */
    MP4_DIST_PTP_MONOTONIC,    /**< no PTP — relative monotonic anchor only */
} mp4_dist_ptp_mode;

/** ST2110-30 1ms ptime frame duration in nanoseconds */
#define MP4_ST30_FRAME_TIME_NS  (1000000ULL)

#ifdef __cplusplus
extern "C" {
#endif

#define MP4_DIST_MAX_NAME_LEN   64
#define MP4_DIST_MAX_PATH_LEN   256
#define MP4_DIST_MAX_IP_LEN     32
#define MP4_DIST_MAX_PLAYLIST   16

/** Full application configuration (from JSON + CLI). */
typedef struct mp4_dist_cfg {
    /* mode */
    enum mp4_dist_mode mode;
    int   test_time;               /* run duration in seconds */

    /* network */
    char  tx_port[MP4_DIST_MAX_NAME_LEN];   /* TX NIC port name, e.g. "native_af_xdp:eth0" */
    char  rx_port[MP4_DIST_MAX_NAME_LEN];   /* RX NIC port name */
    char  tx_lip[MP4_DIST_MAX_IP_LEN];      /* TX local IP */
    char  rx_lip[MP4_DIST_MAX_IP_LEN];      /* RX local IP */
    char  tx_dip[MP4_DIST_MAX_IP_LEN];      /* TX destination IP */
    char  rx_sip[MP4_DIST_MAX_IP_LEN];      /* RX source filter IP */
    uint16_t udp_port;                      /* shared UDP port */
    uint16_t audio_udp_port;                /* audio UDP port */

    /* video parameters */
    int    width;
    int    height;
    float  fps;
    size_t codestream_size;        /* max compressed frame size */

    /* MP4 source */
    char   mp4_file[MP4_DIST_MAX_PATH_LEN];
    char   mp4_files[MP4_DIST_MAX_PLAYLIST][MP4_DIST_MAX_PATH_LEN];
    int    mp4_count;
    bool   loop;
    int64_t seek_ms;
    char   probe_mp4[MP4_DIST_MAX_PATH_LEN];

    /* CLI overrides (when set, skip probe for that field) */
    bool   width_override;
    bool   height_override;
    bool   fps_override;

    /* RX display */
    char   drm_device[MP4_DIST_MAX_PATH_LEN];
    char   drm_connector[MP4_DIST_MAX_NAME_LEN];
    char   drm_mode[MP4_DIST_MAX_NAME_LEN];
    int    drm_buf_count;
    int    playout_delay_ms;

    /** Phase A PiP viewport; disabled = fullscreen (default). Phase B: multi-layer. */
    mp4_dist_display_viewport display_viewport;

    /** Phase B multi-stream PiP; count==0 → legacy single-stream rx_pipeline. */
    mp4_dist_pip_stream pip_streams[MP4_PIP_MAX_LANES];
    int                  pip_stream_count;

    /** One ST22 TX lane (tx_streams[]); count==0 → legacy single mp4_file path. */
    mp4_dist_tx_stream tx_streams[MP4_PIP_MAX_LANES];
    int                tx_stream_count;

    /* RX audio */
    char   alsa_device[MP4_DIST_MAX_NAME_LEN];

    /* CPU affinity — pthread threads (see rx_pipeline / mtl_rx_bridge) */
    int    cpu_bridge;      /* bridge: MTL fb → slab memcpy */
    int    cpu_decode;      /* MPP decode thread */
    int    cpu_render;      /* DRM display thread */
    int    cpu_audio;       /* ST30 → ALSA thread */

    /* MTL DPDK lcore list (comma-separated physical CPUs). Separate RX/TX for
     * both-mode fork so two MTL instances do not pile onto the same core. */
    char   mtl_lcores_rx[32];
    char   mtl_lcores_tx[32];
    /** When set (both-mode), RX and TX share one worker list for MtlManager. */
    char   mtl_lcores_shared[32];

    enum mtl_log_level mtl_log_level; /* MTL library log verbosity */

    /* PTP / global time (P5) */
    mp4_dist_ptp_mode ptp_mode;
    int               ptp_utc_offset; /* subtract from TAI when GM uses UTC domain */

    /** Periodic RX stats/E2E report interval (seconds); 0 = exit only. Default 10. */
    int stats_report_interval_s;
    /** E2E-C report interval; 0 = use stats_report_interval_s. */
    int e2e_report_interval_s;

    /* statistics */
    atomic_ullong stat_tx_frames;
    atomic_ullong stat_rx_frames;
    atomic_ullong stat_rx_bytes;
    atomic_uint   stat_rx_drops;
    atomic_uint   stat_audio_underruns;
} mp4_dist_cfg;

/** Initialize config with safe defaults for RK3588. */
void mp4_dist_cfg_defaults(mp4_dist_cfg* cfg);

/** MTL initialization — PTP mode from cfg (external / builtin / monotonic). */
mtl_handle mp4_dist_mtl_init(const mp4_dist_cfg* cfg,
                             const char* port_name, const char* lip,
                             uint16_t tx_q, uint16_t rx_q,
                             enum mtl_log_level log_level,
                             const char* lcores);

#ifdef __cplusplus
}
#endif

#endif /* _MP4_DIST_APP_H_ */
