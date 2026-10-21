/* SPDX-License-Identifier: BSD-3-Clause
 * Phase 4: Pipeline integration test — local loopback with latency measurement.
 *
 * Thread model:
 *   Core 1: V4L2 Capture
 *   Core 2: RGA + MPP Encode + SEI inject
 *   Core 3: SEI extract + MPP Decode + DRM Render
 *
 * Rings:
 *   CAP2ENC (dmabuf_frame, size=3): Capture → Encode
 *   ENC2DEC (codec_packet, size=3): Encode → Decode+Render
 *
 * Usage: ./test_pipeline [seconds]
 */

#include "../pipeline/tx_pipeline.h"
#include "../pipeline/rx_pipeline.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static atomic_bool g_run = true;
static void sig_handler(int s) { (void)s; atomic_store(&g_run, false); }

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    int test_time = (argc > 1) ? atoi(argv[1]) : 10;
    int w = 1920, h = 1080;

    printf("=== Phase 4 Pipeline Test ===\n");

    /* TX pipeline */
    struct tx_pipeline_params txp = {
        .v4l2_device   = "/dev/video0",
        .width         = w, .height = h,
        .fps           = 60.0f,
        .v4l2_buf_count = 6,
        .drm_device    = "/dev/dri/card0",
        .rga_pool_size = 4,
        .bitrate_bps   = 40000000,
        .gop_size      = 60,
        .num_slices    = 1,
        .coded_blk_size = 512 * 1024,
        .ring_size     = 3,
        .cpu_capture   = 1,
        .cpu_encode    = 2,
    };
    struct tx_pipeline* tx = tx_pipeline_create(&txp);
    if (!tx) { fprintf(stderr, "FAIL: tx_pipeline_create\n"); return 1; }

    /* RX pipeline */
    struct rx_pipeline_params rxp = {
        .drm_device    = "/dev/dri/card0",
        .drm_connector = "HDMI-A-1",
        .drm_mode      = "1920x1080@60",
        .width         = w, .height = h,
        .drm_buf_count = 4,
        .num_slices    = 1,
        .input_ring    = tx_pipeline_get_output_ring(tx),
        .coded_pool    = tx_pipeline_get_slab_pool(tx),
        .cpu_decode    = 3,
    };
    struct rx_pipeline* rx = rx_pipeline_create(&rxp);
    if (!rx) { tx_pipeline_free(tx); fprintf(stderr, "FAIL: rx_pipeline_create\n"); return 1; }

    printf("Starting pipeline for %ds...\n", test_time);
    if (tx_pipeline_start(tx) != 0) { rx_pipeline_free(rx); tx_pipeline_free(tx); return 1; }
    if (rx_pipeline_start(rx) != 0) { tx_pipeline_stop(tx); rx_pipeline_free(rx); tx_pipeline_free(tx); return 1; }

    /* run */
    while (atomic_load(&g_run) && test_time > 0) {
        sleep(1);
        test_time--;
    }

    /* Note: rx_pipeline_free/tx_pipeline_free internally call stop(),
     * so no need for explicit stop() — avoids double pthread_join. */
    uint64_t min_ns, avg_ns, max_ns;
    rx_pipeline_get_latency(rx, &min_ns, &avg_ns, &max_ns);

    rx_pipeline_free(rx);
    tx_pipeline_free(tx);

    printf("\n=== E2E Latency: min=%llu avg=%llu max=%llu us ===\n",
           (unsigned long long)(min_ns / 1000),
           (unsigned long long)(avg_ns / 1000),
           (unsigned long long)(max_ns / 1000));
    printf("=== PASS ===\n");
    return 0;
}
