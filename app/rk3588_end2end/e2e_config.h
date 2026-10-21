/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * JSON configuration loader for e2e_app (optional --config).
 */

#ifndef _E2E_CONFIG_H_
#define _E2E_CONFIG_H_

#include <stdbool.h>
#include <stdint.h>

struct e2e_cfg {
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
    const char* rx_port;
    const char* rx_lip;
    const char* rx_sip;
    const char* v4l2_device;
    const char* drm_device;
    const char* drm_connector;
    const char* drm_mode;
    int         cpu_capture, cpu_encode, cpu_bridge, cpu_decode;
    const char* log_level;
};

struct e2e_cfg e2e_cfg_defaults(void);

/** Load JSON config; overlays defaults. Returns 0 on success. */
int e2e_config_load(const char* path, struct e2e_cfg* cfg);

#endif
