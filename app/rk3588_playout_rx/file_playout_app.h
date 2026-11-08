#ifndef _FILE_PLAYOUT_APP_H_
#define _FILE_PLAYOUT_APP_H_

#include "pipeline_common.h"
#include "display_layout.h"
#include <mtl_api.h>
#include <st20_api.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/** How MTL obtains PTP/TAI time. */
typedef enum file_playout_ptp_mode {
    FILE_PLAYOUT_PTP_EXTERNAL = 0, /**< linuxptp + ptp_get_time_fn → CLOCK_TAI (default) */
    FILE_PLAYOUT_PTP_BUILTIN,      /**< MTL_FLAG_PTP_ENABLE (needs HW timesync) */
    FILE_PLAYOUT_PTP_MONOTONIC,    /**< no PTP — relative monotonic anchor only */
} file_playout_ptp_mode;

/** ST2110-21 TX pacing profile (app-level selection over ST21_TX_PACING_WAY_*). */
typedef enum file_playout_pacing {
    FILE_PLAYOUT_PACING_AUTO = 0,     /**< MTL auto-detect (legacy default) */
    FILE_PLAYOUT_PACING_TSC,          /**< TSC linear (approximates Medium) */
    FILE_PLAYOUT_PACING_TSC_NARROW,   /**< TSC narrow gapped (ST2110-21 Narrow, default) */
    FILE_PLAYOUT_PACING_RL,           /**< rate-limit (needs HW rate-limit NIC) */
} file_playout_pacing;

/** ST2110-30 1ms ptime frame duration in nanoseconds */
#define MP4_ST30_FRAME_TIME_NS  (1000000ULL)

#ifdef __cplusplus
extern "C" {
#endif

#define FILE_PLAYOUT_MAX_NAME_LEN   64
#define FILE_PLAYOUT_MAX_PATH_LEN   256
#define FILE_PLAYOUT_MAX_IP_LEN     32
#define FILE_PLAYOUT_MAX_PLAYLIST   16

/** Full application configuration (from JSON + CLI). */
typedef struct file_playout_cfg {
    /* mode */
    enum file_playout_mode mode;
    int   test_time;               /* run duration in seconds */

    /* network */
    char  tx_port[FILE_PLAYOUT_MAX_NAME_LEN];   /* TX NIC port name, e.g. "native_af_xdp:eth0" */
    char  rx_port[FILE_PLAYOUT_MAX_NAME_LEN];   /* RX NIC port name */
    char  tx_lip[FILE_PLAYOUT_MAX_IP_LEN];      /* TX local IP */
    char  rx_lip[FILE_PLAYOUT_MAX_IP_LEN];      /* RX local IP */
    char  tx_dip[FILE_PLAYOUT_MAX_IP_LEN];      /* TX destination IP */
    char  rx_sip[FILE_PLAYOUT_MAX_IP_LEN];      /* RX source filter IP */
    uint16_t udp_port;                      /* shared UDP port */
    uint16_t audio_udp_port;                /* audio UDP port */

    /* video parameters */
    int    width;
    int    height;
    float  fps;
    size_t codestream_size;        /* max compressed frame size */

    /* MP4 source */
    char   mp4_file[FILE_PLAYOUT_MAX_PATH_LEN];
    char   mp4_files[FILE_PLAYOUT_MAX_PLAYLIST][FILE_PLAYOUT_MAX_PATH_LEN];
    int    mp4_count;
    bool   loop;
    int64_t seek_ms;
    char   probe_mp4[FILE_PLAYOUT_MAX_PATH_LEN];

    /* CLI overrides (when set, skip probe for that field) */
    bool   width_override;
    bool   height_override;
    bool   fps_override;

    /* RX display */
    char   drm_device[FILE_PLAYOUT_MAX_PATH_LEN];
    char   drm_connector[FILE_PLAYOUT_MAX_NAME_LEN];
    char   drm_mode[FILE_PLAYOUT_MAX_NAME_LEN];
    int    drm_buf_count;
    int    playout_delay_ms;

    /** Phase A PiP viewport; disabled = fullscreen (default). Phase B: multi-layer. */
    file_playout_display_viewport display_viewport;

    /** Phase B multi-stream PiP; count==0 → single-stream rx_pipeline. */
    file_playout_pip_stream pip_streams[MP4_PIP_MAX_LANES];
    int                  pip_stream_count;

    /* RX audio */
    char   alsa_device[FILE_PLAYOUT_MAX_NAME_LEN];

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
    file_playout_ptp_mode ptp_mode;
    int               ptp_utc_offset; /* subtract from TAI when GM uses UTC domain */

    /** ST2110-21 TX pacing profile (see docs/plan/rk3588/RK3588_ST2110_PROTOCOL_GAP_REMEDIATION_PLAN.md) */
    file_playout_pacing st21_pacing;

    /** Periodic RX stats/E2E report interval (seconds); 0 = exit only. Default 10. */
    int stats_report_interval_s;
    /** E2E-C report interval; 0 = use stats_report_interval_s. */
    int e2e_report_interval_s;

    /* statistics */
    atomic_ullong stat_rx_frames;
    atomic_ullong stat_rx_bytes;
    atomic_uint   stat_rx_drops;
    atomic_uint   stat_audio_underruns;
} file_playout_cfg;

/** Initialize config with safe defaults for RK3588. */
void file_playout_cfg_defaults(file_playout_cfg* cfg);

/** MTL initialization — PTP mode from cfg (external / builtin / monotonic). */
mtl_handle file_playout_mtl_init(const file_playout_cfg* cfg,
                             const char* port_name, const char* lip,
                             uint16_t tx_q, uint16_t rx_q,
                             enum mtl_log_level log_level,
                             const char* lcores);

/** Dual-port MTL initialization for dvs_media (single handle, RX+TX sessions).
 *
 * Uses the cfg send/receive port plan (tx_port/tx_lip + rx_port/rx_lip) and
 * registers both sides at once: a missing side is not registered, identical
 * port names collapse to one port with combined queues. Single-port apps keep
 * calling file_playout_mtl_init(); only the dual-role engine uses this.
 * lcores may be NULL/empty — then shared>rx>tx from cfg is picked. */
mtl_handle file_playout_mtl_init_dual(const file_playout_cfg* cfg,
                                   uint16_t tx_q, uint16_t rx_q,
                                   enum mtl_log_level log_level,
                                   const char* lcores);

#ifdef __cplusplus
}
#endif

#endif /* _FILE_PLAYOUT_APP_H_ */
