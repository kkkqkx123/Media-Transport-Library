#include "file_playout_app.h"
#include "file_playout_config.h"
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

    file_playout_cfg cfg;
    file_playout_cfg_defaults(&cfg);
    cfg.mode = FILE_PLAYOUT_MODE_TX;

    if (file_playout_config_parse_tx_cli(argc, argv, &cfg) != 0)
        return 1;

    if (cfg.tx_stream_count > 0) {
        if (file_playout_tx_streams_probe(&cfg) != 0)
            return 1;
        if (file_playout_tx_streams_validate(cfg.tx_streams, cfg.tx_stream_count) != 0)
            return 1;
    } else {
        if (cfg.mp4_count <= 0) {
            fprintf(stderr, "TX: specify --mp4 FILE or tx_streams[] in config\n");
            return 1;
        }
        if (file_playout_apply_mp4_probe(&cfg) != 0)
            return 1;
        file_playout_warn_probe_mismatch(&cfg);
        file_playout_print_rx_hint(&cfg, "file_playout_rx");
    }

    file_playout_config_print(&cfg);

    mtl_handle st = file_playout_mtl_init(&cfg, cfg.tx_port, cfg.tx_lip, 1, 0,
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

    tx_scheduler_run(sched, &g_run);

    tx_scheduler_close(sched);
    mtl_stop(st);
    mtl_uninit(st);
    return 0;
}
