#include "mtl_rx_bridge.h"
#include "mtl_rx_mp4.h"
#include "ring_buffer.h"
#include "slab_pool.h"
#include "pipeline_common.h"
#include "au_trace.h"
#include "sei_timestamp.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <st_api.h>

/* Sliding window for per-packet bridge work cost (us). */
#define BRIDGE_WORK_WIN 128

struct mtl_rx_bridge {
    mtl_rx_bridge_params cfg;
    pthread_t            thd;
    atomic_bool          running;
    uint64_t             stat_bridged;
    uint64_t             stat_stamp_ok;
    uint32_t             stat_drops;
    /* Contention observability, single writer (bridge thread):
     * work = memcpy+SEI+push cost, wait = blocked time in ring_pop_wait,
     * ring_max = max mtl_ring depth observed. Shared-core contention shows
     * as work_ema inflation + wait_ema collapse + ring_max growth. */
    uint64_t             stat_work_us_ema;
    uint64_t             stat_wait_us_ema;
    uint32_t             stat_work_win[BRIDGE_WORK_WIN];
    int                  stat_work_count;
    int                  stat_ring_max;
    FILE*                dump_h264;
    FILE*                dump_idx;
    uint64_t             dump_bytes;
};

static void bridge_release_rx_pkt(mtl_rx_bridge* b, struct file_playout_rx_pkt* mp) {
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
    const char* path = getenv("FILE_PLAYOUT_DUMP_RX");
    if (!path || !path[0]) return;

    b->dump_h264 = fopen(path, "wb");
    if (!b->dump_h264) {
        fprintf(stderr, "mtl_rx_bridge: cannot open dump %s\n", path);
        return;
    }

    const char* idx = getenv("FILE_PLAYOUT_DUMP_RX_IDX");
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

static uint64_t bridge_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void bridge_note_timing(mtl_rx_bridge* b, uint64_t t_wait0,
                               uint64_t t_work0, uint64_t t_done) {
    uint64_t wait_us = t_work0 > t_wait0 ? (t_work0 - t_wait0) / 1000ULL : 0;
    uint64_t work_us = t_done > t_work0 ? (t_done - t_work0) / 1000ULL : 0;
    /* integer EMA x9/10, same shape as mpp_encoder encode_cost */
    b->stat_wait_us_ema = b->stat_wait_us_ema
        ? (b->stat_wait_us_ema * 9 + wait_us + 5) / 10
        : wait_us;
    b->stat_work_us_ema = b->stat_work_us_ema
        ? (b->stat_work_us_ema * 9 + work_us + 5) / 10
        : work_us;
    if (b->stat_work_count < BRIDGE_WORK_WIN) {
        b->stat_work_win[b->stat_work_count++] = (uint32_t)work_us;
    } else {
        memmove(&b->stat_work_win[0], &b->stat_work_win[1],
                (size_t)(BRIDGE_WORK_WIN - 1) * sizeof(uint32_t));
        b->stat_work_win[BRIDGE_WORK_WIN - 1] = (uint32_t)work_us;
    }
    if (b->cfg.mtl_ring) {
        int depth = atomic_load(&b->cfg.mtl_ring->count);
        if (depth > b->stat_ring_max)
            b->stat_ring_max = depth;
    }
}

static void* bridge_thread(void* arg) {
    mtl_rx_bridge* b = (mtl_rx_bridge*)arg;
#if defined(__linux__) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "rx_bridge");
#endif
    bridge_dump_open(b);

    while (atomic_load(&b->running)) {
        struct file_playout_rx_pkt* mp = NULL;
        uint64_t t_wait0 = bridge_now_ns();
        if (ring_pop_wait(b->cfg.mtl_ring, (void**)&mp, &b->running) != 0)
            continue;
        uint64_t t_work0 = bridge_now_ns();
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

        /* Unified E2E carrier: in-band SEI NAL (same as live_ingest TX). */
        uint32_t seq = 0;
        uint64_t ts = 0;
        size_t sei_sz = sz;
        if ((sei_timestamp_peek(copy, sei_sz, &seq, &ts) &&
             sei_timestamp_extract(copy, &sei_sz, &seq, &ts)) ||
            (sei_timestamp_peek_h265(copy, sei_sz, &seq, &ts) &&
             sei_timestamp_extract_h265(copy, &sei_sz, &seq, &ts))) {
            capture_tai = ts;
            b->stat_stamp_ok++;
            sz = sei_sz;
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
        bridge_note_timing(b, t_wait0, t_work0, bridge_now_ns());
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
    struct file_playout_rx_pkt* mp;
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

void mtl_rx_bridge_get_timing(const mtl_rx_bridge* b, uint32_t* work_ema_us,
                              uint32_t* wait_ema_us, uint32_t* work_p50_us,
                              uint32_t* work_p99_us, int* ring_max_depth) {
    uint32_t tmp[BRIDGE_WORK_WIN];
    int n;
    if (work_ema_us)
        *work_ema_us = 0;
    if (wait_ema_us)
        *wait_ema_us = 0;
    if (work_p50_us)
        *work_p50_us = 0;
    if (work_p99_us)
        *work_p99_us = 0;
    if (ring_max_depth)
        *ring_max_depth = 0;
    if (!b)
        return;
    if (work_ema_us)
        *work_ema_us = (uint32_t)b->stat_work_us_ema;
    if (wait_ema_us)
        *wait_ema_us = (uint32_t)b->stat_wait_us_ema;
    if (ring_max_depth)
        *ring_max_depth = b->stat_ring_max;
    /* Benign race: single writer (bridge thread), torn window reads only
     * skew one stats sample. */
    n = b->stat_work_count;
    if (n <= 0)
        return;
    if (n > BRIDGE_WORK_WIN)
        n = BRIDGE_WORK_WIN;
    memcpy(tmp, b->stat_work_win, (size_t)n * sizeof(uint32_t));
    for (int i = 1; i < n; i++) {
        uint32_t v = tmp[i];
        int j = i;
        while (j > 0 && tmp[j - 1] > v) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = v;
    }
    if (work_p50_us)
        *work_p50_us = tmp[(n * 50) / 100 >= n ? n - 1 : (n * 50) / 100];
    if (work_p99_us)
        *work_p99_us = tmp[(n * 99) / 100 >= n ? n - 1 : (n * 99) / 100];
}
