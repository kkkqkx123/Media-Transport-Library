/* SPDX-License-Identifier: BSD-3-Clause
 * Phase 5: E2E App with MTL ST22 integration.
 *
 * Modes:
 *   tx:   V4L2 → RGA → MPP Enc → MTL TX → network
 *   rx:   network → MTL RX → MPP Dec → DRM
 *   both: single-board dual-NIC (enP3p49s0 TX + eth0 RX in parallel)
 *
 * Usage:
 *   ./e2e_app tx  [seconds] [--port NAME] [--lip IP] [--dip IP] [--udp PORT]
 *   ./e2e_app rx  [seconds] [--port NAME] [--lip IP] [--sip IP] [--udp PORT]
 *   ./e2e_app both [seconds]  (uses defaults below)
 *
 * Defaults (single-board dual-NIC):
 *   TX: native_af_xdp:enP3p49s0  lip=192.168.80.213  dip=192.168.80.212
 *   RX: native_af_xdp:eth0       lip=192.168.80.212  sip=192.168.80.213 (source)
 */

#include "e2e_config.h"
#include "hal/slab_pool.h"
#include "mtl_tx.h"
#include "mtl_rx.h"
#include "mtl_rx_bridge.h"
#include "pipeline/tx_pipeline.h"
#include "pipeline/rx_pipeline.h"
#include "pipeline/ring_buffer.h"
#include <errno.h>
#include <mtl_api.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_bool g_run = true;
static void sig_handler(int s) { (void)s; atomic_store(&g_run, false); }

static void usage(const char* prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s tx  [sec] [--config FILE] [--port P] [--lip IP] [--dip IP] [--udp PORT] [--log-level L]\n", prog);
    fprintf(stderr, "  %s rx  [sec] [--config FILE] [--port P] [--lip IP] [--sip IP] [--udp PORT] [--log-level L]\n", prog);
    fprintf(stderr, "  %s both [sec] [--config FILE] [--log-level L]\n", prog);
}

static int parse_args(int argc, char** argv, const char** mode_out, struct e2e_cfg* cfg) {
    if (argc < 2) {
        usage(argv[0]);
        return -1;
    }
    *mode_out = argv[1];
    if (strcmp(*mode_out, "tx") != 0 && strcmp(*mode_out, "rx") != 0 &&
        strcmp(*mode_out, "both") != 0) {
        usage(argv[0]);
        return -1;
    }
    int i = 2;
    if (i < argc && argv[i][0] != '-') {
        cfg->test_time = atoi(argv[i]);
        i++;
    }
    while (i < argc) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            if (e2e_config_load(argv[++i], cfg) != 0) return -1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (strcmp(*mode_out, "rx") == 0)
                cfg->rx_port = argv[++i];
            else
                cfg->tx_port = argv[++i];
        } else if (strcmp(argv[i], "--lip") == 0 && i + 1 < argc) {
            if (strcmp(*mode_out, "rx") == 0)
                cfg->rx_lip = argv[++i];
            else
                cfg->tx_lip = argv[++i];
        } else if (strcmp(argv[i], "--dip") == 0 && i + 1 < argc) {
            cfg->tx_dip = argv[++i];
        } else if (strcmp(argv[i], "--sip") == 0 && i + 1 < argc) {
            cfg->rx_sip = argv[++i];
        } else if (strcmp(argv[i], "--udp") == 0 && i + 1 < argc) {
            cfg->udp_port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            cfg->log_level = argv[++i];
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return -1;
        }
        i++;
    }
    return 0;
}

static void mtl_set_common_flags(struct mtl_init_params* mtp) {
    mtp->flags |= MTL_FLAG_BIND_NUMA;
    mtp->flags |= MTL_FLAG_TX_VIDEO_MIGRATE;
    mtp->flags |= MTL_FLAG_RX_VIDEO_MIGRATE;
    mtp->flags |= MTL_FLAG_RX_SEPARATE_VIDEO_LCORE;
}

static void apply_log_level(mtl_handle st, const char* level) {
    if (!level || !st) return;
    if (strcmp(level, "debug") == 0)
        mtl_set_log_level(st, MTL_LOG_LEVEL_DEBUG);
    else if (strcmp(level, "notice") == 0)
        mtl_set_log_level(st, MTL_LOG_LEVEL_NOTICE);
    else if (strcmp(level, "warning") == 0)
        mtl_set_log_level(st, MTL_LOG_LEVEL_WARNING);
    else if (strcmp(level, "error") == 0)
        mtl_set_log_level(st, MTL_LOG_LEVEL_ERR);
    else
        mtl_set_log_level(st, MTL_LOG_LEVEL_INFO); /* default */
}

static mtl_handle mtl_setup_single(const char* port_name, const char* lip,
                                   uint16_t tx_q, uint16_t rx_q) {
    struct mtl_init_params mtp = {0};
    mtp.num_ports = 1;
    strncpy(mtp.port[MTL_PORT_P], port_name, MTL_PORT_MAX_LEN - 1);
    mtp.pmd[MTL_PORT_P] = MTL_PMD_NATIVE_AF_XDP;
    mtp.net_proto[MTL_PORT_P] = MTL_PROTO_STATIC;
    strncpy((char*)mtp.sip_addr[MTL_PORT_P], lip, MTL_IP_ADDR_LEN - 1);
    /* r8168 single-queue: native_af_xdp needs rx_queues_cnt==1 to select 1q path
     * in mt_dev.c even for TX-only (tx=1 rx=0 would request 2 queues otherwise). */
    mtp.tx_queues_cnt[MTL_PORT_P] = tx_q ? tx_q : 1;
    mtp.rx_queues_cnt[MTL_PORT_P] = rx_q ? rx_q : 1;
    mtl_set_common_flags(&mtp);

    mtl_handle st = mtl_init(&mtp);
    if (!st) return NULL;
    if (mtl_start(st) != 0) {
        mtl_uninit(st);
        return NULL;
    }
    printf("MTL: %s (lip=%s) started\n", port_name, lip);
    return st;
}

/* ---- RX path: mtl_rx → bridge → rx_pipeline ---- */
#define RX_CODED_POOL_CNT  10
#define RX_CODED_BLK_SIZE  (512 * 1024)

struct rx_stack {
    mtl_handle          st;
    bool                owns_mtl;
    struct ring_buffer* mtl_ring;
    struct ring_buffer* dec_ring;
    struct slab_pool*   coded_pool;
    struct mtl_rx*      mrx;
    struct mtl_rx_bridge* bridge;
    struct rx_pipeline* rp;
};

static int rx_stack_create(struct rx_stack* rs, struct e2e_cfg* cfg,
                           mtl_handle st, bool owns_mtl) {
    memset(rs, 0, sizeof(*rs));
    rs->st = st;
    rs->owns_mtl = owns_mtl;
    if (!rs->st) return -1;

    /* ring depth 2+2 keeps in-flight MTL fbs ≤ framebuff_cnt(8) − slots(2) − decode(1) */
    rs->mtl_ring = ring_create(2);
    rs->dec_ring = ring_create(2);
    if (!rs->mtl_ring || !rs->dec_ring) return -1;

    rs->coded_pool = slab_pool_create(RX_CODED_POOL_CNT, RX_CODED_BLK_SIZE);
    if (!rs->coded_pool) return -1;

    struct mtl_rx_params mrxp = {
        .st = rs->st,
        .output_ring = rs->mtl_ring,
        .framebuff_cnt = 8,  /* ST22_FB_MAX_COUNT; early put_framebuff in rx_pipeline */
        .width = cfg->width,
        .height = cfg->height,
        .fps = cfg->fps,
        .udp_port = cfg->udp_port,
        .sip_addr = cfg->rx_sip,
        .port_name = cfg->rx_port,
    };
    rs->mrx = mtl_rx_create(&mrxp);
    if (!rs->mrx) return -1;

    struct mtl_rx_bridge_params bp = {
        .mtl_ring = rs->mtl_ring,
        .dec_ring = rs->dec_ring,
        .rx_handle = mtl_rx_handle(rs->mrx),
        .coded_pool = rs->coded_pool,
        .cpu = cfg->cpu_bridge,
    };
    rs->bridge = mtl_rx_bridge_create(&bp);
    if (!rs->bridge) return -1;

    struct rx_pipeline_params rxp = {
        .drm_device = cfg->drm_device,
        .drm_connector = cfg->drm_connector,
        .drm_mode = cfg->drm_mode,
        .width = cfg->width,
        .height = cfg->height,
        .drm_buf_count = 5,
        .num_slices = cfg->num_slices,
        .input_ring = rs->dec_ring,
        .coded_pool = rs->coded_pool,
        .rx_handle = mtl_rx_handle(rs->mrx),
        .time_now = (uint64_t(*)(void*))mtl_ptp_read_time,
        .time_priv = rs->st,
        .cpu_decode = cfg->cpu_decode,
    };
    rs->rp = rx_pipeline_create(&rxp);
    if (!rs->rp) return -1;

    printf("RX: port=%s lip=%s sip=%s udp=%u\n",
           cfg->rx_port, cfg->rx_lip, cfg->rx_sip, cfg->udp_port);
    return 0;
}

static int rx_stack_start(struct rx_stack* rs) {
    if (rx_pipeline_start(rs->rp) != 0) return -1;
    if (mtl_rx_bridge_start(rs->bridge) != 0) return -1;
    mtl_rx_start(rs->mrx);
    return 0;
}

static void rx_stack_stop(struct rx_stack* rs) {
    /* Stop MTL ingress, drain bridge → decode → decoder, then release fbs. */
    mtl_rx_begin_stop(rs->mrx);
    mtl_rx_bridge_stop(rs->bridge);
    rx_pipeline_stop(rs->rp);
    mtl_rx_stop(rs->mrx);
}

static void rx_stack_free(struct rx_stack* rs) {
    if (!rs) return;
    rx_stack_stop(rs);
    if (rs->rp) rx_pipeline_free(rs->rp);
    if (rs->bridge) mtl_rx_bridge_free(rs->bridge);
    if (rs->mrx) mtl_rx_free(rs->mrx);
    if (rs->coded_pool) slab_pool_free(rs->coded_pool);
    if (rs->dec_ring) ring_free(rs->dec_ring);
    if (rs->mtl_ring) ring_free(rs->mtl_ring);
    if (rs->st && rs->owns_mtl) {
        mtl_stop(rs->st);
        mtl_uninit(rs->st);
    }
}

/* ---- TX path: tx_pipeline → mtl_tx (direct-write, no fill thread) ---- */
struct tx_stack {
    mtl_handle          st;
    bool                owns_mtl;
    struct tx_pipeline* tp;
    struct mtl_tx*      mtx;
    struct tx_mtl_sink_ops mtl_sink;
};

static int tx_mtl_acquire(void* priv, uint16_t* idx, void** addr) {
    return mtl_tx_acquire_fb((struct mtl_tx*)priv, idx, addr);
}

static int tx_mtl_publish(void* priv, uint16_t idx, size_t size) {
    return mtl_tx_publish_fb((struct mtl_tx*)priv, idx, size);
}

static void tx_mtl_abort(void* priv, uint16_t idx) {
    mtl_tx_abort_fb((struct mtl_tx*)priv, idx);
}

static int tx_stack_create(struct tx_stack* ts, struct e2e_cfg* cfg,
                           mtl_handle st, bool owns_mtl) {
    memset(ts, 0, sizeof(*ts));
    ts->st = st;
    ts->owns_mtl = owns_mtl;
    if (!ts->st) return -1;

    struct mtl_tx_params mtxp = {
        .st = ts->st,
        .framebuff_cnt = 8,
        .framebuff_max = 512 * 1024,
        .width = cfg->width,
        .height = cfg->height,
        .fps = cfg->fps,
        .udp_port = cfg->udp_port,
        .dip_addr = cfg->tx_dip,
        .port_name = cfg->tx_port,
    };
    ts->mtx = mtl_tx_create(&mtxp);
    if (!ts->mtx) return -1;

    ts->mtl_sink.priv       = ts->mtx;
    ts->mtl_sink.acquire_fb = tx_mtl_acquire;
    ts->mtl_sink.publish_fb = tx_mtl_publish;
    ts->mtl_sink.abort_fb   = tx_mtl_abort;

    struct tx_pipeline_params txp = {
        .v4l2_device = cfg->v4l2_device,
        .width = cfg->width,
        .height = cfg->height,
        .fps = cfg->fps,
        .v4l2_buf_count = 6,
        .drm_device = cfg->drm_device,
        .rga_pool_size = 12,
        .bitrate_bps = cfg->bitrate_bps,
        .gop_size = cfg->gop_size,
        .num_slices = cfg->num_slices,
        .intra_refresh_mode = cfg->intra_refresh_mode,
        .intra_refresh_arg  = cfg->intra_refresh_arg,
        .coded_blk_size = 512 * 1024,
        .ring_size = 2,
        .cpu_capture = cfg->cpu_capture,
        .cpu_encode = cfg->cpu_encode,
        .time_now = (uint64_t(*)(void*))mtl_ptp_read_time,
        .time_priv = ts->st,
        .mtl_sink = &ts->mtl_sink,
    };
    ts->tp = tx_pipeline_create(&txp);
    if (!ts->tp) return -1;

    printf("TX: port=%s lip=%s dip=%s udp=%u (direct-write)\n",
           cfg->tx_port, cfg->tx_lip, cfg->tx_dip, cfg->udp_port);
    return 0;
}

static int tx_stack_start(struct tx_stack* ts) {
    if (mtl_tx_start(ts->mtx) != 0) return -1;
    if (tx_pipeline_start(ts->tp) != 0) {
        mtl_tx_stop(ts->mtx);
        return -1;
    }
    return 0;
}

static void tx_stack_stop(struct tx_stack* ts) {
    if (!ts) return;
    if (ts->tp) tx_pipeline_stop(ts->tp);
    if (ts->mtx) mtl_tx_stop(ts->mtx);
}

static void tx_stack_free(struct tx_stack* ts) {
    if (!ts) return;
    tx_stack_stop(ts);
    if (ts->mtx) mtl_tx_free(ts->mtx);
    if (ts->tp) tx_pipeline_free(ts->tp);
    if (ts->st && ts->owns_mtl) {
        mtl_stop(ts->st);
        mtl_uninit(ts->st);
    }
}

static int run_tx_mode(struct e2e_cfg* cfg) {
    mtl_handle st = mtl_setup_single(cfg->tx_port, cfg->tx_lip, 1, 0);
    if (!st) return 1;
    apply_log_level(st, cfg->log_level);
    struct tx_stack ts;
    if (tx_stack_create(&ts, cfg, st, true) != 0) {
        tx_stack_free(&ts);
        return 1;
    }
    printf("TX: running %ds...\n", cfg->test_time);
    if (tx_stack_start(&ts) != 0) {
        tx_stack_free(&ts);
        return 1;
    }
    int t = cfg->test_time;
    while (atomic_load(&g_run) && t > 0) {
        sleep(1);
        t--;
    }
    tx_stack_free(&ts);
    printf("=== TX PASS ===\n");
    return 0;
}

static int run_rx_mode(struct e2e_cfg* cfg) {
    mtl_handle st = mtl_setup_single(cfg->rx_port, cfg->rx_lip, 0, 1);
    if (!st) return 1;
    apply_log_level(st, cfg->log_level);
    struct rx_stack rs;
    if (rx_stack_create(&rs, cfg, st, true) != 0) {
        rx_stack_free(&rs);
        return 1;
    }
    printf("RX: waiting for stream %ds...\n", cfg->test_time);
    if (rx_stack_start(&rs) != 0) {
        rx_stack_free(&rs);
        return 1;
    }
    int t = cfg->test_time;
    while (atomic_load(&g_run) && t > 0) {
        sleep(1);
        t--;
    }

    uint64_t min_ns, avg_ns, max_ns;
    rx_pipeline_get_latency(rs.rp, &min_ns, &avg_ns, &max_ns);
    rx_stack_free(&rs);

    printf("\n=== E2E Latency: min=%llu avg=%llu max=%llu us ===\n",
           (unsigned long long)(min_ns / 1000),
           (unsigned long long)(avg_ns / 1000),
           (unsigned long long)(max_ns / 1000));
    printf("=== RX PASS ===\n");
    return 0;
}

/*
 * Both mode: fork RX + TX child processes.
 * DPDK EAL cannot re-init in one process; r8168 is single-queue so dual-port
 * single-handle also fails. Two processes each call mtl_init once.
 */
static int run_both_mode(struct e2e_cfg* cfg) {
    printf("=== Both mode: TX(%s) → L2 → RX(%s), %ds (fork) ===\n",
           cfg->tx_port, cfg->rx_port, cfg->test_time);
    printf("Prerequisites: MtlManager running, hugepages configured\n");

    pid_t rx_pid = fork();
    if (rx_pid == 0) {
        return run_rx_mode(cfg);
    }
    if (rx_pid < 0) {
        perror("fork rx");
        return 1;
    }

    pid_t tx_pid = fork();
    if (tx_pid == 0) {
        printf("both: RX child pid=%d, TX starts in 2s...\n", (int)rx_pid);
        sleep(2);
        return run_tx_mode(cfg);
    }
    if (tx_pid < 0) {
        kill(rx_pid, SIGTERM);
        waitpid(rx_pid, NULL, 0);
        perror("fork tx");
        return 1;
    }

    printf("both: parent waiting (rx_pid=%d tx_pid=%d)\n", (int)rx_pid, (int)tx_pid);
    int rx_st = 1, tx_st = 1;
    int rx_done = 0, tx_done = 0;
    while (!rx_done || !tx_done) {
        int st;
        pid_t w;
        if (!rx_done) {
            w = waitpid(rx_pid, &st, WNOHANG);
            if (w == rx_pid) { rx_st = WIFEXITED(st) ? WEXITSTATUS(st) : 1; rx_done = 1; }
        }
        if (!tx_done) {
            w = waitpid(tx_pid, &st, WNOHANG);
            if (w == tx_pid) { tx_st = WIFEXITED(st) ? WEXITSTATUS(st) : 1; tx_done = 1; }
        }
        if (!rx_done || !tx_done) usleep(200000);
    }

    printf("=== BOTH %s (rx=%d tx=%d) ===\n",
           (rx_st == 0 && tx_st == 0) ? "PASS" : "FAIL", rx_st, tx_st);
    return (rx_st == 0 && tx_st == 0) ? 0 : 1;
}

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);

    const char* mode = "both";
    struct e2e_cfg cfg = e2e_cfg_defaults();
    if (parse_args(argc, argv, &mode, &cfg) != 0) return 1;

    printf("=== Phase 5 E2E App [%s] %ds ===\n", mode, cfg.test_time);

    if (strcmp(mode, "tx") == 0) return run_tx_mode(&cfg);
    if (strcmp(mode, "rx") == 0) return run_rx_mode(&cfg);
    return run_both_mode(&cfg);
}
