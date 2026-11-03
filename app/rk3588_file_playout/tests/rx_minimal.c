/* Minimal RX-only MTL ST22 receiver — validates data path */
#include <mtl_api.h>
#include <st20_api.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>

static atomic_ullong g_recv = 0;
static atomic_ullong g_bytes = 0;

static int notify_frame_ready(void* priv, void* frame,
                               struct st22_rx_frame_meta* meta) {
    atomic_fetch_add(&g_recv, 1);
    atomic_fetch_add(&g_bytes, meta->frame_total_size);
    st22_rx_put_framebuff((st22_rx_handle)priv, frame);
    return 0;
}

int main(void) {
    struct mtl_init_params mtp = {0};
    mtp.num_ports = 1;
    strcpy(mtp.port[MTL_PORT_P], "native_af_xdp:enP3p49s0");
    mtp.pmd[MTL_PORT_P] = MTL_PMD_NATIVE_AF_XDP;
    mtp.net_proto[MTL_PORT_P] = MTL_PROTO_STATIC;
    strcpy((char*)mtp.sip_addr[MTL_PORT_P], "192.168.80.213");
    mtp.tx_queues_cnt[MTL_PORT_P] = 0;
    mtp.rx_queues_cnt[MTL_PORT_P] = 1;
    mtp.flags |= MTL_FLAG_BIND_NUMA | MTL_FLAG_PTP_ENABLE;

    printf("[RX] Initializing MTL...\n");
    mtl_handle st = mtl_init(&mtp);
    if (!st) { printf("[RX] FAIL: mtl_init\n"); return 1; }
    mtl_start(st);
    printf("[RX] MTL started on enP3p49s0:192.168.80.213\n");

    struct st22_rx_ops ops = {0};
    ops.name = "rx_minimal";
    ops.type = ST22_TYPE_FRAME_LEVEL;
    ops.priv = st;  /* pass handle for put_framebuff */
    ops.fps = st_frame_rate_to_st_fps(30.0);
    ops.width = 1920; ops.height = 1080;
    ops.fmt = ST20_FMT_YUV_420_8BIT;
    ops.pack_type = ST22_PACK_CODESTREAM;
    ops.num_port = 1;
    ops.framebuff_cnt = 8;
    ops.framebuff_max_size = 512 * 1024;
    ops.notify_frame_ready = notify_frame_ready;
    ops.payload_type = 112;
    ops.flags = ST22_RX_FLAG_DISABLE_BOXES;
    ops.udp_port[0] = 20000;
    ops.ip_addr[0][0]=192; ops.ip_addr[0][1]=168; ops.ip_addr[0][2]=80; ops.ip_addr[0][3]=212;
    strcpy(ops.port[0], "native_af_xdp:enP3p49s0");

    st22_rx_handle rx = st22_rx_create(st, &ops);
    if (!rx) { printf("[RX] FAIL: st22_rx_create\n"); return 1; }
    printf("[RX] ST22 session ready, listening on UDP 20000\n");

    for (int i = 1; i <= 15; i++) {
        sleep(1);
        unsigned long long r = atomic_load(&g_recv);
        unsigned long long b = atomic_load(&g_bytes);
        printf("[RX] t=%ds  frames=%llu  MB=%.2f\n", i, r, b / 1e6);
    }

    st22_rx_free(rx);
    mtl_stop(st);
    mtl_uninit(st);
    printf("[RX] Done — %llu frames, %.2f MB\n",
           (unsigned long long)atomic_load(&g_recv),
           atomic_load(&g_bytes) / 1e6);
    return 0;
}
