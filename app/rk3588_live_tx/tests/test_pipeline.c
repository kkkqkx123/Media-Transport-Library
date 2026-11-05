#include "../pipeline/tx_pipeline.h"
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

    printf("=== TX Pipeline Test (encode-only) ===\n");

    /* TX pipeline without MTL sink: encoded frames land in the output ring. */
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
        .mtl_sink      = NULL,
    };
    struct tx_pipeline* tx = tx_pipeline_create(&txp);
    if (!tx) { fprintf(stderr, "FAIL: tx_pipeline_create\n"); return 1; }

    printf("Starting TX pipeline for %ds...\n", test_time);
    if (tx_pipeline_start(tx) != 0) { tx_pipeline_free(tx); return 1; }

    /* run */
    while (atomic_load(&g_run) && test_time > 0) {
        sleep(1);
        test_time--;
    }

    tx_pipeline_free(tx);

    printf("=== PASS ===\n");
    return 0;
}
