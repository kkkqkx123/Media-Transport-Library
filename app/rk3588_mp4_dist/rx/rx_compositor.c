/* SPDX-License-Identifier: BSD-3-Clause
 *
 * RX compositor — multi-lane ST22 decode + DRM multi-plane display (Phase B).
 */

#define _GNU_SOURCE
#include "rx_compositor.h"
#include "mtl_rx_mp4.h"
#include "mtl_rx_bridge.h"
#include "mtl_rx_audio.h"
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

#define RX_RING_DEPTH      16
#define RX_ST22_FB_CNT     8
#define RX_CODED_POOL_CNT  24

typedef struct rx_video_lane {
    rx_compositor*        comp;
    mp4_dist_pip_stream   stream;
    int                   layer_idx;
    struct ring_buffer*   mtl_ring;
    struct ring_buffer*   dec_ring;
    struct slab_pool*     coded_pool;
    mtl_rx_mp4*           mrx;
    mtl_rx_bridge*        bridge;
    struct codec_plugin*  decoder;
    h264_prep*            h264;
    playout_buffer        pb;
    pthread_t             dec_thd;
    atomic_bool           running;
    uint64_t              frame_period_ns;
    uint64_t              stat_received;
    uint64_t              stat_decoded;
    uint64_t              last_rx_pts;
    rx_e2e_stats          e2e;
    uint64_t              snap_received;
    uint64_t              snap_decoded;
    uint64_t              snap_bridged;
    uint64_t              snap_stamp_ok;
    uint32_t              snap_mtl_drops;
    uint32_t              snap_br_drops;
    char                  stats_label[24];
} rx_video_lane;

struct rx_compositor {
    rx_compositor_params cfg;
    int                  lane_count;
    int                  main_lane_idx;
    display_layout       layout;
    struct drm_renderer* renderer;
    rx_video_lane        lanes[MP4_PIP_V1_NV12_LANES];
    av_sync              sync;
    alsa_audio*          alsa;
    mtl_rx_audio*        audio_rx;
    pthread_t            render_thd;
    atomic_bool          render_running;
    uint64_t             stat_composite;
};

static void mode_dims_from_str(const char* mode_str, int fallback_w, int fallback_h,
                               int* mw, int* mh) {
    int w = 0, h = 0, v = 0;
    if (mode_str && sscanf(mode_str, "%dx%d@%d", &w, &h, &v) >= 2 && w > 0 && h > 0) {
        *mw = w;
        *mh = h;
        return;
    }
    *mw = fallback_w > 0 ? fallback_w : 1920;
    *mh = fallback_h > 0 ? fallback_h : 1080;
}

static void pin_thread(pthread_t thd, int cpu) {
    if (cpu < 0) return;
    cpu_set_t cset;
    CPU_ZERO(&cset);
    CPU_SET(cpu, &cset);
    pthread_setaffinity_np(thd, sizeof(cset), &cset);
}

static int lane_dispatch_frame(rx_video_lane* lane, int out_fd, uint32_t out_h_stride,
                               uint32_t out_v_stride, uint64_t rx_timestamp,
                               uint64_t capture_tai_ns) {
    lane->stat_decoded++;
    playout_buffer_push(&lane->pb, out_fd, out_h_stride, out_v_stride, rx_timestamp,
                        capture_tai_ns);
    return 1;
}

static int lane_drain_decoder(rx_compositor* comp, rx_video_lane* lane, uint64_t au_pts,
                              uint64_t capture_tai_ns, int idle_rounds) {
    void* dec = lane->decoder->priv;
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
                frame_pts = au_pts + (uint64_t)au_frame_idx * lane->frame_period_ns;
            else if (lane->layer_idx == comp->main_lane_idx)
                frame_pts = av_sync_playout_target_ns(&comp->sync);
            else
                frame_pts = 0;
            lane_dispatch_frame(lane, out_fd, out_h_stride, out_v_stride, frame_pts,
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

static int lane_feed_packet(rx_compositor* comp, rx_video_lane* lane, uint8_t* data,
                            size_t size, uint64_t rx_timestamp, uint64_t capture_tai_ns) {
    const uint8_t* feed_data = data;
    size_t feed_size = size;
    int feed_owned = 0;
    void* dec = lane->decoder->priv;

    if (lane->h264) {
        if (h264_prep_for_mpp(lane->h264, data, size, &feed_data, &feed_size,
                              &feed_owned) < 0)
            return -1;
    }

    if (mpp_dec_uses_whole_au(dec)) {
        if (mpp_dec_put_whole(dec, (uint8_t*)feed_data, feed_size) < 0) {
            if (feed_owned) free((void*)feed_data);
            return -1;
        }
    } else if (mpp_dec_put(dec, (uint8_t*)feed_data, feed_size) < 0) {
        if (feed_owned) free((void*)feed_data);
        return -1;
    }

    int idle_rounds = mpp_dec_uses_aggressive_pull(dec) ? MPP_DEC_AGGRESSIVE_IDLE_ROUNDS : 0;
    int outputs = lane_drain_decoder(comp, lane, rx_timestamp, capture_tai_ns, idle_rounds);
    if (feed_owned) free((void*)feed_data);
    return outputs;
}

static void* decode_thread(void* arg) {
    rx_video_lane* lane = arg;
    rx_compositor* comp = lane->comp;
    void* dec = lane->decoder->priv;

    while (atomic_load(&lane->running)) {
        struct codec_packet* pkt = NULL;
        if (ring_pop_wait(lane->dec_ring, (void**)&pkt, &lane->running) != 0)
            continue;
        if (!pkt) continue;
        lane->stat_received++;
        lane->last_rx_pts = pkt->pts;
        lane_feed_packet(comp, lane, pkt->data, pkt->size, pkt->pts,
                         pkt->capture_tai_ns);
        if (lane->coded_pool && pkt->data)
            slab_free(lane->coded_pool, pkt->data);
        free(pkt);
    }

    if (mpp_dec_uses_aggressive_pull(dec)) {
        mpp_dec_send_eos(dec);
        lane_drain_decoder(comp, lane, lane->last_rx_pts, 0, MPP_DEC_EOS_FLUSH_IDLE_ROUNDS);
    } else {
        lane_drain_decoder(comp, lane, lane->last_rx_pts, 0, 0);
    }
    return NULL;
}

static uint64_t comp_time_now(rx_compositor* comp) {
    if (comp->cfg.time_now)
        return comp->cfg.time_now(comp->cfg.time_priv);
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    return (uint64_t)now_ts.tv_sec * 1000000000ULL + (uint64_t)now_ts.tv_nsec;
}

static void sleep_until_ns(rx_compositor* comp, uint64_t wake_ns) {
    for (int i = 0; i < 200; i++) {
        uint64_t now = comp_time_now(comp);
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
    rx_compositor* comp = arg;
    rx_video_lane* main_lane = &comp->lanes[comp->main_lane_idx];
    float fps = main_lane->stream.fps > 0 ? main_lane->stream.fps : 24.0f;
    uint64_t frame_period_ns = (uint64_t)(1000000000.0 / (double)fps);
    int wait_ms = (int)(frame_period_ns / 2000000ULL);
    if (wait_ms < 2) wait_ms = 2;
    if (wait_ms > 20) wait_ms = 20;

    drm_layer_frame frames[MP4_PIP_V1_NV12_LANES];
    uint64_t        lane_capture[MP4_PIP_V1_NV12_LANES];

    while (atomic_load(&comp->render_running)) {
        if (!playout_buffer_has_frames(&main_lane->pb)) {
            if (!playout_buffer_wait_data(&main_lane->pb, wait_ms))
                continue;
        }

        uint64_t target = av_sync_playout_target_ns(&comp->sync);
        uint64_t delay  = comp->sync.playout_delay_ns;
        uint64_t peek   = playout_buffer_peek_pts(&main_lane->pb);

        /* Same gate as rx_pipeline: wait until main frame is due (pts + delay). */
        if (peek > target) {
            sleep_until_ns(comp, peek + delay);
            continue;
        }

        int main_fd;
        uint32_t main_hs, main_vs;
        uint64_t main_cap = 0;
        int main_r = playout_buffer_pop_newest(&main_lane->pb, target, &main_fd,
                                               &main_hs, &main_vs, &main_cap);
        if (main_r != 1) {
            uint64_t now = comp_time_now(comp);
            sleep_until_ns(comp, now + frame_period_ns / 4);
            continue;
        }

        int frame_count = 0;
        frames[frame_count].layer_idx   = main_lane->layer_idx;
        frames[frame_count].dma_fd      = main_fd;
        frames[frame_count].hor_stride  = main_hs;
        frames[frame_count].ver_stride  = main_vs;
        frames[frame_count].content_w   = main_lane->stream.width;
        frames[frame_count].content_h   = main_lane->stream.height;
        lane_capture[frame_count]       = main_cap;
        frame_count++;

        for (int i = 0; i < comp->lane_count; i++) {
            if (i == comp->main_lane_idx)
                continue;
            rx_video_lane* lane = &comp->lanes[i];
            int fd;
            uint32_t hs, vs;
            uint64_t cap = 0;
            int r = playout_buffer_pop_newest(&lane->pb, UINT64_MAX, &fd, &hs, &vs, &cap);
            if (r == 0)
                continue;

            frames[frame_count].layer_idx   = lane->layer_idx;
            frames[frame_count].dma_fd      = fd;
            frames[frame_count].hor_stride  = hs;
            frames[frame_count].ver_stride  = vs;
            frames[frame_count].content_w   = lane->stream.width;
            frames[frame_count].content_h   = lane->stream.height;
            lane_capture[frame_count]       = cap;
            frame_count++;
        }

        if (drm_renderer_display_layers(comp->renderer, frames, frame_count) != 0)
            continue;

        comp->stat_composite++;
        if (comp->cfg.time_now) {
            uint64_t present = comp->cfg.time_now(comp->cfg.time_priv);
            for (int i = 0; i < frame_count; i++) {
                if (!lane_capture[i])
                    continue;
                int li = frames[i].layer_idx;
                if (li < 0 || li >= comp->lane_count)
                    continue;
                rx_e2e_stats_record(&comp->lanes[li].e2e, lane_capture[i], present,
                                    comp->lanes[li].stats_label);
            }
        }
    }
    return NULL;
}

static void lane_free(rx_video_lane* lane) {
    if (!lane) return;
    if (lane->bridge) mtl_rx_bridge_free(lane->bridge);
    if (lane->mrx) mtl_rx_mp4_free(lane->mrx);
    if (lane->coded_pool) slab_pool_free(lane->coded_pool);
    if (lane->dec_ring) ring_free(lane->dec_ring);
    if (lane->mtl_ring) ring_free(lane->mtl_ring);
    if (lane->decoder) {
        lane->decoder->free(lane->decoder->priv);
        free(lane->decoder);
    }
    if (lane->h264) h264_prep_free(lane->h264);
    rx_e2e_stats_destroy(&lane->e2e);
    playout_buffer_destroy(&lane->pb);
}

static int lane_init(rx_compositor* comp, int idx, const mp4_dist_pip_stream* stream,
                     mtl_handle mt, media_clk_time_fn time_fn, void* time_priv,
                     size_t coded_blk) {
    rx_video_lane* lane = &comp->lanes[idx];
    lane->comp      = comp;
    lane->stream    = *stream;
    lane->layer_idx = idx;

    float fps = stream->fps > 0 ? stream->fps : 24.0f;
    lane->frame_period_ns = (uint64_t)(1000000000.0 / (double)fps);

    size_t blk = stream->codestream_size > 0 ? stream->codestream_size : coded_blk;

    lane->mtl_ring = ring_create(RX_RING_DEPTH);
    lane->dec_ring = ring_create(RX_RING_DEPTH);
    lane->coded_pool = slab_pool_create(RX_CODED_POOL_CNT, blk);
    if (!lane->mtl_ring || !lane->dec_ring || !lane->coded_pool)
        return -1;

    mtl_rx_mp4_params mrxp = {
        .st                  = mt,
        .output_ring         = lane->mtl_ring,
        .framebuff_cnt       = RX_ST22_FB_CNT,
        .width               = stream->width,
        .height              = stream->height,
        .fps                 = fps,
        .udp_port            = stream->udp_port,
        .sip_addr            = stream->sip,
        .port_name           = comp->cfg.rx_port,
        .codestream_max_size = blk,
        .time_fn             = time_fn,
        .time_priv           = time_priv,
    };
    lane->mrx = mtl_rx_mp4_create(&mrxp);
    if (!lane->mrx)
        return -1;

    mtl_rx_bridge_params bp = {
        .mtl_ring   = lane->mtl_ring,
        .dec_ring   = lane->dec_ring,
        .rx_handle  = mtl_rx_mp4_handle(lane->mrx),
        .coded_pool = lane->coded_pool,
        .cpu        = comp->cfg.cpu_bridge,
    };
    lane->bridge = mtl_rx_bridge_create(&bp);
    if (!lane->bridge)
        return -1;

    struct codec_config codec_cfg = {
        .format      = CODEC_FMT_H264,
        .width       = stream->width,
        .height      = stream->height,
        .fps         = fps,
        .low_latency = true,
        .num_slices  = 1,
    };
    lane->decoder = mpp_decoder_create(&codec_cfg);
    if (!lane->decoder)
        return -1;

    lane->h264 = h264_prep_create();
    if (!lane->h264)
        return -1;

    rx_e2e_stats_init(&lane->e2e);
    snprintf(lane->stats_label, sizeof(lane->stats_label), "%u", stream->udp_port);

    playout_buffer_init(&lane->pb);
    return 0;
}

rx_compositor* rx_compositor_create(const rx_compositor_params* params) {
    if (!params || !params->streams || params->stream_count <= 0)
        return NULL;

    rx_compositor* comp = calloc(1, sizeof(*comp));
    if (!comp)
        return NULL;
    comp->cfg = *params;

    int mode_w, mode_h;
    mode_dims_from_str(params->drm_mode, params->streams[0].width,
                       params->streams[0].height, &mode_w, &mode_h);

    int n = display_layout_from_pip_streams(params->streams, params->stream_count,
                                            mode_w, mode_h, &comp->layout);
    if (n < 0) {
        free(comp);
        return NULL;
    }
    comp->lane_count = n;

    for (int i = 0; i < params->stream_count && i < n; i++) {
        if (params->streams[i].main)
            comp->main_lane_idx = i;
    }

    const mp4_dist_pip_stream* main_s = &params->streams[comp->main_lane_idx];
    comp->renderer = drm_renderer_create(&(drm_renderer_params){
        .device            = params->drm_device,
        .connector         = params->drm_connector,
        .mode_str          = params->drm_mode,
        .content_width     = main_s->width,
        .content_height    = main_s->height,
        .fourcc            = DRM_FORMAT_NV12,
        .buf_count         = params->drm_buf_count,
        .compositor_layout = &comp->layout,
    });
    if (!comp->renderer) {
        free(comp);
        return NULL;
    }

    media_clk_time_fn time_fn = NULL;
    void* time_priv = NULL;
    if (params->time_now) {
        time_fn   = (media_clk_time_fn)params->time_now;
        time_priv = params->time_priv;
    }

    for (int i = 0; i < comp->lane_count; i++) {
        if (lane_init(comp, i, &params->streams[i], params->mt,
                      time_fn, time_priv, params->default_codestream_size) < 0) {
            rx_compositor_free(comp);
            return NULL;
        }
    }

    if (alsa_audio_open(params->alsa_device, &comp->alsa) != 0) {
        fprintf(stderr, "rx_comp: ALSA open failed — continuing without audio\n");
        comp->alsa = NULL;
    }

    av_sync_init(&comp->sync, comp->alsa,
                 &comp->lanes[comp->main_lane_idx].pb, params->playout_delay_ms,
                 params->time_now, params->time_priv);

    uint16_t audio_port = main_s->audio_udp_port;
    if (!audio_port)
        audio_port = params->default_audio_udp_port;

    if (comp->alsa && params->mt && audio_port) {
        mtl_rx_audio_params arp = {
            .st             = params->mt,
            .rx_port        = params->rx_port,
            .rx_sip         = main_s->sip,
            .audio_udp_port = audio_port,
            .payload_type   = 98,
            .alsa           = comp->alsa,
            .sync           = &comp->sync,
            .cpu            = params->cpu_audio,
        };
        comp->audio_rx = mtl_rx_audio_create(&arp);
        if (!comp->audio_rx) {
            alsa_audio_close(comp->alsa);
            comp->alsa = NULL;
            av_sync_init(&comp->sync, NULL, &comp->lanes[comp->main_lane_idx].pb,
                         params->playout_delay_ms, params->time_now,
                         params->time_priv);
        }
    }

    printf("rx_comp: %d lane(s) main=%d delay=%dms audio=%s\n", comp->lane_count,
           comp->main_lane_idx, params->playout_delay_ms,
           comp->alsa ? "on" : "off");
    return comp;
}

int rx_compositor_start(rx_compositor* comp) {
    if (!comp)
        return -1;

    atomic_store(&comp->render_running, true);
    for (int i = 0; i < comp->lane_count; i++)
        atomic_store(&comp->lanes[i].running, true);

    if (comp->audio_rx && mtl_rx_audio_start(comp->audio_rx) != 0)
        return -1;

    if (pthread_create(&comp->render_thd, NULL, render_thread, comp) != 0)
        return -1;
    pin_thread(comp->render_thd, comp->cfg.cpu_render);

    for (int i = 0; i < comp->lane_count; i++) {
        rx_video_lane* lane = &comp->lanes[i];
        if (pthread_create(&lane->dec_thd, NULL, decode_thread, lane) != 0)
            return -1;
        pin_thread(lane->dec_thd, comp->cfg.cpu_decode + i);
        if (mtl_rx_bridge_start(lane->bridge) != 0)
            return -1;
        mtl_rx_mp4_start(lane->mrx);
    }
    return 0;
}

void rx_compositor_stop(rx_compositor* comp) {
    if (!comp)
        return;

    for (int i = 0; i < comp->lane_count; i++) {
        rx_video_lane* lane = &comp->lanes[i];
        if (!atomic_exchange(&lane->running, false))
            continue;
        mtl_rx_mp4_begin_stop(lane->mrx);
        mtl_rx_bridge_stop(lane->bridge);
        mtl_rx_mp4_stop(lane->mrx);
        ring_wake_all(lane->dec_ring);
        if (lane->dec_thd)
            pthread_join(lane->dec_thd, NULL);
        lane->dec_thd = 0;
    }

    if (comp->audio_rx)
        mtl_rx_audio_stop(comp->audio_rx);

    if (atomic_exchange(&comp->render_running, false)) {
        playout_buffer_wake_all(&comp->lanes[comp->main_lane_idx].pb);
        if (comp->render_thd)
            pthread_join(comp->render_thd, NULL);
        comp->render_thd = 0;
    }

    printf("rx_comp: composite=%llu frames\n",
           (unsigned long long)comp->stat_composite);
    if (comp->alsa)
        av_sync_print_stats(&comp->sync);
}

void rx_compositor_free(rx_compositor* comp) {
    if (!comp)
        return;
    rx_compositor_stop(comp);
    if (comp->audio_rx)
        mtl_rx_audio_free(comp->audio_rx);
    if (comp->alsa)
        alsa_audio_close(comp->alsa);
    if (comp->renderer)
        drm_renderer_free(comp->renderer);
    for (int i = 0; i < comp->lane_count; i++)
        lane_free(&comp->lanes[i]);
    pthread_mutex_destroy(&comp->sync.lock);
    free(comp);
}

void rx_compositor_get_latency(const rx_compositor* comp, uint64_t* min_ns,
                               uint64_t* avg_ns, uint64_t* max_ns) {
    if (!comp)
        return;
    rx_video_lane* main = &comp->lanes[comp->main_lane_idx];
    uint64_t count = 0;
    rx_e2e_stats_get(&main->e2e, min_ns, avg_ns, max_ns, &count);
}

uint64_t rx_compositor_e2e_count(const rx_compositor* comp) {
    if (!comp)
        return 0;
    uint64_t total = 0;
    for (int i = 0; i < comp->lane_count; i++) {
        uint64_t c = 0;
        rx_e2e_stats_get(&comp->lanes[i].e2e, NULL, NULL, NULL, &c);
        total += c;
    }
    return total;
}

void rx_compositor_report_stats(rx_compositor* comp, int period_s, bool reset_period) {
    if (!comp)
        return;

    printf("=== RX compositor stats (+%ds) ===\n", period_s > 0 ? period_s : 0);
    for (int i = 0; i < comp->lane_count; i++) {
        rx_video_lane* lane = &comp->lanes[i];
        uint64_t mtl_rx = 0, bridged = 0, stamp_ok = 0;
        uint32_t mtl_drops = 0, br_drops = 0, corrupt = 0, incomplete = 0;

        if (lane->mrx)
            mtl_rx_mp4_get_stats(lane->mrx, &mtl_rx, &mtl_drops, &corrupt, &incomplete);
        if (lane->bridge)
            mtl_rx_bridge_get_stats(lane->bridge, &bridged, &br_drops, &stamp_ok);

        uint64_t d_mtl = mtl_rx - lane->snap_received;
        uint64_t d_br  = bridged - lane->snap_bridged;
        uint64_t d_stamp = stamp_ok - lane->snap_stamp_ok;
        uint64_t d_dec = lane->stat_decoded - lane->snap_decoded;
        uint32_t d_mtl_drop = mtl_drops - lane->snap_mtl_drops;
        uint32_t d_br_drop  = br_drops - lane->snap_br_drops;

        rx_e2e_stats_print(&lane->e2e, lane->stats_label, true, d_stamp,
                           comp->cfg.time_now != NULL);
        printf("  RX[%s]: mtl_rx=+%llu bridge=+%llu dec=+%llu "
               "mtl_drop=+%u br_drop=+%u corrupt=%u incomplete=%u\n",
               lane->stats_label,
               (unsigned long long)d_mtl, (unsigned long long)d_br,
               (unsigned long long)d_dec, d_mtl_drop, d_br_drop, corrupt,
               incomplete);
        printf("  RX[%s] total: pkt=%llu dec=%llu\n",
               lane->stats_label,
               (unsigned long long)lane->stat_received,
               (unsigned long long)lane->stat_decoded);

        if (reset_period) {
            rx_e2e_stats_reset_period(&lane->e2e);
            lane->snap_received  = mtl_rx;
            lane->snap_bridged   = bridged;
            lane->snap_stamp_ok  = stamp_ok;
            lane->snap_decoded   = lane->stat_decoded;
            lane->snap_mtl_drops = mtl_drops;
            lane->snap_br_drops  = br_drops;
        }
    }
    printf("  composite_frames=%llu\n", (unsigned long long)comp->stat_composite);
}
