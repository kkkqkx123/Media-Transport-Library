#ifndef _DVS_CAPABILITY_H_
#define _DVS_CAPABILITY_H_

#include <stddef.h>

/* Runtime-probed node registration capability. All fields are
 * filled by dvs_capability_probe(); any subsystem that cannot be probed falls
 * back to a sane default and logs a warning — probing must never abort the
 * process, so dvs_media can still serve IPC and report partial capability. */
typedef struct {
    /* output ports (DRM connectors), each with its supported mode count */
    char** output_ports; /* array of connector names, e.g. "HDMI-A-1" */
    int*   output_mode_counts;
    int*   output_max_w;
    int*   output_max_h;
    int*   output_max_hz;
    int    output_port_count;
    /* input ports (capture); probed from V4L2 devices, empty when absent.
     * Parallel arrays share input_port_count; device/stable/signal come from
     * the same scan table the TX backend resolves (physical identity first). */
    char** input_ports;
    char** input_devices;  /* current /dev/videoN path per port */
    char** input_stable;   /* stable physical identity per port */
    int*   input_signal;   /* 1 = signal present */
    int    input_port_count;

    /* max simultaneous decode sessions (MPP measured; default conservative) */
    int    max_decode_sessions;

    /* max DRM layers (plane count) */
    int    max_layers;

    /* TX/send static ceiling (SoC public limits, not probed). Per-board
     * variance, if any, is documented by the board bring-up. */
    char   tx_codecs[64]; /* comma list, e.g. "h264,h265" */
    int    tx_max_w;
    int    tx_max_h;
    int    tx_max_fps;
    int    tx_max_br_kbps;

    /* PTP */
    char   ptp_state[32];  /* "synced" | "unsynced" | "unknown" */
    int    ptp_offset_us;  /* TAI offset in microseconds */

    /* MTL */
    char   mtl_version[32];
    char   mtl_backend[32]; /* "dpdk" | "af_xdp" | "kernel" */
} dvs_capability;

/* Probe all subsystems. Returns 0 on success (best-effort; never fails hard). */
int dvs_capability_probe(dvs_capability* cap);
/* Static decode ceiling: min(compositor lane limit, DRM plane count), with
 * the DVS_MAX_DECODE_SESSIONS bring-up override taking precedence. */
int dvs_capability_static_ceiling(const dvs_capability* cap);
/* Runtime available decodes: static ceiling minus admitted sessions. */
int dvs_capability_available(const dvs_capability* cap);
void dvs_capability_refresh_mtl(dvs_capability* cap, void* mtl_handle);
/* Backend from the MTL port names, sharing the PMD prefix mapping with
 * file_playout_mtl_init_dual. Single/dual merge uses the registered ports. */
void dvs_capability_refresh_backend(dvs_capability* cap, const char* tx_port,
                                     const char* rx_port);
/* PTP refresh honoring the playout clock mode (0 external, 1 builtin,
 * 2 monotonic). Monotonic always reports unknown. */
void dvs_capability_refresh_ptp(dvs_capability* cap, void* mtl_handle,
                                int ptp_mode);

/* Override the MPP decode-session ceiling (RK3588 bring-up default 4).
 * n <= 0 restores the default. The static ceiling takes the minimum of
 * the plane limit, the compositor lane limit and this value; the
 * DVS_MAX_DECODE_SESSIONS environment override still wins. */
void dvs_capability_set_mpp_ceiling(int n);

/* Backend name for one MTL port string. Never NULL: "dpdk" | "af_xdp" |
 * "kernel" | "unknown". Shared by the capability refresh and the IPC
 * bridge so both paths report the same name. */
const char* dvs_capability_backend_name(const char* port);

/* PTP state math without clock reads (unit-testable). tai_valid == 0
 * (unsynced handle or monotonic mode) reports unknown with zero offset;
 * otherwise synced with delta_us clamped into the int range. */
void dvs_ptp_state_from_delta(char* state, size_t n, int* offset_us,
                              int tai_valid, long long delta_us);

/* Release dynamically allocated arrays inside cap. */
void dvs_capability_free(dvs_capability* cap);

/* Fill the TX static ceiling (shared by probe default and seed). */
void dvs_capability_tx_defaults(dvs_capability* cap);

/* Serialize capability as a JSON string (caller json_object_put). NULL on err. */
char* dvs_capability_to_json(const dvs_capability* cap);

/* Build the query_state *data* JSON fragment into buf.
 * Live counters (rx_sessions/decoding/layers) come from the running engine;
 * when the engine is not started they are 0, which is the honest state.
 * Returns 0 on success, -1 if buf too small. */
int dvs_capability_state_json(char* buf, size_t n, int rx_sessions,
                              int decoding, int layers,
                              const dvs_capability* cap);

#endif /* _DVS_CAPABILITY_H_ */
