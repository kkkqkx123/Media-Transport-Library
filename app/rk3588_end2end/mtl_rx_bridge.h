/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * Bridges MTL ST22 RX output (rx_pkt ring) to rx_pipeline input (codec_packet ring).
 * Copies codestream into slab_pool and returns MTL framebuff immediately.
 */

#ifndef _E2E_MTL_RX_BRIDGE_H_
#define _E2E_MTL_RX_BRIDGE_H_

#include <stdint.h>
#include <st20_api.h>

struct ring_buffer;
struct slab_pool;
struct mtl_rx_bridge;

struct mtl_rx_bridge_params {
    struct ring_buffer* mtl_ring;   /* from mtl_rx output */
    struct ring_buffer* dec_ring;   /* to rx_pipeline input */
    st22_rx_handle      rx_handle;  /* for returning framebuff on drain/error */
    struct slab_pool*   coded_pool; /* RX codestream copy pool */
    int                 cpu;        /* bridge thread affinity */
};

struct mtl_rx_bridge* mtl_rx_bridge_create(struct mtl_rx_bridge_params* p);
void mtl_rx_bridge_free(struct mtl_rx_bridge* b);
int  mtl_rx_bridge_start(struct mtl_rx_bridge* b);
void mtl_rx_bridge_stop(struct mtl_rx_bridge* b);

#endif
