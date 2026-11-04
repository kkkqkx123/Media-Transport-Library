#define _GNU_SOURCE

#include "mtl_rx_bridge.h"
#include "mtl_rx.h"
#include "ring_buffer.h"
#include "pipeline_common.h"
#include "slab_pool.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <st_api.h>

struct mtl_rx_bridge {
    struct mtl_rx_bridge_params cfg;
    pthread_t                   thd;
    atomic_bool                 running;
    uint64_t                    stat_bridged;
    uint32_t                    stat_drops;
    uint64_t                    stat_bytes; /* sum of copied codestream bytes */
    /* split drops (bridge thread writes, poll reads) */
    uint64_t                    stat_bad_drops;
    uint64_t                    stat_slab_drops;
    uint64_t                    stat_malloc_drops;
    uint64_t                    stat_ring_drops;
    uint64_t                    stat_copy_ema_us;
    /* poll-based fps window */
    uint32_t                    poll_fps;
    uint64_t                    poll_last_ns;
    uint64_t                    poll_last_bridged;
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

static inline uint64_t bridge_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void* bridge_thread(void* arg) {
    struct mtl_rx_bridge* b = (struct mtl_rx_bridge*)arg;

    while (atomic_load(&b->running)) {
        struct rx_pkt* mp = NULL;
        if (ring_pop_wait(b->cfg.mtl_ring, (void**)&mp, &b->running) != 0)
            continue;
        if (!mp || !mp->frame || !mp->size) {
            bridge_release_rx_pkt(b, mp);
            b->stat_drops++;
            b->stat_bad_drops++;
            continue;
        }

        uint8_t* copy = slab_alloc(b->cfg.coded_pool);
        if (!copy) {
            bridge_release_rx_pkt(b, mp);
            b->stat_drops++;
            b->stat_slab_drops++;
            continue;
        }
        uint64_t t0 = bridge_now_ns();
        memcpy(copy, mp->frame, mp->size);
        uint64_t sample_us = (bridge_now_ns() - t0) / 1000;
        if (b->stat_copy_ema_us == 0)
            b->stat_copy_ema_us = sample_us;
        else
            b->stat_copy_ema_us += (sample_us - b->stat_copy_ema_us) / 8;
        size_t sz = mp->size;
        bridge_release_rx_pkt(b, mp);

        struct codec_packet* pkt = calloc(1, sizeof(*pkt));
        if (!pkt) {
            slab_free(b->cfg.coded_pool, copy);
            b->stat_drops++;
            b->stat_malloc_drops++;
            continue;
        }
        pkt->data = copy;
        pkt->size = sz;
        pkt->mtl_owner = NULL;

        if (ring_push(b->cfg.dec_ring, pkt) != 0) {
            bridge_free_codec_pkt(b, pkt);
            b->stat_drops++;
            b->stat_ring_drops++;
            continue;
        }
        b->stat_bridged++;
        b->stat_bytes += sz;
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
    printf("mtl_rx_bridge: %llu bridged, %u drops (bad=%llu slab=%llu malloc=%llu ring=%llu) copy_ema=%lluus\n",
           (unsigned long long)b->stat_bridged, b->stat_drops,
           (unsigned long long)b->stat_bad_drops,
           (unsigned long long)b->stat_slab_drops,
           (unsigned long long)b->stat_malloc_drops,
           (unsigned long long)b->stat_ring_drops,
           (unsigned long long)b->stat_copy_ema_us);
}

void mtl_rx_bridge_stats(struct mtl_rx_bridge* b,
                         struct mtl_rx_bridge_stats* out) {
    if (!b || !out) return;
    uint64_t now = bridge_now_ns();
    uint32_t fps = 0;
    if (b->poll_last_ns != 0 && now > b->poll_last_ns) {
        uint64_t dt = now - b->poll_last_ns;
        uint64_t dn = b->stat_bridged - b->poll_last_bridged;
        if (dt > 0) fps = (uint32_t)((dn * 1000000000ULL) / dt);
    }
    b->poll_last_ns = now;
    b->poll_last_bridged = b->stat_bridged;
    b->poll_fps = fps;
    out->bridged = b->stat_bridged;
    out->drops   = b->stat_drops;
    out->bytes   = b->stat_bytes;
    out->bad_drops = b->stat_bad_drops;
    out->slab_drops = b->stat_slab_drops;
    out->malloc_drops = b->stat_malloc_drops;
    out->ring_drops = b->stat_ring_drops;
    out->fps = b->poll_fps;
    out->copy_ema_us = b->stat_copy_ema_us;
}
