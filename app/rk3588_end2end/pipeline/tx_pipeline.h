/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * TX Pipeline: V4L2 capture → RGA convert → MPP encode → ring buffer output.
 *
 * Phase 4 (no MTL): the output ring is consumed by rx_pipeline for local
 * loopback testing. Phase 5 will replace the ring consumer with MTL ST22
 * session for network transmission.
 */

#ifndef _E2E_TX_PIPELINE_H_
#define _E2E_TX_PIPELINE_H_

#include "pipeline_common.h"
#include <stdint.h>

struct ring_buffer;
struct tx_pipeline;

/** Optional MTL direct-write callbacks (e2e_app wires to mtl_tx). */
struct tx_mtl_sink_ops {
    void* priv;
    int  (*acquire_fb)(void* priv, uint16_t* idx, void** addr);
    int  (*publish_fb)(void* priv, uint16_t idx, size_t size);
    void (*abort_fb)(void* priv, uint16_t idx);
};

struct tx_pipeline_params {
    const char* v4l2_device;    /* "/dev/video0" */
    int         width, height;
    float       fps;
    int         v4l2_buf_count; /* capture buffer count (4-6) */
    const char* drm_device;     /* "/dev/dri/card0" — for RGA DMA alloc */
    int         rga_pool_size;  /* RGA output buffer pool (4) */

    /* codec */
    int         bitrate_bps;    /* e.g. 40000000 */
    int         gop_size;       /* e.g. 60 */
    int         num_slices;     /* 1 = frame-level, 2 = slice-level */
    int         intra_refresh_mode; /* 0=off, 1=MB-row, 2=MB-col, 3=MB-gap */
    int         intra_refresh_arg;  /* rows/cols/gap per frame */
    int         coded_blk_size; /* slab block size for coded data */
    int         ring_size;      /* CAP2ENC ring capacity (2-3) */

    /* optional: PTP time source callback (NULL = CLOCK_MONOTONIC fallback).
     * Phase 5: pass mtl_ptp_read_time for cross-machine E2E measurement.
     * Phase 1-4: NULL — uses clock_gettime(CLOCK_MONOTONIC). */
    uint64_t (*time_now)(void* priv);
    void*       time_priv;

    /* affinity */
    int         cpu_capture;    /* CPU core for capture thread */
    int         cpu_encode;     /* CPU core for encode thread */

    /** NULL = ring output for test_pipeline; set for e2e MTL direct-write */
    struct tx_mtl_sink_ops* mtl_sink;
};

struct tx_pipeline* tx_pipeline_create(struct tx_pipeline_params* p);
void  tx_pipeline_free(struct tx_pipeline* tp);

/** Start capture and encode threads. Non-blocking. */
int   tx_pipeline_start(struct tx_pipeline* tp);
/** Signal threads to stop and join them. */
void  tx_pipeline_stop(struct tx_pipeline* tp);

/** Get the output ring (ENC2DEC). Consumer (rx_pipeline) pops from it. */
struct ring_buffer* tx_pipeline_get_output_ring(struct tx_pipeline* tp);
/** Get the shared slab pool for coded data lifecycle management. */
struct slab_pool*  tx_pipeline_get_slab_pool(struct tx_pipeline* tp);

#endif /* _E2E_TX_PIPELINE_H_ */
