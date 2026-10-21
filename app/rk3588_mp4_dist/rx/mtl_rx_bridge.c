/* SPDX-License-Identifier: BSD-3-Clause
 *
 * MTL RX Bridge — copies codestream from MTL framebuffers to slab pool,
 * pushes codec_packet to decode ring. Dedicated pthread.
 *
 * Adapted from E2E mtl_rx_bridge.c.
 */

#include "mtl_rx_bridge.h"
#include "mtl_rx_mp4.h"
#include "../shared/ring_buffer.h"
#include "../shared/slab_pool.h"
#include "../shared/pipeline_common.h"
#include "../shared/au_trace.h"
#include "../shared/e2e_stamp.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <st_api.h>

struct mtl_rx_bridge {
    mtl_rx_bridge_params cfg;
    pthread_t            thd;
    atomic_bool          running;
    uint64_t             stat_bridged;
    uint64_t             stat_stamp_ok;
    uint32_t             stat_drops;
    FILE*                dump_h264;
    FILE*                dump_idx;
    uint64_t             dump_bytes;
};

static void bridge_release_rx_pkt(mtl_rx_bridge* b, struct rx_pkt* mp) {
    if (!mp) return;
    if (b->cfg.rx_handle && mp->frame) {
        st22_rx_put_framebuff(b->cfg.rx_handle, mp->frame);
    }
    mp->frame     = NULL;
    mp->size      = 0;
    mp->tai_ns    = 0;
    mp->media_clk = 0;
    atomic_store(&mp->used, 0);
}

static void bridge_dump_open(mtl_rx_bridge* b) {
    const char* path = getenv("MP4_DIST_DUMP_RX");
    if (!path || !path[0]) return;

    b->dump_h264 = fopen(path, "wb");
    if (!b->dump_h264) {
        fprintf(stderr, "mtl_rx_bridge: cannot open dump %s\n", path);
        return;
    }

    const char* idx = getenv("MP4_DIST_DUMP_RX_IDX");
    if (idx && idx[0]) {
        b->dump_idx = fopen(idx, "w");
        if (!b->dump_idx)
            fprintf(stderr, "mtl_rx_bridge: cannot open idx %s\n", idx);
    }

    printf("mtl_rx_bridge: dumping ST22 packets -> %s", path);
    if (b->dump_idx) printf(" (idx %s)", idx);
    printf("\n");
}

static void bridge_dump_pkt(mtl_rx_bridge* b, const uint8_t* data, size_t size,
                            uint64_t tai_ns) {
    if (!b->dump_h264 || !data || !size) return;

    if (fwrite(data, 1, size, b->dump_h264) != size) {
        fprintf(stderr, "mtl_rx_bridge: dump write failed\n");
        return;
    }
    b->dump_bytes += size;

    if (b->dump_idx)
        fprintf(b->dump_idx, "%zu %llu\n", size,
                (unsigned long long)tai_ns);
}

static void bridge_dump_close(mtl_rx_bridge* b) {
    if (b->dump_idx) {
        fclose(b->dump_idx);
        b->dump_idx = NULL;
    }
    if (b->dump_h264) {
        fclose(b->dump_h264);
        printf("mtl_rx_bridge: dump closed, %llu pkts %llu bytes\n",
               (unsigned long long)b->stat_bridged,
               (unsigned long long)b->dump_bytes);
        b->dump_h264 = NULL;
    }
}

static void* bridge_thread(void* arg) {
    mtl_rx_bridge* b = (mtl_rx_bridge*)arg;
    bridge_dump_open(b);

    while (atomic_load(&b->running)) {
        struct rx_pkt* mp = NULL;
        if (ring_pop_wait(b->cfg.mtl_ring, (void**)&mp, &b->running) != 0)
            continue;
        if (!mp || !mp->frame || !mp->size) {
            bridge_release_rx_pkt(b, mp);
            continue;
        }

        uint8_t* copy = slab_alloc(b->cfg.coded_pool);
        size_t   blk  = slab_blk_size(b->cfg.coded_pool);
        if (!copy || mp->size > blk) {
            if (mp->size > blk)
                fprintf(stderr, "mtl_rx_bridge: frame %zu > slab %zu, drop\n",
                        mp->size, blk);
            bridge_release_rx_pkt(b, mp);
            b->stat_drops++;
            continue;
        }
        memcpy(copy, mp->frame, mp->size);
        size_t     sz            = mp->size;
        uint64_t   tai_ns        = mp->tai_ns;
        uint64_t   capture_tai   = 0;
        size_t     payload_off     = 0;
        size_t     payload_sz      = sz;

        if (e2e_stamp_try_parse(copy, sz, &capture_tai, &payload_off, &payload_sz)) {
            b->stat_stamp_ok++;
            if (payload_off > 0)
                memmove(copy, copy + payload_off, payload_sz);
            sz = payload_sz;
        }

        au_trace_record(AU_TRACE_RX_BRIDGE, copy, sz, (int64_t)tai_ns);
        bridge_dump_pkt(b, copy, sz, tai_ns);
        bridge_release_rx_pkt(b, mp);

        struct codec_packet* pkt = calloc(1, sizeof(*pkt));
        if (!pkt) {
            slab_free(b->cfg.coded_pool, copy);
            b->stat_drops++;
            continue;
        }
        pkt->data           = copy;
        pkt->size           = sz;
        pkt->pts            = tai_ns;
        pkt->capture_tai_ns = capture_tai;
        pkt->owner = NULL;

        if (ring_push(b->cfg.dec_ring, pkt) != 0) {
            slab_free(b->cfg.coded_pool, copy);
            free(pkt);
            b->stat_drops++;
            continue;
        }
        b->stat_bridged++;
    }

    bridge_dump_close(b);
    return NULL;
}

mtl_rx_bridge* mtl_rx_bridge_create(const mtl_rx_bridge_params* params) {
    mtl_rx_bridge* b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->cfg = *params;
    printf("mtl_rx_bridge: ready (mtl_ring→slab→dec_ring)\n");
    return b;
}

void mtl_rx_bridge_free(mtl_rx_bridge* b) {
    if (!b) return;
    mtl_rx_bridge_stop(b);
    free(b);
}

int mtl_rx_bridge_start(mtl_rx_bridge* b) {
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

void mtl_rx_bridge_stop(mtl_rx_bridge* b) {
    if (!b) return;
    if (!atomic_exchange(&b->running, false)) return;
    if (b->cfg.mtl_ring) ring_wake_all(b->cfg.mtl_ring);
    if (b->cfg.coded_pool) slab_pool_stop(b->cfg.coded_pool);
    if (b->thd) { pthread_join(b->thd, NULL); b->thd = 0; }
    /* drain rings */
    struct codec_packet* pkt;
    while (ring_pop(b->cfg.dec_ring, (void**)&pkt) == 0) {
        if (b->cfg.coded_pool) slab_free(b->cfg.coded_pool, pkt->data);
        free(pkt);
    }
    struct rx_pkt* mp;
    while (ring_pop(b->cfg.mtl_ring, (void**)&mp) == 0)
        bridge_release_rx_pkt(b, mp);
    printf("mtl_rx_bridge: %llu bridged, %u drops\n",
           (unsigned long long)b->stat_bridged, b->stat_drops);
    au_trace_close();
}

void mtl_rx_bridge_get_stats(const mtl_rx_bridge* b, uint64_t* bridged,
                             uint32_t* drops, uint64_t* stamp_ok) {
    if (!b)
        return;
    if (bridged)
        *bridged = b->stat_bridged;
    if (drops)
        *drops = b->stat_drops;
    if (stamp_ok)
        *stamp_ok = b->stat_stamp_ok;
}
