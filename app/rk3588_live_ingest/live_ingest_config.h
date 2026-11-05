#ifndef _LIVE_INGEST_CONFIG_H_
#define _LIVE_INGEST_CONFIG_H_

#include <stdbool.h>
#include <stdint.h>

struct live_ingest_cfg {
    int         test_time;
    int         width, height;
    float       fps;
    uint16_t    udp_port;
    int         num_slices;
    int         bitrate_bps;
    int         gop_size;
    int         intra_refresh_mode;
    int         intra_refresh_arg;
    const char* tx_port;
    const char* tx_lip;
    const char* tx_dip;
    const char* v4l2_device;
    const char* drm_device;
    int         cpu_capture, cpu_encode;
    const char* log_level;
};

struct live_ingest_cfg live_ingest_cfg_defaults(void);

/** Load JSON config; overlays defaults. Returns 0 on success. */
int live_ingest_config_load(const char* path, struct live_ingest_cfg* cfg);

#endif
