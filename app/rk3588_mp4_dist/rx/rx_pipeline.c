/* SPDX-License-Identifier: BSD-3-Clause
 *
 * RX Pipeline — MPP decode and DRM render on separate threads.
 * P2: decode pushes playout_buffer; render pops at scheduled PTS.
 */

#define _GNU_SOURCE
#include "rx_pipeline.h"
#include "mtl_rx_audio.h"
#include "mtl_rx_mp4.h"
#include "mtl_rx_bridge.h"
#include "h264_prep.h"
#include "alsa_audio.h"
#include "playout_buffer.h"
#include "av_sync.h"
#include "../shared/pipeline_common.h"
#include "../shared/ring_buffer.h"
#include "../shared/slab_pool.h"
#include "../shared/codec_plugin.h"
#include "../shared/mpp_decoder.h"
#include "../shared/drm_renderer.h"
#include "../shared/rx_e2e_stats.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Legacy path chunks at 8KB inside mpp_dec_put (see FEED_CHUNK_SIZE). */

struct rx_pipeline {
    rx_pipeline_params  cfg;

    struct codec_plugin* decoder;
    struct drm_renderer* renderer;
    h264_prep*           h264;

    playout_buffer       pb;
    av_sync              sync;
    alsa_audio*          alsa;
    mtl_rx_audio*        audio_rx;

    pthread_t            dec_thd;
    pthread_t            render_thd;
    atomic_bool          running;
    atomic_bool          render_running;

    uint64_t             stat_received;
    uint64_t             stat_decoded;
    uint64_t             stat_rendered;
    uint64_t             stat_queued;
    uint64_t             stat_put_fail;
    uint64_t             stat_prep_invalid;
    int                  dec_err_streak;
    uint64_t             stat_pb_repeated;
    uint64_t             stat_pb_underrun;
    uint64_t             stat_pb_skip;
    uint64_t             frame_period_ns;
    rx_e2e_stats         e2e;
    uint64_t             last_rx_pts;
    char                 stats_label[24];
    /* snapshots for periodic delta reporting */
    uint64_t             snap_received;
    uint64_t             snap_decoded;
    uint64_t             snap_rendered;
    uint64_t             snap_bridged;
    uint64_t             snap_stamp_ok;
    uint32_t             snap_mtl_drops;
    uint32_t             snap_br_drops;
};

static int rx_dispatch_frame(rx_pipeline* rp, int out_fd, uint32_t out_h_stride,
                             uint32_t out_v_stride, uint64_t rx_timestamp,
                             uint64_t capture_tai_ns) {
    rp->stat_decoded++;

    if (!rx_timestamp)
        rx_timestamp = av_sync_playout_target_ns(&rp->sync);

    playout_buffer_push(&rp->pb, out_fd, out_h_stride, out_v_stride, rx_timestamp,
                        capture_tai_ns);
    rp->stat_queued++;
    return 1;
}

/** Pull displayable frames; optional idle retries (B-frame reorder delay).
 * P3: each MPP output gets au_pts + n * frame_period (n = 0,1,... within AU). */
static int rx_drain_decoder(rx_pipeline* rp, uint64_t au_pts, uint64_t capture_tai_ns,
                            int idle_rounds) {
    void* dec = rp->decoder->priv;
    int outputs = 0;
    int idle = 0;
    int au_frame_idx = 0;

    for (;;) {
        int out_fd;
        uint32_t out_h_stride, out_v_stride;

        if (mpp_dec_pull(dec, &out_fd, &out_h_stride, &out_v_stride) == 1) {
            idle = 0;
            uint64_t frame_pts;
            if (au_pts)
                frame_pts = au_pts + (uint64_t)au_frame_idx * rp->frame_period_ns;
            else
                frame_pts = av_sync_playout_target_ns(&rp->sync);
            rx_dispatch_frame(rp, out_fd, out_h_stride, out_v_stride, frame_pts,
                              capture_tai_ns);
            au_frame_idx++;
            outputs++;
            continue;
        }
        if (idle >= idle_rounds)
            break;
        idle++;
        for (int w = 0; w < 50; w++)
            sched_yield();
    }
    return outputs;
}

static int rx_render_frame(rx_pipeline* rp, int dma_fd, uint32_t hor_stride,
                           uint32_t ver_stride, uint64_t capture_tai_ns) {
    if (dma_fd < 0) return -1;
    int ret = drm_renderer_display(rp->renderer, dma_fd, hor_stride, ver_stride);
    if (ret == 0) {
        rp->stat_rendered++;
        if (capture_tai_ns && rp->cfg.time_now) {
            uint64_t present = rp->cfg.time_now(rp->cfg.time_priv);
            rx_e2e_stats_record(&rp->e2e, capture_tai_ns, present, rp->stats_label);
        }
    }
    return ret;
}

static int rx_feed_packet(rx_pipeline* rp, uint8_t* data, size_t size,
                          uint64_t rx_timestamp, uint64_t capture_tai_ns) {
    const uint8_t* feed_data = data;
    size_t feed_size = size;
    int feed_owned = 0;
    void* dec = rp->decoder->priv;

    if (rp->h264) {
        if (h264_prep_for_mpp(rp->h264, data, size, &feed_data, &feed_size,
                              &feed_owned) < 0) {
            rp->stat_prep_invalid++;
            return -1;
        }
    }

    if (mpp_dec_uses_whole_au(dec)) {
        if (mpp_dec_put_whole(dec, (uint8_t*)feed_data, feed_size) < 0) {
            rp->stat_put_fail++;
            if (feed_owned) free((void*)feed_data);
            return -1;
        }
    } else if (mpp_dec_put(dec, (uint8_t*)feed_data, feed_size) < 0) {
        rp->stat_put_fail++;
        if (feed_owned) free((void*)feed_data);
        return -1;
    }

    int idle_rounds = mpp_dec_uses_aggressive_pull(dec) ? MPP_DEC_AGGRESSIVE_IDLE_ROUNDS : 0;
    int outputs = rx_drain_decoder(rp, rx_timestamp, capture_tai_ns, idle_rounds);

    if (feed_owned) free((void*)feed_data);

    if (outputs > 0)
        rp->dec_err_streak = 0;

    return outputs;
}

static void sleep_until_ns(rx_pipeline* rp, uint64_t wake_ns) {
    for (int i = 0; i < 200; i++) {
        uint64_t now;
        if (rp->cfg.time_now)
            now = rp->cfg.time_now(rp->cfg.time_priv);
        else {
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            now = (uint64_t)now_ts.tv_sec * 1000000000ULL +
                  (uint64_t)now_ts.tv_nsec;
        }
        if (wake_ns <= now)
            return;

        uint64_t delta = wake_ns - now;
        if (delta > 100000000ULL)
            delta = 100000000ULL;

        struct timespec ts;
        ts.tv_sec  = (time_t)(delta / 1000000000ULL);
        ts.tv_nsec = (long)(delta % 1000000000ULL);
        nanosleep(&ts, NULL);
    }
}

static void* render_thread(void* arg) {
    rx_pipeline* rp = (rx_pipeline*)arg;
    float fps = rp->cfg.fps > 0 ? rp->cfg.fps : 24.0f;
    uint64_t frame_period_ns = (uint64_t)(1000000000.0 / (double)fps);
    int wait_ms = (int)(frame_period_ns / 2000000ULL);
    if (wait_ms < 2) wait_ms = 2;
    if (wait_ms > 20) wait_ms = 20;

    while (atomic_load(&rp->render_running)) {
        if (!playout_buffer_has_frames(&rp->pb)) {
            if (!playout_buffer_wait_data(&rp->pb, wait_ms))
                rp->stat_pb_underrun++;
            continue;
        }

        uint64_t target = av_sync_playout_target_ns(&rp->sync);
        uint64_t peek   = playout_buffer_peek_pts(&rp->pb);
        uint64_t delay  = rp->sync.playout_delay_ns;

        /* Frame due when pts <= now - delay  <=>  now >= pts + delay */
        if (peek > target) {
            sleep_until_ns(rp, peek + delay);
            continue;
        }

        /* P2-C: drain all due frames; render newest only; count skips. */
        int fd = -1;
        uint32_t hs = 0, vs = 0;
        uint64_t capture_tai = 0;
        int pops = 0;
        int r;

        do {
            int out_fd;
            uint32_t out_hs, out_vs;
            uint64_t cap = 0;
            r = playout_buffer_pop_at(&rp->pb, target, 0,
                                      &out_fd, &out_hs, &out_vs, &cap);
            if (r == 1) {
                fd = out_fd;
                hs = out_hs;
                vs = out_vs;
                if (cap)
                    capture_tai = cap;
                pops++;
            }
        } while (r == 1);

        if (pops > 0) {
            if (pops > 1)
                rp->stat_pb_skip += (uint64_t)(pops - 1);
            rx_render_frame(rp, fd, hs, vs, capture_tai);
        } else if (r == -1) {
            rp->stat_pb_repeated++;
            sleep_until_ns(rp, target + frame_period_ns / 4);
        }
    }
    return NULL;
}

static void* decode_thread(void* arg) {
    rx_pipeline* rp = (rx_pipeline*)arg;
    void* dec = rp->decoder->priv;

    while (atomic_load(&rp->running)) {
        struct codec_packet* pkt = NULL;
        if (ring_pop_wait(rp->cfg.input_ring, (void**)&pkt, &rp->running) != 0)
            continue;
        if (!pkt) continue;
        rp->stat_received++;
        rp->last_rx_pts = pkt->pts;

        rx_feed_packet(rp, pkt->data, pkt->size, pkt->pts, pkt->capture_tai_ns);

        if (rp->cfg.coded_pool && pkt->data)
            slab_free(rp->cfg.coded_pool, pkt->data);
        free(pkt);
    }

    if (mpp_dec_uses_aggressive_pull(dec)) {
        mpp_dec_send_eos(dec);
        rx_drain_decoder(rp, rp->last_rx_pts, 0, MPP_DEC_EOS_FLUSH_IDLE_ROUNDS);
    } else {
        rx_drain_decoder(rp, rp->last_rx_pts, 0, 0);
    }

    return NULL;
}

static void pin_thread(pthread_t thd, int cpu) {
    if (cpu < 0) return;
    cpu_set_t cset;
    CPU_ZERO(&cset);
    CPU_SET(cpu, &cset);
    pthread_setaffinity_np(thd, sizeof(cset), &cset);
}

rx_pipeline* rx_pipeline_create(const rx_pipeline_params* params) {
    rx_pipeline* rp = calloc(1, sizeof(*rp));
    if (!rp) return NULL;
    rp->cfg = *params;
    rx_e2e_stats_init(&rp->e2e);
    if (params->udp_port)
        snprintf(rp->stats_label, sizeof(rp->stats_label), "%u", params->udp_port);
    else
        snprintf(rp->stats_label, sizeof(rp->stats_label), "video");

    float decode_fps = params->fps > 0 ? params->fps : 24.0f;
    rp->frame_period_ns = (uint64_t)(1000000000.0 / (double)decode_fps);

    struct codec_config codec_cfg = {
        .format      = CODEC_FMT_H264,
        .width       = params->width,
        .height      = params->height,
        .fps         = decode_fps,
        .low_latency = true,
        .num_slices  = 1,
    };
    rp->decoder = mpp_decoder_create(&codec_cfg);
    if (!rp->decoder) { fprintf(stderr, "rx_pipe: decoder create failed\n"); goto fail; }

    rp->h264 = h264_prep_create();
    if (!rp->h264) { fprintf(stderr, "rx_pipe: h264_prep create failed\n"); goto fail; }

    rp->renderer = drm_renderer_create(&(drm_renderer_params){
        .device          = params->drm_device,
        .connector       = params->drm_connector,
        .mode_str        = params->drm_mode,
        .content_width   = params->width,
        .content_height  = params->height,
        .fourcc          = DRM_FORMAT_NV12,
        .buf_count       = params->drm_buf_count,
        .viewport        = params->display_viewport,
    });
    if (!rp->renderer) { fprintf(stderr, "rx_pipe: renderer create failed\n"); goto fail; }

    if (alsa_audio_open(params->alsa_device, &rp->alsa) != 0) {
        fprintf(stderr, "rx_pipe: ALSA open failed — continuing without audio\n");
        rp->alsa = NULL;
    }

    playout_buffer_init(&rp->pb);
    av_sync_init(&rp->sync, rp->alsa, &rp->pb, params->playout_delay_ms,
                 params->time_now, params->time_priv);

    if (rp->alsa && params->mt) {
        mtl_rx_audio_params arp = {
            .st              = params->mt,
            .rx_port         = params->rx_port,
            .rx_sip          = params->rx_sip,
            .audio_udp_port  = params->audio_udp_port,
            .payload_type    = 98,
            .alsa            = rp->alsa,
            .sync            = &rp->sync,
            .cpu             = params->cpu_audio,
        };
        rp->audio_rx = mtl_rx_audio_create(&arp);
        if (!rp->audio_rx) {
            fprintf(stderr, "rx_pipe: mtl_rx_audio create failed — no audio\n");
            alsa_audio_close(rp->alsa);
            rp->alsa = NULL;
            av_sync_init(&rp->sync, NULL, &rp->pb, params->playout_delay_ms,
                         params->time_now, params->time_priv);
        }
    }

    printf("rx_pipe: %dx%d @ %.3ffps delay=%dms audio=%s "
           "display=%s playout=latest stream=continuous (decode cpu=%d render cpu=%d)\n",
           params->width, params->height, (double)decode_fps,
           params->playout_delay_ms, rp->alsa ? "on" : "off",
           (params->display_viewport && params->display_viewport->enabled)
               ? "viewport"
               : "fullscreen",
           params->cpu_decode, params->cpu_render);
    return rp;

fail:
    rx_pipeline_free(rp);
    return NULL;
}

void rx_pipeline_free(rx_pipeline* rp) {
    if (!rp) return;
    rx_pipeline_stop(rp);
    if (rp->audio_rx) mtl_rx_audio_free(rp->audio_rx);
    if (rp->alsa)  alsa_audio_close(rp->alsa);
    if (rp->renderer) drm_renderer_free(rp->renderer);
    rp->renderer = NULL;
    if (rp->decoder) { rp->decoder->free(rp->decoder->priv); free(rp->decoder); }
    if (rp->h264) h264_prep_free(rp->h264);
    rx_e2e_stats_destroy(&rp->e2e);
    playout_buffer_destroy(&rp->pb);
    pthread_mutex_destroy(&rp->sync.lock);
    free(rp);
}

int rx_pipeline_start(rx_pipeline* rp) {
    if (!rp || !rp->cfg.input_ring) return -1;
    atomic_store(&rp->running, true);
    atomic_store(&rp->render_running, true);

    if (rp->audio_rx && mtl_rx_audio_start(rp->audio_rx) != 0) {
        atomic_store(&rp->running, false);
        atomic_store(&rp->render_running, false);
        return -1;
    }

    if (pthread_create(&rp->render_thd, NULL, render_thread, rp) != 0) {
        atomic_store(&rp->running, false);
        atomic_store(&rp->render_running, false);
        if (rp->audio_rx) mtl_rx_audio_stop(rp->audio_rx);
        return -1;
    }
    pin_thread(rp->render_thd, rp->cfg.cpu_render);

    if (pthread_create(&rp->dec_thd, NULL, decode_thread, rp) != 0) {
        atomic_store(&rp->running, false);
        atomic_store(&rp->render_running, false);
        playout_buffer_wake_all(&rp->pb);
        pthread_join(rp->render_thd, NULL);
        if (rp->audio_rx) mtl_rx_audio_stop(rp->audio_rx);
        return -1;
    }
    pin_thread(rp->dec_thd, rp->cfg.cpu_decode);
    return 0;
}

void rx_pipeline_stop(rx_pipeline* rp) {
    if (!rp) return;
    if (!atomic_exchange(&rp->running, false)) return;

    if (rp->audio_rx) mtl_rx_audio_stop(rp->audio_rx);

    if (rp->cfg.input_ring) ring_wake_all(rp->cfg.input_ring);
    if (rp->dec_thd) { pthread_join(rp->dec_thd, NULL); rp->dec_thd = 0; }

    atomic_store(&rp->render_running, false);
    playout_buffer_wake_all(&rp->pb);
    if (rp->render_thd) { pthread_join(rp->render_thd, NULL); rp->render_thd = 0; }

    struct codec_packet* pkt;
    while (ring_pop(rp->cfg.input_ring, (void**)&pkt) == 0) {
        if (rp->cfg.coded_pool && pkt->data)
            slab_free(rp->cfg.coded_pool, pkt->data);
        free(pkt);
    }

    if (rp->decoder)
        mpp_dec_print_stats(rp->decoder->priv);
    if (rp->h264)
        h264_prep_print_stats(rp->h264);

    float fps = rp->cfg.fps > 0 ? rp->cfg.fps : 24.0f;
    int test_s = rp->cfg.test_time > 0 ? rp->cfg.test_time : 60;
    double expected = (double)test_s * (double)fps;
    double fps_yield = expected > 0
                       ? (100.0 * (double)rp->stat_decoded / expected) : 0.0;
    double render_yield = expected > 0
                          ? (100.0 * (double)rp->stat_rendered / expected) : 0.0;
    uint64_t pb_drop = playout_buffer_overrun_drops(&rp->pb);

    printf("rx_pipe: %llu pkt, %llu dec (%.1f%% of ~%.0f @ %.3ffps), "
           "%llu pb_push, %llu render (%.1f%%), "
           "pb_skip=%llu pb_overrun=%llu pb_repeat=%llu pb_underrun=%llu, "
           "%llu put_fail, %llu prep_bad\n",
           (unsigned long long)rp->stat_received,
           (unsigned long long)rp->stat_decoded, fps_yield, expected, (double)fps,
           (unsigned long long)rp->stat_queued,
           (unsigned long long)rp->stat_rendered, render_yield,
           (unsigned long long)rp->stat_pb_skip,
           (unsigned long long)pb_drop,
           (unsigned long long)rp->stat_pb_repeated,
           (unsigned long long)rp->stat_pb_underrun,
           (unsigned long long)rp->stat_put_fail,
           (unsigned long long)rp->stat_prep_invalid);
    if (rp->alsa) av_sync_print_stats(&rp->sync);
}

void rx_pipeline_get_latency(const rx_pipeline* rp,
                             uint64_t* min_ns, uint64_t* avg_ns, uint64_t* max_ns) {
    if (!rp) return;
    uint64_t count = 0;
    rx_e2e_stats_get(&rp->e2e, min_ns, avg_ns, max_ns, &count);
}

uint64_t rx_pipeline_e2e_count(const rx_pipeline* rp) {
    uint64_t count = 0;
    if (rp)
        rx_e2e_stats_get(&rp->e2e, NULL, NULL, NULL, &count);
    return count;
}

void rx_pipeline_report_stats(rx_pipeline* rp, const mtl_rx_mp4* mrx,
                              const mtl_rx_bridge* bridge, int period_s,
                              bool reset_period) {
    if (!rp)
        return;

    uint64_t mtl_rx = 0, bridged = 0, stamp_ok = 0;
    uint32_t mtl_drops = 0, br_drops = 0, corrupt = 0, incomplete = 0;
    if (mrx)
        mtl_rx_mp4_get_stats(mrx, &mtl_rx, &mtl_drops, &corrupt, &incomplete);
    if (bridge)
        mtl_rx_bridge_get_stats(bridge, &bridged, &br_drops, &stamp_ok);

    uint64_t d_mtl = mtl_rx - rp->snap_received;
    uint64_t d_br  = bridged - rp->snap_bridged;
    uint64_t d_stamp = stamp_ok - rp->snap_stamp_ok;
    uint64_t d_dec = rp->stat_decoded - rp->snap_decoded;
    uint64_t d_ren = rp->stat_rendered - rp->snap_rendered;
    uint32_t d_mtl_drop = mtl_drops - rp->snap_mtl_drops;
    uint32_t d_br_drop  = br_drops - rp->snap_br_drops;

    printf("=== RX stats (+%ds) ===\n", period_s > 0 ? period_s : 0);
    rx_e2e_stats_print(&rp->e2e, rp->stats_label, true, d_stamp,
                       rp->cfg.time_now != NULL);
    printf("  RX[%s]: mtl_rx=+%llu bridge=+%llu dec=+%llu render=+%llu "
           "mtl_drop=+%u br_drop=+%u corrupt=%u incomplete=%u\n",
           rp->stats_label,
           (unsigned long long)d_mtl, (unsigned long long)d_br,
           (unsigned long long)d_dec, (unsigned long long)d_ren,
           d_mtl_drop, d_br_drop, corrupt, incomplete);
    printf("  RX[%s] total: pkt=%llu dec=%llu render=%llu pb_skip=%llu "
           "pb_underrun=%llu\n",
           rp->stats_label,
           (unsigned long long)rp->stat_received,
           (unsigned long long)rp->stat_decoded,
           (unsigned long long)rp->stat_rendered,
           (unsigned long long)rp->stat_pb_skip,
           (unsigned long long)rp->stat_pb_underrun);

    if (reset_period) {
        rx_e2e_stats_reset_period(&rp->e2e);
        rp->snap_received  = mtl_rx;
        rp->snap_bridged   = bridged;
        rp->snap_stamp_ok  = stamp_ok;
        rp->snap_decoded   = rp->stat_decoded;
        rp->snap_rendered  = rp->stat_rendered;
        rp->snap_mtl_drops = mtl_drops;
        rp->snap_br_drops  = br_drops;
    }
}
