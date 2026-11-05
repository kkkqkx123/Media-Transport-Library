/* TX capture port mapping — physical-identity-first device resolution.
 *
 * Startup table binds HDMIk to the V4L2 physical identity
 * (QUERYCAP card + bus_info + input index) with /dev/videoN kept only as the
 * runtime path. TX start rescans and resolves the requested port by stable ID
 * first, device path second; a missing target is rejected with the available
 * table attached, never silently rebound to another signal.
 *
 * This header is hardware-free (plain C strings, no MTL/MPP/RGA) so the table
 * construction stays x86-unit-testable. The scan itself degrades gracefully:
 * no devices means count 0, never a hard failure. */

#ifndef _DVS_TX_PORT_MAP_H_
#define _DVS_TX_PORT_MAP_H_

#include <stddef.h>

#define DVS_TX_PORT_MAX   8
#define DVS_TX_DEV_LEN    32
#define DVS_TX_STABLE_LEN 128
#define DVS_TX_PORT_LEN   16

typedef struct {
    char port[DVS_TX_PORT_LEN];         /* "HDMI1" .. "HDMI8" */
    char device[DVS_TX_DEV_LEN];        /* "/dev/videoN" at scan time */
    char stable_id[DVS_TX_STABLE_LEN];  /* "card|bus|input" or device fallback */
    int  has_signal;                   /* 1 = signal, 0 = no signal/unknown */
} dvs_tx_port_entry;

/* Build HDMI1..n in scan order. Returns entries written, 0 for empty scan. */
int dvs_tx_table_build(dvs_tx_port_entry* out, int cap,
                       const char devices[][DVS_TX_DEV_LEN],
                       const char stable_ids[][DVS_TX_STABLE_LEN],
                       const int* has_signal, int n);

/* "HDMIk" -> k (1-based), -1 when not a capture port id. */
int dvs_tx_port_index(const char* port);

/* Resolve a requested port to the current device path.
 * requested accepts "HDMIk", a "/dev/videoN" path, or a stable ID string.
 * table is the startup binding, (devices/stable_ids,m) the fresh rescan.
 * Returns 0 with out_device set; -1 with err carrying the reason plus the
 * available port table. No-signal targets are rejected, never rebound. */
int dvs_tx_resolve_device(const dvs_tx_port_entry* table, int table_n,
                          const char devices[][DVS_TX_DEV_LEN],
                          const char stable_ids[][DVS_TX_STABLE_LEN],
                          const int* has_signal, int m,
                          const char* requested,
                          char* out_device, size_t out_len,
                          char* err, size_t errlen);

/* "HDMI1(/dev/video0), HDMI2(/dev/video2)" into buf. Always NUL-terminates. */
int dvs_tx_format_available(const dvs_tx_port_entry* table, int n,
                            char* buf, size_t len);

/* Codec name to encoder format: 0 = H264, 1 = H265, -1 = unsupported. */
int dvs_tx_codec_from_name(const char* codec);

/* kbps (500..20000, validated upstream) to bps. -1 on non-positive input. */
int dvs_tx_bitrate_to_bps(int kbps, int* bps_out);

/* Engineering defaults (tuned on single-board smoke, not locked). */
#define DVS_TX_DEF_SLICES      1
#define DVS_TX_DEF_V4L2_BUFS   6
#define DVS_TX_DEF_RGA_POOL    12
#define DVS_TX_DEF_CODED_BLK   (512 * 1024)
#define DVS_TX_DEF_RING        2
#define DVS_TX_DEF_FB_CNT      8
#define DVS_TX_DEF_FB_MAX      (512 * 1024)
#define DVS_TX_DEF_CPU_CAP     1
#define DVS_TX_DEF_CPU_ENC     2

/* GOP tracks the frame rate; fps<=0 falls back to 60. */
int dvs_tx_default_gop(int fps);

/* Rescan /dev/video0..7 into the array form used by table_build.
 * Fills device paths, stable IDs (QUERYCAP card|bus|input, device fallback),
 * and per-device signal flags (G_FMT placeholder check). Returns device
 * count, 0 when none present. Never fails hard. */
int dvs_tx_scan_devices(char devices[][DVS_TX_DEV_LEN],
                        char stable_ids[][DVS_TX_STABLE_LEN],
                        int* has_signal, int cap);

#endif /* _DVS_TX_PORT_MAP_H_ */
