/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * RX Pipeline: ring buffer → MPP decode → DRM render.
 *
 * Phase 4 (no MTL): input ring is connected to tx_pipeline output for
 * local loopback. Phase 5 will replace the ring producer with MTL ST22
 * session (network receive).
 */

#ifndef _E2E_RX_PIPELINE_H_
#define _E2E_RX_PIPELINE_H_

#include <stdint.h>
#include <st20_api.h>

struct ring_buffer;
struct rx_pipeline;

struct rx_pipeline_params {
    const char* drm_device;     /* "/dev/dri/card0" */
    const char* drm_connector;  /* "HDMI-A-1" */
    const char* drm_mode;       /* "1920x1080@60" */
    int         width, height;
    int         drm_buf_count;  /* display buffer count (3-4) */
    int         num_slices;     /* matches tx codec config */

    /* input: connected to tx_pipeline output ring */
    struct ring_buffer* input_ring;

    /* slab pool: shared with tx_pipeline; NULL for MTL-only RX path */
    struct slab_pool*  coded_pool;

    /* Phase 5 MTL: return framebuff to ST22 RX after decode */
    st22_rx_handle     rx_handle;

    /* PTP clock for cross-machine E2E latency (NULL → CLOCK_MONOTONIC) */
    uint64_t (*time_now)(void* priv);
    void*              time_priv;

    /* affinity */
    int         cpu_decode;     /* CPU core for decode+render thread */
};

struct rx_pipeline* rx_pipeline_create(struct rx_pipeline_params* p);
void  rx_pipeline_free(struct rx_pipeline* rp);

int   rx_pipeline_start(struct rx_pipeline* rp);
void  rx_pipeline_stop(struct rx_pipeline* rp);

/** Get E2E latency stats after stop. All values in nanoseconds. */
void  rx_pipeline_get_latency(struct rx_pipeline* rp,
                              uint64_t* min_ns, uint64_t* avg_ns, uint64_t* max_ns);

#endif /* _E2E_RX_PIPELINE_H_ */
