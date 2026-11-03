#ifndef _LIVE_INGEST_MTL_RX_H_
#define _LIVE_INGEST_MTL_RX_H_

#include <mtl_api.h>
#include <stdatomic.h>
#include <st20_api.h>

struct ring_buffer;
struct mtl_rx;

/* pre-allocated rx packet descriptor (lcore-safe, no malloc in callback) */
struct rx_pkt {
    void*      frame;    /* MTL framebuff pointer (codestream data) */
    size_t     size;     /* frame_total_size from meta */
    atomic_int used;     /* 1 = in use, 0 = free (CAS in lcore callback) */
};

struct mtl_rx_params {
    mtl_handle          st;             /* MTL instance */
    struct ring_buffer* output_ring;    /* RX2DEC: received frames → decode thread */
    int                 framebuff_cnt;  /* MTL framebuff count (2-8, ST22_FB_MAX_COUNT) */
    int                 width, height;  /* session resolution */
    float               fps;            /* session fps */
    uint16_t            udp_port;       /* UDP listen port */
    const char*         sip_addr;       /* unicast source IP (TX sender) → ops.ip_addr */
    const char*         port_name;      /* MTL port name */
};

struct mtl_rx* mtl_rx_create(struct mtl_rx_params* p);
void mtl_rx_free(struct mtl_rx* rx);
int  mtl_rx_start(struct mtl_rx* rx);
void mtl_rx_begin_stop(struct mtl_rx* rx); /* stop callbacks; drain via bridge first */
void mtl_rx_stop(struct mtl_rx* rx);
st22_rx_handle mtl_rx_handle(struct mtl_rx* rx);

#endif
