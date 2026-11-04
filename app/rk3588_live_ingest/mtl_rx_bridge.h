#ifndef _LIVE_INGEST_MTL_RX_BRIDGE_H_
#define _LIVE_INGEST_MTL_RX_BRIDGE_H_

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

/* Snapshot for the 1s aggregation log (safe to call while running). */
struct mtl_rx_bridge_stats {
    uint64_t bridged;
    uint32_t drops;         /* total (compat) */
    uint64_t bytes;
    /* split drops + windowed observability (added, totals kept) */
    uint64_t bad_drops;     /* empty mtl descriptor */
    uint64_t slab_drops;    /* coded slab alloc failed */
    uint64_t malloc_drops;  /* codec_packet header alloc failed */
    uint64_t ring_drops;    /* dec ring full */
    uint32_t fps;           /* windowed rate, computed at stats poll */
    uint64_t copy_ema_us;   /* memcpy cost EMA */
};
void mtl_rx_bridge_stats(struct mtl_rx_bridge* b,
                         struct mtl_rx_bridge_stats* out);

#endif
