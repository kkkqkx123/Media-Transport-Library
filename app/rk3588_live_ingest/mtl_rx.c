#include "mtl_rx.h"
#include "ring_buffer.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <st_api.h>

#define RX_POOL_SIZE  16  /* rx_pkt descriptors; must be >= framebuff_cnt + ring depth */
#define RX_FULL_WARN_NS (5000000000ULL) /* pool-full warn cooldown: 5s */

static inline uint64_t rx_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct mtl_rx {
    struct mtl_rx_params  cfg;
    st22_rx_handle        handle;

    struct rx_pkt         pool[RX_POOL_SIZE];
    atomic_bool           stopping;

    uint64_t              stat_received;
    uint32_t              stat_drops;
    uint64_t              stat_bytes;      /* sum of frame_total_size */
    uint64_t              stat_pool_full;  /* pool-exhausted acquire fails */
    int                   stat_peak_inflight;
    uint64_t              last_full_warn_ns; /* lcore-side 5s warn cooldown */
    /* split drops + windowed observability (lcore writes, poll reads) */
    uint64_t              stat_ring_drops;
    uint64_t              stat_stopping_drops;
    size_t                stat_frame_max;
    uint32_t              poll_fps;
    uint64_t              poll_last_ns;
    uint64_t              poll_last_received;
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

/* Count descriptors currently checked out (lcore + bridge/pipeline owned).
 * O(POOL) scan; called once per frame and once per stats poll — trivial. */
static int rx_inflight(struct mtl_rx* rx) {
    int n = 0;
    for (int i = 0; i < RX_POOL_SIZE; i++)
        n += atomic_load(&rx->pool[i].used);
    return n;
}

/* lcore callback: receive frame → push to ring (non-blocking).
 * ST22 RX uses st22_rx_frame_meta (not st20_rx_frame_meta). */
static int rx_notify_frame_ready(void* priv, void* frame,
                                 struct st22_rx_frame_meta* meta) {
    struct mtl_rx* rx = (struct mtl_rx*)priv;

    if (atomic_load(&rx->stopping)) {
        rx->stat_stopping_drops++;
        st22_rx_put_framebuff(rx->handle, frame);
        return 0;
    }

    struct rx_pkt* pkt = rx_pool_acquire(rx);
    if (!pkt) {
        rx->stat_drops++;
        rx->stat_pool_full++;
        /* 5s-cooled warn: a persistent storm means the downstream (bridge
         * or decode) no longer keeps up; each event is still counted. */
        uint64_t now = rx_now_ns();
        if (now - rx->last_full_warn_ns >= RX_FULL_WARN_NS) {
            rx->last_full_warn_ns = now;
            fprintf(stderr, "mtl_rx: descriptor pool exhausted "
                    "(inflight=%d peak=%d, total pool-full=%llu) — "
                    "downstream backpressure\n",
                    rx_inflight(rx), rx->stat_peak_inflight,
                    (unsigned long long)rx->stat_pool_full);
        }
        st22_rx_put_framebuff(rx->handle, frame);
        return -ENOSPC;
    }

    pkt->frame = frame;
    pkt->size  = meta->frame_total_size;

    int live = rx_inflight(rx);
    if (live > rx->stat_peak_inflight) rx->stat_peak_inflight = live;

    if (ring_push(rx->cfg.output_ring, pkt) == -ENOSPC) {
        rx_pool_release(pkt);
        rx->stat_drops++;
        rx->stat_ring_drops++;
        st22_rx_put_framebuff(rx->handle, frame);
        return -ENOSPC;
    }

    rx->stat_received++;
    rx->stat_bytes += meta->frame_total_size;
    if (meta->frame_total_size > rx->stat_frame_max)
        rx->stat_frame_max = meta->frame_total_size;
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
    ops.name               = "live_ingest_rx";
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
        if (sscanf(p->sip_addr, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 ||
            a > 255 || b > 255 || c > 255 || d > 255) {
            fprintf(stderr, "mtl_rx: invalid sip_addr\n");
            free(rx);
            return NULL;
        }
        ops.ip_addr[0][0] = (uint8_t)a;
        ops.ip_addr[0][1] = (uint8_t)b;
        ops.ip_addr[0][2] = (uint8_t)c;
        ops.ip_addr[0][3] = (uint8_t)d;
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
    printf("mtl_rx: %llu received, %u drops (ring=%llu stop=%llu), %llu pool-full (peak inflight %d) max=%zu\n",
           (unsigned long long)rx->stat_received, rx->stat_drops,
           (unsigned long long)rx->stat_ring_drops,
           (unsigned long long)rx->stat_stopping_drops,
           (unsigned long long)rx->stat_pool_full, rx->stat_peak_inflight,
           rx->stat_frame_max);
}

void mtl_rx_stats(struct mtl_rx* rx, struct mtl_rx_stats* out) {
    if (!rx || !out) return;
    /* windowed fps from poll deltas: no per-frame clock on the lcore. */
    uint64_t now = rx_now_ns();
    uint32_t fps = 0;
    if (rx->poll_last_ns != 0 && now > rx->poll_last_ns) {
        uint64_t dt_ns = now - rx->poll_last_ns;
        uint64_t dn = rx->stat_received - rx->poll_last_received;
        if (dt_ns > 0)
            fps = (uint32_t)((dn * 1000000000ULL) / dt_ns);
    }
    rx->poll_last_ns = now;
    rx->poll_last_received = rx->stat_received;
    rx->poll_fps = fps;
    out->received  = rx->stat_received;
    out->drops     = rx->stat_drops;
    out->bytes     = rx->stat_bytes;
    out->pool_full = rx->stat_pool_full;
    out->peak      = rx->stat_peak_inflight;
    out->inflight  = rx_inflight(rx);
    out->ring_drops = rx->stat_ring_drops;
    out->stopping_drops = rx->stat_stopping_drops;
    out->fps       = rx->poll_fps;
    out->frame_max_size = rx->stat_frame_max;
}
