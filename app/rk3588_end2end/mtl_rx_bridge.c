#define _GNU_SOURCE
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "mtl_rx_bridge.h"
#include "mtl_rx.h"
#include "pipeline/ring_buffer.h"
#include "pipeline/pipeline_common.h"
#include "../hal/slab_pool.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <st_api.h>

struct mtl_rx_bridge {
    struct mtl_rx_bridge_params cfg;
    pthread_t                   thd;
    atomic_bool                 running;
    uint64_t                    stat_bridged;
    uint32_t                    stat_drops;
};

static void bridge_release_rx_pkt(struct mtl_rx_bridge* b, struct rx_pkt* mp) {
    if (!mp) return;
    if (b->cfg.rx_handle && mp->frame) {
        st22_rx_put_framebuff(b->cfg.rx_handle, mp->frame);
        mp->frame = NULL;
    }
    atomic_store(&mp->used, 0);
}

static void bridge_free_codec_pkt(struct mtl_rx_bridge* b, struct codec_packet* pkt) {
    if (!pkt) return;
    if (pkt->mtl_owner)
        bridge_release_rx_pkt(b, (struct rx_pkt*)pkt->mtl_owner);
    if (pkt->data && b->cfg.coded_pool)
        slab_free(b->cfg.coded_pool, pkt->data);
    free(pkt);
}

static void bridge_drain_rings(struct mtl_rx_bridge* b) {
    if (!b) return;

    struct codec_packet* pkt;
    while (ring_pop(b->cfg.dec_ring, (void**)&pkt) == 0)
        bridge_free_codec_pkt(b, pkt);

    struct rx_pkt* mp;
    while (ring_pop(b->cfg.mtl_ring, (void**)&mp) == 0)
        bridge_release_rx_pkt(b, mp);
}

static void* bridge_thread(void* arg) {
    struct mtl_rx_bridge* b = (struct mtl_rx_bridge*)arg;

    while (atomic_load(&b->running)) {
        struct rx_pkt* mp = NULL;
        if (ring_pop_wait(b->cfg.mtl_ring, (void**)&mp, &b->running) != 0)
            continue;
        if (!mp || !mp->frame || !mp->size) {
            bridge_release_rx_pkt(b, mp);
            continue;
        }

        uint8_t* copy = slab_alloc(b->cfg.coded_pool);
        if (!copy) {
            bridge_release_rx_pkt(b, mp);
            b->stat_drops++;
            continue;
        }
        memcpy(copy, mp->frame, mp->size);
        size_t sz = mp->size;
        bridge_release_rx_pkt(b, mp);

        struct codec_packet* pkt = calloc(1, sizeof(*pkt));
        if (!pkt) {
            slab_free(b->cfg.coded_pool, copy);
            b->stat_drops++;
            continue;
        }
        pkt->data = copy;
        pkt->size = sz;
        pkt->mtl_owner = NULL;

        if (ring_push(b->cfg.dec_ring, pkt) != 0) {
            bridge_free_codec_pkt(b, pkt);
            b->stat_drops++;
            continue;
        }
        b->stat_bridged++;
    }
    return NULL;
}

struct mtl_rx_bridge* mtl_rx_bridge_create(struct mtl_rx_bridge_params* p) {
    struct mtl_rx_bridge* b = calloc(1, sizeof(*b));
    if (!b || !p || !p->mtl_ring || !p->dec_ring || !p->coded_pool) {
        free(b);
        return NULL;
    }
    b->cfg = *p;
    printf("mtl_rx_bridge: ready (mtl_ring → slab copy → dec_ring)\n");
    return b;
}

void mtl_rx_bridge_free(struct mtl_rx_bridge* b) {
    if (!b) return;
    mtl_rx_bridge_stop(b);
    free(b);
}

int mtl_rx_bridge_start(struct mtl_rx_bridge* b) {
    if (!b) return -1;
    atomic_store(&b->running, true);
    if (pthread_create(&b->thd, NULL, bridge_thread, b) != 0) {
        atomic_store(&b->running, false);
        return -1;
    }
    if (b->cfg.cpu >= 0) {
        cpu_set_t cset;
        CPU_ZERO(&cset);
        CPU_SET(b->cfg.cpu, &cset);
        pthread_setaffinity_np(b->thd, sizeof(cset), &cset);
    }
    return 0;
}

void mtl_rx_bridge_stop(struct mtl_rx_bridge* b) {
    if (!b) return;
    if (!atomic_exchange(&b->running, false)) return;
    if (b->cfg.mtl_ring) ring_wake_all(b->cfg.mtl_ring);
    if (b->cfg.coded_pool) slab_pool_stop(b->cfg.coded_pool);
    if (b->thd) {
        pthread_join(b->thd, NULL);
        b->thd = 0;
    }
    bridge_drain_rings(b);
    printf("mtl_rx_bridge: %llu bridged, %u drops\n",
           (unsigned long long)b->stat_bridged, b->stat_drops);
}
