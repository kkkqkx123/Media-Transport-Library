/* SPDX-License-Identifier: BSD-3-Clause
 *
 * MP4 Distribution TX — MP4 file → FFmpeg demux → MTL ST22/ST30.
 *
 * Single stream (legacy): mp4_file / --mp4
 * Multi-session (same NIC): tx_streams[] in JSON — one process, N ST22 sessions
 */

#include "mp4_dist_app.h"
#include "mp4_dist_config.h"
#include "shared/display_layout.h"
#include "tx/tx_scheduler.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_bool g_run = true;

static void sig_handler(int s) {
    (void)s;
    atomic_store(&g_run, false);
}

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);

    mp4_dist_cfg cfg;
    mp4_dist_cfg_defaults(&cfg);
    cfg.mode = MP4_DIST_MODE_TX;

    if (mp4_dist_config_parse_tx_cli(argc, argv, &cfg) != 0)
        return 1;

    if (cfg.tx_stream_count > 0) {
        if (mp4_dist_tx_streams_probe(&cfg) != 0)
            return 1;
        if (mp4_dist_tx_streams_validate(cfg.tx_streams, cfg.tx_stream_count) != 0)
            return 1;
    } else {
        if (cfg.mp4_count <= 0) {
            fprintf(stderr, "TX: specify --mp4 FILE or tx_streams[] in config\n");
            return 1;
        }
        if (mp4_dist_apply_mp4_probe(&cfg) != 0)
            return 1;
        mp4_dist_warn_probe_mismatch(&cfg);
        mp4_dist_print_rx_hint(&cfg, "mp4_dist_rx");
    }

    mp4_dist_config_print(&cfg);
    if (cfg.tx_stream_count > 0)
        printf("=== MP4 Distribution TX multi-session %ds (%d lanes) ===\n",
               cfg.test_time, cfg.tx_stream_count);
    else
        printf("=== MP4 Distribution TX %ds ===\n", cfg.test_time);

    mtl_handle st = mp4_dist_mtl_init(&cfg, cfg.tx_port, cfg.tx_lip, 1, 0,
                                      cfg.mtl_log_level, cfg.mtl_lcores_tx);
    if (!st) return 1;

    tx_scheduler* sched = NULL;
    int ret = tx_scheduler_create(st, &cfg, &sched);
    if (ret < 0) {
        fprintf(stderr, "TX: scheduler create failed: %d\n", ret);
        mtl_stop(st);
        mtl_uninit(st);
        return 1;
    }

    printf("TX: running %ds on %s (shared TX queue)\n",
           cfg.test_time, cfg.tx_port);
    tx_scheduler_run(sched, &g_run);

    tx_scheduler_close(sched);
    mtl_stop(st);
    mtl_uninit(st);
    printf("=== TX PASS ===\n");
    return 0;
}
