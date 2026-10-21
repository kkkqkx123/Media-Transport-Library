/* SPDX-License-Identifier: BSD-3-Clause
 *
 * MTL ST22 RX wrapper — session API, notify_frame_ready callback,
 * codestream frames pushed to a ring buffer.
 *
 * Adapted from E2E mtl_rx.c.
 */

#include "mtl_rx_mp4.h"
#include "../shared/ring_buffer.h"
#include "../shared/au_trace.h"
#include "../shared/pipeline_common.h"
#include "media_clk.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <st_api.h>

#define RX_POOL_SIZE 16   /* rx_pkt descriptor pool, match mtl_ring depth */

struct mtl_rx_mp4 {
    mtl_rx_mp4_params   cfg;
    st22_rx_handle      handle;
    media_clk_anchor    clk;

    struct rx_pkt       pool[RX_POOL_SIZE];
    atomic_bool         stopping;

    uint64_t            stat_received;
    uint32_t            stat_drops;
    uint32_t            stat_corrupt;
    uint32_t            stat_incomplete;
};

/* ---- pool helpers ---- */
static struct rx_pkt* rx_pool_acquire(mtl_rx_mp4* rx) {
    for (int i = 0; i < RX_POOL_SIZE; i++) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&rx->pool[i].used, &expected, 1))
            return &rx->pool[i];
    }
    return NULL;
}

static void rx_pool_release(struct rx_pkt* pkt) {
    if (!pkt) return;
    pkt->frame     = NULL;
    pkt->size      = 0;
    pkt->tai_ns    = 0;
    pkt->media_clk = 0;
    atomic_store(&pkt->used, 0);
}

/* ---- lcore callback: receive frame, push to ring (NON-BLOCKING) ---- */
static int rx_notify_frame_ready(void* priv, void* frame,
                                 struct st22_rx_frame_meta* meta) {
    mtl_rx_mp4* rx = (mtl_rx_mp4*)priv;

    if (atomic_load(&rx->stopping)) {
        st22_rx_put_framebuff(rx->handle, frame);
        return 0;
    }

    if (meta->status == ST_FRAME_STATUS_CORRUPTED) {
        rx->stat_corrupt++;
        st22_rx_put_framebuff(rx->handle, frame);
        return 0;
    }
    if (meta->status != ST_FRAME_STATUS_COMPLETE &&
        meta->status != ST_FRAME_STATUS_RECONSTRUCTED) {
        rx->stat_incomplete++;
        st22_rx_put_framebuff(rx->handle, frame);
        return 0;
    }

    struct rx_pkt* pkt = rx_pool_acquire(rx);
    if (!pkt) {
        rx->stat_drops++;
        st22_rx_put_framebuff(rx->handle, frame);
        return -ENOSPC;
    }

    pkt->frame  = frame;
    pkt->size   = meta->frame_total_size;
    pkt->media_clk = (uint32_t)meta->timestamp;
    pkt->tai_ns = media_clk_frame_pts(&rx->clk, meta->tfmt, meta->timestamp);

    au_trace_record(AU_TRACE_RX_MTL, (const uint8_t*)frame, pkt->size,
                    (int64_t)pkt->tai_ns);

    if (ring_push(rx->cfg.output_ring, pkt) == -ENOSPC) {
        rx_pool_release(pkt);
        rx->stat_drops++;
        st22_rx_put_framebuff(rx->handle, frame);
        return -ENOSPC;
    }

    rx->stat_received++;
    return 0;
}

/* ---- public API ---- */

mtl_rx_mp4* mtl_rx_mp4_create(const mtl_rx_mp4_params* params) {
    mtl_rx_mp4* rx = calloc(1, sizeof(*rx));
    if (!rx) return NULL;

    rx->cfg = *params;
    media_clk_anchor_init(&rx->clk);
    media_clk_set_time_source(&rx->clk, params->time_fn, params->time_priv);
    for (int i = 0; i < RX_POOL_SIZE; i++)
        atomic_init(&rx->pool[i].used, 0);
    atomic_init(&rx->stopping, false);

    struct st22_rx_ops ops = {0};
    ops.name               = "mp4_dist_rx";
    ops.priv               = rx;
    ops.type               = ST22_TYPE_FRAME_LEVEL;
    ops.fps                = st_frame_rate_to_st_fps((double)params->fps);
    ops.width              = (uint32_t)(params->width > 0 ? params->width : 1920);
    ops.height             = (uint32_t)(params->height > 0 ? params->height : 1080);
    ops.fmt                = ST20_FMT_YUV_420_8BIT;
    ops.pack_type          = ST22_PACK_CODESTREAM;
    ops.num_port           = 1;
    ops.framebuff_cnt       = (uint16_t)params->framebuff_cnt;
    ops.framebuff_max_size  = params->codestream_max_size > 0
                              ? params->codestream_max_size
                              : (1024 * 1024);
    ops.notify_frame_ready  = rx_notify_frame_ready;
    ops.payload_type       = 112;
    ops.udp_port[0]        = params->udp_port;

    if (params->sip_addr) {
        unsigned int a, b, c, d;
        if (sscanf(params->sip_addr, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            ops.ip_addr[0][0] = (uint8_t)a;
            ops.ip_addr[0][1] = (uint8_t)b;
            ops.ip_addr[0][2] = (uint8_t)c;
            ops.ip_addr[0][3] = (uint8_t)d;
        }
    }
    strncpy(ops.port[0], params->port_name, MTL_PORT_MAX_LEN - 1);

    rx->handle = st22_rx_create(params->st, &ops);
    if (!rx->handle) {
        fprintf(stderr, "mtl_rx: st22_rx_create failed\n");
        free(rx);
        return NULL;
    }

    printf("mtl_rx: %d fbs, max=%zu, %.0ffps, port %u ready\n",
           params->framebuff_cnt, ops.framebuff_max_size,
           (double)params->fps, params->udp_port);
    return rx;
}

st22_rx_handle mtl_rx_mp4_handle(mtl_rx_mp4* rx) {
    return rx ? rx->handle : NULL;
}

int mtl_rx_mp4_start(mtl_rx_mp4* rx) { (void)rx; return 0; }

void mtl_rx_mp4_begin_stop(mtl_rx_mp4* rx) {
    if (!rx) return;
    atomic_store(&rx->stopping, true);
}

void mtl_rx_mp4_stop(mtl_rx_mp4* rx) {
    if (!rx) return;
    for (int i = 0; i < RX_POOL_SIZE; i++) {
        if (rx->pool[i].frame) {
            st22_rx_put_framebuff(rx->handle, rx->pool[i].frame);
            rx_pool_release(&rx->pool[i]);
        }
    }
    struct rx_pkt* pkt;
    while (ring_pop(rx->cfg.output_ring, (void**)&pkt) == 0) {
        if (pkt->frame)
            st22_rx_put_framebuff(rx->handle, pkt->frame);
        rx_pool_release(pkt);
    }
    printf("mtl_rx: %llu received, %u drops, %u corrupt, %u incomplete\n",
           (unsigned long long)rx->stat_received, rx->stat_drops,
           rx->stat_corrupt, rx->stat_incomplete);
}

void mtl_rx_mp4_free(mtl_rx_mp4* rx) {
    if (!rx) return;
    mtl_rx_mp4_stop(rx);
    if (rx->handle) st22_rx_free(rx->handle);
    free(rx);
}

void mtl_rx_mp4_get_stats(const mtl_rx_mp4* rx, uint64_t* received, uint32_t* drops,
                          uint32_t* corrupt, uint32_t* incomplete) {
    if (!rx)
        return;
    if (received)
        *received = rx->stat_received;
    if (drops)
        *drops = rx->stat_drops;
    if (corrupt)
        *corrupt = rx->stat_corrupt;
    if (incomplete)
        *incomplete = rx->stat_incomplete;
}
