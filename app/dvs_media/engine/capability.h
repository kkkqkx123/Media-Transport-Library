#ifndef _DVS_CAPABILITY_H_
#define _DVS_CAPABILITY_H_

#include <stddef.h>

/* Runtime-probed node registration capability (req 03 §12). All fields are
 * filled by dvs_capability_probe(); any subsystem that cannot be probed falls
 * back to a sane default and logs a warning — probing must never abort the
 * process, so dvs_media can still serve IPC and report partial capability. */
typedef struct {
    /* output ports (DRM connectors), each with its supported mode count */
    char** output_ports; /* array of connector names, e.g. "HDMI-A-1" */
    int*   output_mode_counts;
    int    output_port_count;

    /* max simultaneous decode sessions (MPP measured; default conservative) */
    int    max_decode_sessions;

    /* max DRM layers (plane count) */
    int    max_layers;

    /* PTP */
    char   ptp_state[32];  /* "synced" | "unsynced" | "unknown" */
    int    ptp_offset_us;  /* TAI offset in microseconds */

    /* MTL */
    char   mtl_version[32];
    char   mtl_backend[32]; /* "dpdk" | "af_xdp" | "kernel" */
} dvs_capability;

/* Probe all subsystems. Returns 0 on success (best-effort; never fails hard). */
int dvs_capability_probe(dvs_capability* cap);

/* Release dynamically allocated arrays inside cap. */
void dvs_capability_free(dvs_capability* cap);

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
