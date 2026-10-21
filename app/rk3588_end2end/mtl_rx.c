/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * MTL ST22 RX wrapper — see mtl_rx.h for architecture.
 *
 * notify_frame_ready callback runs in MTL lcore tasklet (non-blocking).
 * Uses a pre-allocated packet pool to avoid malloc in the hot path.
 */

#include "mtl_rx.h"
#include "pipeline/ring_buffer.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <st_api.h>

#define RX_POOL_SIZE  16  /* rx_pkt descriptors; must be >= framebuff_cnt + ring depth */

struct mtl_rx {
    struct mtl_rx_params  cfg;
    st22_rx_handle        handle;

    struct rx_pkt         pool[RX_POOL_SIZE];
    atomic_bool           stopping;

    uint64_t              stat_received;
    uint32_t              stat_drops;
};

static struct rx_pkt* rx_pool_acquire(struct mtl_rx* rx) {
    for (int i = 0; i < RX_POOL_SIZE; i++) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&rx->pool[i].used, &expected, 1))
            return &rx->pool[i];
    }
    return NULL;
}

static void rx_pool_release(struct rx_pkt* pkt) {
    if (!pkt) return;
    atomic_store(&pkt->used, 0);
}

/* lcore callback: receive frame → push to ring (non-blocking).
 * ST22 RX uses st22_rx_frame_meta (not st20_rx_frame_meta). */
static int rx_notify_frame_ready(void* priv, void* frame,
                                 struct st22_rx_frame_meta* meta) {
    struct mtl_rx* rx = (struct mtl_rx*)priv;

    if (atomic_load(&rx->stopping)) {
        st22_rx_put_framebuff(rx->handle, frame);
        return 0;
    }

    struct rx_pkt* pkt = rx_pool_acquire(rx);
    if (!pkt) {
        rx->stat_drops++;
        st22_rx_put_framebuff(rx->handle, frame);
        return -ENOSPC;
    }

    pkt->frame = frame;
    pkt->size  = meta->frame_total_size;

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

struct mtl_rx* mtl_rx_create(struct mtl_rx_params* p) {
    struct mtl_rx* rx = calloc(1, sizeof(*rx));
    if (!rx) return NULL;
    rx->cfg = *p;

    for (int i = 0; i < RX_POOL_SIZE; i++)
        atomic_init(&rx->pool[i].used, 0);
    atomic_init(&rx->stopping, false);

    struct st22_rx_ops ops = {0};
    ops.name               = "e2e_rx";
    ops.priv               = rx;
    ops.type               = ST22_TYPE_FRAME_LEVEL;
    ops.fps                = st_frame_rate_to_st_fps((double)p->fps);
    ops.width              = (uint32_t)(p->width > 0 ? p->width : 1920);
    ops.height             = (uint32_t)(p->height > 0 ? p->height : 1080);
    ops.fmt                = ST20_FMT_YUV_420_8BIT;
    ops.pack_type          = ST22_PACK_CODESTREAM;
    ops.num_port           = 1;
    ops.framebuff_cnt       = (uint16_t)p->framebuff_cnt;
    ops.framebuff_max_size  = 512 * 1024;
    ops.notify_frame_ready  = rx_notify_frame_ready;
    ops.payload_type       = 112;
    ops.flags |= ST22_RX_FLAG_DISABLE_BOXES;
    ops.udp_port[0]       = p->udp_port;
    if (p->sip_addr) {
        unsigned int a, b, c, d;
        if (sscanf(p->sip_addr, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            ops.ip_addr[0][0] = (uint8_t)a;
            ops.ip_addr[0][1] = (uint8_t)b;
            ops.ip_addr[0][2] = (uint8_t)c;
            ops.ip_addr[0][3] = (uint8_t)d;
        }
    }
    strncpy(ops.port[0], p->port_name, MTL_PORT_MAX_LEN - 1);

    rx->handle = st22_rx_create(p->st, &ops);
    if (!rx->handle) { fprintf(stderr, "mtl_rx: st22_rx_create failed\n"); free(rx); return NULL; }

    printf("mtl_rx: %d fbs, %.0ffps, port %u ready\n",
           p->framebuff_cnt, (double)p->fps, p->udp_port);
    return rx;
}

void mtl_rx_free(struct mtl_rx* rx) {
    if (!rx) return;
    mtl_rx_stop(rx);
    if (rx->handle) st22_rx_free(rx->handle);
    free(rx);
}

int mtl_rx_start(struct mtl_rx* rx) { (void)rx; return 0; }
st22_rx_handle mtl_rx_handle(struct mtl_rx* rx) {
    return rx ? rx->handle : NULL;
}

void mtl_rx_begin_stop(struct mtl_rx* rx) {
    if (!rx) return;
    atomic_store(&rx->stopping, true);
}

void mtl_rx_stop(struct mtl_rx* rx) {
    if (!rx) return;
    for (int i = 0; i < RX_POOL_SIZE; i++) {
        if (!atomic_load(&rx->pool[i].used)) continue;
        if (rx->pool[i].frame)
            st22_rx_put_framebuff(rx->handle, rx->pool[i].frame);
        rx->pool[i].frame = NULL;
        rx_pool_release(&rx->pool[i]);
    }
    struct rx_pkt* pkt;
    while (ring_pop(rx->cfg.output_ring, (void**)&pkt) == 0) {
        st22_rx_put_framebuff(rx->handle, pkt->frame);
        rx_pool_release(pkt);
    }
    printf("mtl_rx: %llu received, %u drops\n",
           (unsigned long long)rx->stat_received, rx->stat_drops);
}
