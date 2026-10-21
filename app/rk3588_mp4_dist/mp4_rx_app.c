/* SPDX-License-Identifier: BSD-3-Clause
 *
 * MP4 Distribution RX — network → MTL RX → MPP decode → DRM + ALSA.
 *
 * Usage:
 *   ./mp4_dist_rx [sec] [--config FILE] [--port P] [--lip IP] [--sip IP]
 *       [--udp PORT] [--audio-udp PORT] [--width W] [--height H] [--fps FPS]
 *       [--probe-mp4 FILE] [--mtl-log-level LEVEL]
 *
 * Phase B: config with pip_streams[] → rx_compositor (multi-lane PiP).
 */

#include "mp4_dist_app.h"
#include "mp4_dist_config.h"
#include "rx/rx_pipeline.h"
#include "rx/rx_compositor.h"
#include "rx/mtl_rx_mp4.h"
#include "rx/mtl_rx_bridge.h"
#include "shared/ring_buffer.h"
#include "shared/slab_pool.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RX_RING_DEPTH      16
#define RX_ST22_FB_CNT     8
#define RX_CODED_POOL_CNT  24

static atomic_bool g_run = true;

static void sig_handler(int s) {
    (void)s;
    atomic_store(&g_run, false);
}

static int rx_report_interval_s(const mp4_dist_cfg* cfg) {
    if (cfg->stats_report_interval_s > 0)
        return cfg->stats_report_interval_s;
    if (cfg->e2e_report_interval_s > 0)
        return cfg->e2e_report_interval_s;
    return 0;
}

static void rx_print_final_e2e(uint64_t count, uint64_t min_ns, uint64_t avg_ns,
                               uint64_t max_ns) {
    if (!count) {
        printf("\nE2E-C: (not measured — need PTP time_now + TX in-band stamp)\n");
        return;
    }
    printf("\n=== E2E-C total: n=%llu min=%llu avg=%llu max=%llu us ===\n",
           (unsigned long long)count,
           (unsigned long long)(min_ns / 1000),
           (unsigned long long)(avg_ns / 1000),
           (unsigned long long)(max_ns / 1000));
}

static int rx_main_loop(const mp4_dist_cfg* cfg, int report_interval,
                        void (*on_tick)(void* ctx, int period_s), void* ctx) {
    int t = cfg->test_time;
    int elapsed = 0;
    while (atomic_load(&g_run) && t > 0) {
        sleep(1);
        t--;
        elapsed++;
        if (report_interval > 0 && on_tick && (elapsed % report_interval) == 0)
            on_tick(ctx, report_interval);
    }
    return 0;
}

typedef struct {
    rx_compositor* comp;
} comp_loop_ctx;

static void compositor_on_tick(void* vctx, int period_s) {
    comp_loop_ctx* c = vctx;
    rx_compositor_report_stats(c->comp, period_s, true);
}

typedef struct {
    rx_pipeline*   rp;
    mtl_rx_mp4*      mrx;
    mtl_rx_bridge*   bridge;
} pipe_loop_ctx;

static void pipeline_on_tick(void* vctx, int period_s) {
    pipe_loop_ctx* c = vctx;
    rx_pipeline_report_stats(c->rp, c->mrx, c->bridge, period_s, true);
}

static const mp4_dist_pip_stream* pip_main_stream(const mp4_dist_cfg* cfg) {
    for (int i = 0; i < cfg->pip_stream_count; i++) {
        if (cfg->pip_streams[i].main)
            return &cfg->pip_streams[i];
    }
    return &cfg->pip_streams[0];
}

static void pip_fill_defaults(mp4_dist_cfg* cfg) {
    for (int i = 0; i < cfg->pip_stream_count; i++) {
        mp4_dist_pip_stream* s = &cfg->pip_streams[i];
        if (!s->codestream_size && cfg->codestream_size)
            s->codestream_size = cfg->codestream_size;
        if (!s->audio_udp_port && s->main && cfg->audio_udp_port)
            s->audio_udp_port = cfg->audio_udp_port;
    }
}

static int run_compositor(mp4_dist_cfg* cfg) {
    const mp4_dist_pip_stream* main = pip_main_stream(cfg);

    if (cfg->width <= 0)
        cfg->width = 1920;
    if (cfg->height <= 0)
        cfg->height = 1080;
    if (cfg->fps <= 0.0f)
        cfg->fps = main->fps > 0 ? main->fps : 24.0f;

    pip_fill_defaults(cfg);
    mp4_dist_apply_drm_mode(cfg);

    int mode_w = 0, mode_h = 0;
    mp4_dist_mode_dims(cfg, &mode_w, &mode_h);
    if (mp4_dist_pip_validate(cfg->pip_streams, cfg->pip_stream_count,
                              mode_w, mode_h) < 0)
        return 1;

    mp4_dist_config_print(cfg);
    printf("=== MP4 Distribution RX compositor %ds ===\n", cfg->test_time);

    mtl_handle st = mp4_dist_mtl_init(cfg, cfg->rx_port, cfg->rx_lip, 0, 1,
                                      cfg->mtl_log_level, cfg->mtl_lcores_rx);
    if (!st)
        return 1;

    rx_compositor_params cp = {
        .mt                       = st,
        .drm_device               = cfg->drm_device,
        .drm_connector            = cfg->drm_connector,
        .drm_mode                 = cfg->drm_mode,
        .drm_buf_count            = cfg->drm_buf_count,
        .streams                  = cfg->pip_streams,
        .stream_count             = cfg->pip_stream_count,
        .rx_port                  = cfg->rx_port,
        .alsa_device              = cfg->alsa_device,
        .playout_delay_ms         = cfg->playout_delay_ms,
        .test_time                = cfg->test_time,
        .default_codestream_size  = cfg->codestream_size,
        .default_audio_udp_port   = cfg->audio_udp_port,
        .cpu_bridge               = cfg->cpu_bridge,
        .cpu_decode               = cfg->cpu_decode,
        .cpu_render               = cfg->cpu_render,
        .cpu_audio                = cfg->cpu_audio,
    };
    if (cfg->ptp_mode != MP4_DIST_PTP_MONOTONIC) {
        cp.time_now  = (uint64_t(*)(void*))mtl_ptp_read_time;
        cp.time_priv = st;
    }

    rx_compositor* comp = rx_compositor_create(&cp);
    if (!comp) {
        fprintf(stderr, "RX: compositor create failed\n");
        mtl_stop(st);
        mtl_uninit(st);
        return 1;
    }

    printf("RX: waiting for %d stream(s) %ds...\n", cfg->pip_stream_count,
           cfg->test_time);

    if (rx_compositor_start(comp) != 0) {
        fprintf(stderr, "RX: compositor start failed\n");
        rx_compositor_free(comp);
        mtl_stop(st);
        mtl_uninit(st);
        return 1;
    }

    int report_iv = rx_report_interval_s(cfg);
    comp_loop_ctx loop = { .comp = comp };
    rx_main_loop(cfg, report_iv, compositor_on_tick, &loop);

    rx_compositor_stop(comp);

    uint64_t min_ns, avg_ns, max_ns;
    rx_compositor_get_latency(comp, &min_ns, &avg_ns, &max_ns);
    uint64_t e2e_n = rx_compositor_e2e_count(comp);

    rx_compositor_report_stats(comp, 0, false);
    rx_print_final_e2e(e2e_n, min_ns, avg_ns, max_ns);

    rx_compositor_free(comp);
    mtl_stop(st);
    mtl_uninit(st);

    printf("=== RX PASS (compositor) ===\n");
    return 0;
}

static int run_legacy_pipeline(mp4_dist_cfg* cfg) {
    if (cfg->fps <= 0.0f) {
        if (cfg->probe_mp4[0]) {
            if (mp4_dist_apply_mp4_probe(cfg) != 0)
                return 1;
        } else {
            fprintf(stderr,
                    "RX: --fps is required (must match TX). "
                    "Use --probe-mp4 FILE or --fps N.NN\n");
            return 1;
        }
    }
    if (cfg->width <= 0)
        cfg->width = 1920;
    if (cfg->height <= 0)
        cfg->height = 1080;
    mp4_dist_warn_probe_mismatch(cfg);
    mp4_dist_apply_drm_mode(cfg);

    mp4_dist_config_print(cfg);
    printf("=== MP4 Distribution RX %ds ===\n", cfg->test_time);

    mtl_handle st = mp4_dist_mtl_init(cfg, cfg->rx_port, cfg->rx_lip, 0, 1,
                                      cfg->mtl_log_level, cfg->mtl_lcores_rx);
    if (!st)
        return 1;

    size_t coded_blk_size = cfg->codestream_size > 0 ? cfg->codestream_size
                                                     : (1024 * 1024);

    struct ring_buffer* mtl_ring = ring_create(RX_RING_DEPTH);
    struct ring_buffer* dec_ring = ring_create(RX_RING_DEPTH);
    if (!mtl_ring || !dec_ring) {
        fprintf(stderr, "RX: ring create failed\n");
        mtl_stop(st);
        mtl_uninit(st);
        return 1;
    }

    struct slab_pool* coded_pool = slab_pool_create(RX_CODED_POOL_CNT, coded_blk_size);
    if (!coded_pool) {
        fprintf(stderr, "RX: slab pool create failed (blk=%zu)\n", coded_blk_size);
        ring_free(mtl_ring);
        ring_free(dec_ring);
        mtl_stop(st);
        mtl_uninit(st);
        return 1;
    }

    mtl_rx_mp4_params mrxp = {
        .st                  = st,
        .output_ring         = mtl_ring,
        .framebuff_cnt       = RX_ST22_FB_CNT,
        .width               = cfg->width,
        .height              = cfg->height,
        .fps                 = cfg->fps > 0 ? cfg->fps : 24.0f,
        .udp_port            = cfg->udp_port,
        .sip_addr            = cfg->rx_sip,
        .port_name           = cfg->rx_port,
        .codestream_max_size = coded_blk_size,
    };
    if (cfg->ptp_mode != MP4_DIST_PTP_MONOTONIC) {
        mrxp.time_fn   = (media_clk_time_fn)mtl_ptp_read_time;
        mrxp.time_priv = st;
    }
    mtl_rx_mp4* mrx = mtl_rx_mp4_create(&mrxp);
    if (!mrx) {
        fprintf(stderr, "RX: mtl_rx_mp4_create failed\n");
        slab_pool_free(coded_pool);
        ring_free(mtl_ring);
        ring_free(dec_ring);
        mtl_stop(st);
        mtl_uninit(st);
        return 1;
    }

    mtl_rx_bridge_params bp = {
        .mtl_ring   = mtl_ring,
        .dec_ring   = dec_ring,
        .rx_handle  = mtl_rx_mp4_handle(mrx),
        .coded_pool = coded_pool,
        .cpu        = cfg->cpu_bridge,
    };
    mtl_rx_bridge* bridge = mtl_rx_bridge_create(&bp);
    if (!bridge) {
        fprintf(stderr, "RX: bridge create failed\n");
        mtl_rx_mp4_free(mrx);
        slab_pool_free(coded_pool);
        ring_free(mtl_ring);
        ring_free(dec_ring);
        mtl_stop(st);
        mtl_uninit(st);
        return 1;
    }

    rx_pipeline_params rxp = {
        .mt               = st,
        .drm_device       = cfg->drm_device,
        .drm_connector    = cfg->drm_connector,
        .drm_mode         = cfg->drm_mode,
        .width            = cfg->width,
        .height           = cfg->height,
        .fps              = cfg->fps,
        .drm_buf_count    = cfg->drm_buf_count,
        .input_ring       = dec_ring,
        .coded_pool       = coded_pool,
        .rx_handle        = mtl_rx_mp4_handle(mrx),
        .alsa_device      = cfg->alsa_device,
        .audio_udp_port   = cfg->audio_udp_port,
        .rx_sip           = cfg->rx_sip,
        .rx_port          = cfg->rx_port,
        .playout_delay_ms = cfg->playout_delay_ms,
        .test_time        = cfg->test_time,
        .cpu_decode       = cfg->cpu_decode,
        .cpu_render       = cfg->cpu_render,
        .cpu_audio        = cfg->cpu_audio,
        .display_viewport = &cfg->display_viewport,
        .udp_port         = cfg->udp_port,
    };
    if (cfg->ptp_mode != MP4_DIST_PTP_MONOTONIC) {
        rxp.time_now  = (uint64_t(*)(void*))mtl_ptp_read_time;
        rxp.time_priv = st;
    }
    rx_pipeline* rp = rx_pipeline_create(&rxp);
    if (!rp) {
        fprintf(stderr, "RX: pipeline create failed\n");
        mtl_rx_bridge_free(bridge);
        mtl_rx_mp4_free(mrx);
        slab_pool_free(coded_pool);
        ring_free(mtl_ring);
        ring_free(dec_ring);
        mtl_stop(st);
        mtl_uninit(st);
        return 1;
    }

    printf("RX: waiting for stream %ds...\n", cfg->test_time);

    if (rx_pipeline_start(rp) != 0 || mtl_rx_bridge_start(bridge) != 0) {
        fprintf(stderr, "RX: start failed\n");
        rx_pipeline_free(rp);
        mtl_rx_bridge_free(bridge);
        mtl_rx_mp4_free(mrx);
        slab_pool_free(coded_pool);
        ring_free(mtl_ring);
        ring_free(dec_ring);
        mtl_stop(st);
        mtl_uninit(st);
        return 1;
    }
    mtl_rx_mp4_start(mrx);

    int report_iv = rx_report_interval_s(cfg);
    pipe_loop_ctx loop = { .rp = rp, .mrx = mrx, .bridge = bridge };
    rx_main_loop(cfg, report_iv, pipeline_on_tick, &loop);

    mtl_rx_mp4_begin_stop(mrx);
    mtl_rx_bridge_stop(bridge);
    rx_pipeline_stop(rp);
    mtl_rx_mp4_stop(mrx);

    uint64_t min_ns, avg_ns, max_ns;
    rx_pipeline_get_latency(rp, &min_ns, &avg_ns, &max_ns);
    uint64_t e2e_n = rx_pipeline_e2e_count(rp);

    rx_pipeline_report_stats(rp, mrx, bridge, 0, false);
    rx_print_final_e2e(e2e_n, min_ns, avg_ns, max_ns);

    rx_pipeline_free(rp);
    mtl_rx_bridge_free(bridge);
    mtl_rx_mp4_free(mrx);
    slab_pool_free(coded_pool);
    ring_free(dec_ring);
    ring_free(mtl_ring);

    mtl_stop(st);
    mtl_uninit(st);

    printf("=== RX PASS ===\n");
    return 0;
}

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);

    mp4_dist_cfg cfg;
    mp4_dist_cfg_defaults(&cfg);
    cfg.mode = MP4_DIST_MODE_RX;

    if (mp4_dist_config_parse_rx_cli(argc, argv, &cfg) != 0)
        return 1;

    if (cfg.pip_stream_count > 0)
        return run_compositor(&cfg);
    return run_legacy_pipeline(&cfg);
}
