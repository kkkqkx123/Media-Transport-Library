/* SPDX-License-Identifier: BSD-3-Clause
 *
 * MTL RX Bridge — copies codestream from MTL hugepage buffers to slab pool,
 * then pushes codec_packet to the decode ring.
 *
 * Adapted from E2E mtl_rx_bridge.
 */

#ifndef _MTL_RX_BRIDGE_H_
#define _MTL_RX_BRIDGE_H_

#include "../shared/pipeline_common.h"
#include <st20_api.h>
#include <stdint.h>

struct ring_buffer;
struct slab_pool;
struct mtl_rx_mp4;
typedef struct mtl_rx_bridge mtl_rx_bridge;

typedef struct mtl_rx_bridge_params {
    struct ring_buffer* mtl_ring;    /* input: rx_pkt* from mtl_rx_mp4 */
    struct ring_buffer* dec_ring;    /* output: codec_packet* to rx_pipeline */
    st22_rx_handle     rx_handle;   /* for framebuffer release */
    struct slab_pool*  coded_pool;  /* slab for coded data copies */
    int                cpu;         /* CPU affinity for bridge thread */
} mtl_rx_bridge_params;

mtl_rx_bridge* mtl_rx_bridge_create(const mtl_rx_bridge_params* params);
int  mtl_rx_bridge_start(mtl_rx_bridge* b);
void mtl_rx_bridge_stop(mtl_rx_bridge* b);
void mtl_rx_bridge_free(mtl_rx_bridge* b);

/** Snapshot bridge counters (valid while running or after stop). */
void mtl_rx_bridge_get_stats(const mtl_rx_bridge* b, uint64_t* bridged,
                             uint32_t* drops, uint64_t* stamp_ok);

#endif /* _MTL_RX_BRIDGE_H_ */
