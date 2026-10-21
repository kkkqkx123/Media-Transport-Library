/* SPDX-License-Identifier: BSD-3-Clause
 *
 * MTL ST22 RX wrapper for MP4 distribution.
 * Adapted from E2E mtl_rx — uses session API with notify_frame_ready callback.
 */

#ifndef _MTL_RX_MP4_H_
#define _MTL_RX_MP4_H_

#include "../mp4_dist_app.h"
#include "../shared/pipeline_common.h"
#include "media_clk.h"
#include <st20_api.h>

/** Opaque RX context. */
typedef struct mtl_rx_mp4 mtl_rx_mp4;

/** Parameters for creating the RX session. */
typedef struct mtl_rx_mp4_params {
    mtl_handle      st;
    struct ring_buffer* output_ring;    /* push rx_pkt* here */
    int             framebuff_cnt;
    int             width;
    int             height;
    float           fps;
    uint16_t        udp_port;
    const char*     sip_addr;          /* source filter IP */
    const char*     port_name;         /* NIC port name */
    size_t          codestream_max_size; /* must match TX codestream_size */
    /** PTP anchor for MEDIA_CLK → TAI (NULL = monotonic fallback). */
    media_clk_time_fn time_fn;
    void*             time_priv;
} mtl_rx_mp4_params;

/**
 * Create the ST22 RX session (session API, codestream receive).
 * Frames are delivered via notify_frame_ready → ring_push.
 */
mtl_rx_mp4* mtl_rx_mp4_create(const mtl_rx_mp4_params* params);

/** Get the st22_rx_handle for framebuffer lifecycle management. */
st22_rx_handle mtl_rx_mp4_handle(mtl_rx_mp4* rx);

/** Start receiving (already active from creation — this is a no-op for API symmetry). */
int  mtl_rx_mp4_start(mtl_rx_mp4* rx);

/** Signal that reception is stopping (prevents new ring pushes). */
void mtl_rx_mp4_begin_stop(mtl_rx_mp4* rx);

/** Stop and drain pending frames. */
void mtl_rx_mp4_stop(mtl_rx_mp4* rx);

/** Free all resources. */
void mtl_rx_mp4_free(mtl_rx_mp4* rx);

/** Snapshot MTL RX counters (valid while running or after stop). */
void mtl_rx_mp4_get_stats(const mtl_rx_mp4* rx, uint64_t* received, uint32_t* drops,
                          uint32_t* corrupt, uint32_t* incomplete);

#endif /* _MTL_RX_MP4_H_ */
