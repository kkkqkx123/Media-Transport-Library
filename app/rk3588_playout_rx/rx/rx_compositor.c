#include "rx_compositor.h"
#include "rx_decode_common.h"
#include "mtl_rx_mp4.h"
#include "mtl_rx_bridge.h"
#include "mtl_rx_audio.h"
#include "h264_prep.h"
#include "alsa_audio.h"
#include "playout_buffer.h"
#include "av_sync.h"
#include "pipeline_common.h"
#include "ring_buffer.h"
#include "slab_pool.h"
#include "codec_plugin.h"
#include "../shared/mpp_decoder.h"
#include "../shared/drm_renderer.h"
#include "rx_e2e_stats.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RX_RING_DEPTH      16
#define RX_ST22_FB_CNT     8
#define RX_CODED_POOL_CNT  24

/* Lane considered stale when no PTS progress within this window. */
#define RX_LANE_READY_TIMEOUT_NS (3000000000ULL)

typedef struct rx_video_lane {
    rx_compositor*        comp;
    file_playout_pip_stream   stream;
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
    uint64_t              last_progress_mono_ns;
    /* per-frame decode latency: feed -> first displayable frame of that AU.
     * Single-writer (lane decode thread); window lives in rx_decode_common. */
    rx_dec_lat            dec_lat;
    uint64_t              dec_feed_start_ns;
    bool                  dec_first_pending;
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
    /* Per-composite display cost (display_layers only, monotonic ns). */
    uint64_t             stat_render_ns_sum;
    uint64_t             stat_render_ns_max;
    uint64_t             stat_render_ns_min;
    uint64_t             stat_render_count;
    uint64_t             stat_render_slow;
    /* Newest-drop skips: decoded but never shown (main vs sub lanes). */
    uint64_t             stat_skip_main;
    uint64_t             stat_skip_sub;
    /* Period snapshots for delta reporting. */
    uint64_t             snap_composite;
    uint64_t             snap_render_sum;
    uint64_t             snap_render_count;
    uint64_t             snap_render_slow;
    uint64_t             snap_skip_main;
    uint64_t             snap_skip_sub;
};

/* Slow-composite threshold: one 60fps period is 16.6ms; 25ms marks a round
 * that cannot sustain 40fps and deserves a slow tick in the report. */
#define RX_COMP_RENDER_SLOW_NS (25000000ULL)

static void comp_note_render(rx_compositor* comp, uint64_t ns) {
    comp->stat_render_ns_sum += ns;
    comp->stat_render_count++;
    if (comp->stat_render_count == 1 || ns < comp->stat_render_ns_min)
        comp->stat_render_ns_min = ns;
    if (ns > comp->stat_render_ns_max)
        comp->stat_render_ns_max = ns;
    if (ns > RX_COMP_RENDER_SLOW_NS)
        comp->stat_render_slow++;
}

static uint64_t comp_timespec_delta_ns(const struct timespec* a,
                                       const struct timespec* b) {
    uint64_t sa = (uint64_t)a->tv_sec * 1000000000ULL + (uint64_t)a->tv_nsec;
    uint64_t sb = (uint64_t)b->tv_sec * 1000000000ULL + (uint64_t)b->tv_nsec;
    return sb >= sa ? sb - sa : 0;
}

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

static uint64_t lane_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Shared-drain frame sink: count, latency-sample the first frame per feed,
 * then queue for render. pts 0 (sub-lane, no au stamp) rides through for
 * newest-frame pop. */
static void lane_dispatch_cb(int out_fd, uint32_t out_h_stride,
                             uint32_t out_v_stride, uint64_t frame_pts,
                             uint64_t capture_tai_ns, void* arg) {
    rx_video_lane* lane = (rx_video_lane*)arg;
    lane->stat_decoded++;
    if (lane->dec_first_pending) {
        rx_dec_lat_record(&lane->dec_lat, lane->dec_feed_start_ns);
        lane->dec_first_pending = false;
    }
    playout_buffer_push(&lane->pb, out_fd, out_h_stride, out_v_stride,
                        frame_pts, capture_tai_ns);
}

/* Fallback pts when the AU carries no stamp: main lane waits on the playout
 * target, sub lanes pop newest regardless. Computed once per feed (per-frame
 * values would differ by less than one decode service; absorbed by the
 * playout delay and invisible to E2E-C, which keys off capture_tai_ns). */
static uint64_t lane_fallback_pts(rx_compositor* comp, rx_video_lane* lane) {
    if (lane->layer_idx == comp->main_lane_idx)
        return av_sync_playout_target_ns(&comp->sync);
    return 0;
}

static int lane_feed_packet(rx_compositor* comp, rx_video_lane* lane, uint8_t* data,
                            size_t size, uint64_t rx_timestamp, uint64_t capture_tai_ns) {
    lane->dec_feed_start_ns = lane_now_ns();
    lane->dec_first_pending = true;
    return rx_decode_feed_and_drain(lane->decoder->priv, lane->h264, data,
                                    size, rx_timestamp, lane->frame_period_ns,
                                    lane_fallback_pts(comp, lane), capture_tai_ns,
                                    0, lane_dispatch_cb, lane, NULL, NULL);
}

static void* decode_thread(void* arg) {
    rx_video_lane* lane = arg;
#if defined(__linux__) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "rx_dec");
#endif
    rx_compositor* comp = lane->comp;
    void* dec = lane->decoder->priv;

    while (atomic_load(&lane->running)) {
        struct codec_packet* pkt = NULL;
        if (ring_pop_wait(lane->dec_ring, (void**)&pkt, &lane->running) != 0)
            continue;
        if (!pkt) continue;
        lane->stat_received++;
        lane->last_rx_pts = pkt->pts;
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            lane->last_progress_mono_ns =
                (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        }
        lane_feed_packet(comp, lane, pkt->data, pkt->size, pkt->pts,
                         pkt->capture_tai_ns);
        if (lane->coded_pool && pkt->data)
            slab_free(lane->coded_pool, pkt->data);
        free(pkt);
    }

    if (mpp_dec_uses_aggressive_pull(dec)) {
        mpp_dec_send_eos(dec);
        rx_decode_drain(dec, lane->last_rx_pts, lane->frame_period_ns,
                        lane_fallback_pts(comp, lane), 0,
                        MPP_DEC_EOS_FLUSH_IDLE_ROUNDS, lane_dispatch_cb, lane);
    } else {
        rx_decode_drain(dec, lane->last_rx_pts, lane->frame_period_ns,
                        lane_fallback_pts(comp, lane), 0, 0, lane_dispatch_cb,
                        lane);
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
#if defined(__linux__) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "rx_render");
#endif
    if (comp->lane_count == 0) {
        /* Backdrop-only: no frames to composite. Reassert the background
         * CRTC periodically so the still (or black) survives display
         * hiccups; planes stay empty by construction. */
        while (atomic_load(&comp->render_running)) {
            if (drm_renderer_show_background(comp->renderer) != 0)
                break;
            struct timespec idle = {.tv_sec = 0, .tv_nsec = 200000000L};
            nanosleep(&idle, NULL);
        }
        return NULL;
    }
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
        int dropped_main = 0;
        int main_r = playout_buffer_pop_newest_counted(&main_lane->pb, target,
                                                       &main_fd, &main_hs,
                                                       &main_vs, &main_cap,
                                                       &dropped_main);
        if (dropped_main > 0)
            comp->stat_skip_main += (uint64_t)dropped_main;
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
        frames[frame_count].src_x       = main_lane->stream.src_x;
        frames[frame_count].src_y       = main_lane->stream.src_y;
        frames[frame_count].src_w       = main_lane->stream.src_w;
        frames[frame_count].src_h       = main_lane->stream.src_h;
        lane_capture[frame_count]       = main_cap;
        frame_count++;

        for (int i = 0; i < comp->lane_count; i++) {
            if (i == comp->main_lane_idx)
                continue;
            rx_video_lane* lane = &comp->lanes[i];
            int fd;
            uint32_t hs, vs;
            uint64_t cap = 0;
            int dropped = 0;
            int r = playout_buffer_pop_newest_counted(&lane->pb, UINT64_MAX,
                                                      &fd, &hs, &vs, &cap,
                                                      &dropped);
            if (dropped > 0)
                comp->stat_skip_sub += (uint64_t)dropped;
            if (r == 0)
                continue;

            frames[frame_count].layer_idx   = lane->layer_idx;
            frames[frame_count].dma_fd      = fd;
            frames[frame_count].hor_stride  = hs;
            frames[frame_count].ver_stride  = vs;
            frames[frame_count].content_w   = lane->stream.width;
            frames[frame_count].content_h   = lane->stream.height;
            frames[frame_count].src_x       = lane->stream.src_x;
            frames[frame_count].src_y       = lane->stream.src_y;
            frames[frame_count].src_w       = lane->stream.src_w;
            frames[frame_count].src_h       = lane->stream.src_h;
            lane_capture[frame_count]       = cap;
            frame_count++;
        }

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int disp_rc = drm_renderer_display_layers(comp->renderer, frames,
                                                  frame_count);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        comp_note_render(comp, comp_timespec_delta_ns(&t0, &t1));
        if (disp_rc != 0)
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
    rx_dec_lat_destroy(&lane->dec_lat);
    playout_buffer_destroy(&lane->pb);
}

static int lane_init(rx_compositor* comp, int idx, const file_playout_pip_stream* stream,
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
        .src_filter          = stream->src_ip[0] ? stream->src_ip : NULL,
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
        .format      = comp->cfg.codec,
        .width       = stream->width,
        .height      = stream->height,
        .fps         = fps,
        .low_latency = true,
        .num_slices  = 1,
    };
    lane->decoder = mpp_decoder_create(&codec_cfg);
    if (!lane->decoder)
        return -1;

    lane->h264 = h264_prep_create_for_codec(comp->cfg.codec);
    if (!lane->h264)
        return -1;

    rx_e2e_stats_init(&lane->e2e);
    rx_dec_lat_init(&lane->dec_lat);
    snprintf(lane->stats_label, sizeof(lane->stats_label), "%u", stream->udp_port);

    playout_buffer_init(&lane->pb);
    return 0;
}

rx_compositor* rx_compositor_create(const rx_compositor_params* params) {
    if (!params)
        return NULL;
    int stream_count = params->stream_count;
    if (stream_count < 0 || stream_count > MP4_PIP_MAX_LANES)
        return NULL;
    const file_playout_pip_stream* streams = params->streams;
    if (stream_count > 0 && !streams)
        return NULL;

    rx_compositor* comp = calloc(1, sizeof(*comp));
    if (!comp)
        return NULL;
    comp->cfg = *params;

    int mode_w, mode_h;
    /* Backdrop-only (zero lanes): no stream to size from, use the mode. */
    mode_dims_from_str(params->drm_mode,
                       stream_count > 0 ? streams[0].width : 1920,
                       stream_count > 0 ? streams[0].height : 1080,
                       &mode_w, &mode_h);

    const file_playout_pip_stream* main_s = NULL;
    if (stream_count == 0) {
        memset(&comp->layout, 0, sizeof(comp->layout));
        comp->layout.mode_w = mode_w;
        comp->layout.mode_h = mode_h;
        comp->layout.compositor = true;
        comp->lane_count = 0;
        comp->main_lane_idx = 0;
    } else {
        int n = display_layout_from_pip_streams(streams, stream_count,
                                                mode_w, mode_h, &comp->layout);
        if (n < 0) {
            free(comp);
            return NULL;
        }
        comp->lane_count = n;

        for (int i = 0; i < stream_count && i < n; i++) {
            if (streams[i].main)
                comp->main_lane_idx = i;
        }
        main_s = &streams[comp->main_lane_idx];
    }
    comp->renderer = drm_renderer_create(&(drm_renderer_params){
        .device            = params->drm_device,
        .connector         = params->drm_connector,
        .mode_str          = params->drm_mode,
        .content_width     = main_s ? main_s->width : mode_w,
        .content_height    = main_s ? main_s->height : mode_h,
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
        if (lane_init(comp, i, &streams[i], params->mt,
                      time_fn, time_priv, params->default_codestream_size) < 0) {
            rx_compositor_free(comp);
            return NULL;
        }
    }

    if (comp->lane_count == 0) {
        /* Backdrop-only: no lanes, no audio subscription. The sync clock
         * still initializes with no frame source. */
        av_sync_init(&comp->sync, NULL, NULL, params->playout_delay_ms,
                     params->time_now, params->time_priv);
        printf("rx_comp: backdrop-only (0 lanes)\n");
        return comp;
    }

    uint16_t audio_port = main_s->audio_udp_port;
    if (!audio_port)
        audio_port = params->default_audio_udp_port;

    av_sync_init(&comp->sync, NULL,
                 &comp->lanes[comp->main_lane_idx].pb, params->playout_delay_ms,
                 params->time_now, params->time_priv);

    if (audio_port != 0) {
        if (alsa_audio_open(params->alsa_device, &comp->alsa) != 0) {
            fprintf(stderr, "rx_comp: ALSA open failed (audio requested)\n");
            rx_compositor_free(comp);
            return NULL;
        }
        comp->sync.audio = comp->alsa;
    } else {
        comp->alsa = NULL;
    }

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
            fprintf(stderr, "rx_comp: mtl_rx_audio create failed\n");
            rx_compositor_free(comp);
            return NULL;
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
        if (comp->lane_count > 0)
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

int rx_compositor_try_fast_apply(rx_compositor* comp,
                                 const file_playout_pip_stream* streams,
                                 int count) {
    if (!comp || !streams || count <= 0) return -1;
    if (count != comp->lane_count) return 1;
    for (int i = 0; i < count; i++) {
        const file_playout_pip_stream* a = &comp->lanes[i].stream;
        const file_playout_pip_stream* b = &streams[i];
        if (strncmp(a->sip, b->sip, sizeof(a->sip)) != 0) return 1;
        if (a->udp_port != b->udp_port) return 1;
        if (a->width != b->width || a->height != b->height) return 1;
        /* Source filter selects the multicast sender; a change rejoins the
         * session, so it forces the shadow path like any source change. */
        if (strncmp(a->src_ip, b->src_ip, sizeof(a->src_ip)) != 0) return 1;
        /* Source crop is a plane attribute consumed per frame, so it rides
         * the fast layout update below instead of forcing a rebuild. */
    }
    int mode_w = comp->layout.mode_w;
    int mode_h = comp->layout.mode_h;
    display_layout next;
    if (display_layout_from_pip_streams(streams, count, mode_w, mode_h,
                                        &next) < 0)
        return -1;
    if (drm_renderer_update_layout(comp->renderer, &next) != 0) return 1;
    comp->layout = next;
    for (int i = 0; i < count; i++) {
        comp->lanes[i].stream.x = streams[i].x;
        comp->lanes[i].stream.y = streams[i].y;
        comp->lanes[i].stream.w = streams[i].w;
        comp->lanes[i].stream.h = streams[i].h;
        comp->lanes[i].stream.src_x = streams[i].src_x;
        comp->lanes[i].stream.src_y = streams[i].src_y;
        comp->lanes[i].stream.src_w = streams[i].src_w;
        comp->lanes[i].stream.src_h = streams[i].src_h;
        comp->lanes[i].stream.main = streams[i].main;
        comp->lanes[i].stream.lane = streams[i].lane;
    }
    return 0;
}

void rx_compositor_get_latency(const rx_compositor* comp, uint64_t* min_ns,
                               uint64_t* avg_ns, uint64_t* max_ns) {
    if (!comp)
        return;
    const rx_video_lane* main = &comp->lanes[comp->main_lane_idx];
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

int rx_compositor_lane_count(const rx_compositor* comp) {
    if (!comp) return 0;
    return comp->lane_count;
}

const file_playout_pip_stream* rx_compositor_lane_stream(const rx_compositor* comp,
                                                          int idx) {
    if (!comp || idx < 0 || idx >= comp->lane_count) return NULL;
    return &comp->lanes[idx].stream;
}

int rx_compositor_lane_running(const rx_compositor* comp, int idx) {
    if (!comp || idx < 0 || idx >= comp->lane_count) return 0;
    return atomic_load(&comp->lanes[idx].running) ? 1 : 0;
}

static uint64_t comp_mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int rx_compositor_lane_ready(const rx_compositor* comp, int idx) {
    if (!comp || idx < 0 || idx >= comp->lane_count) return 0;
    const rx_video_lane* lane = &comp->lanes[idx];
    if (!atomic_load(&lane->running)) return 0;
    if (lane->last_progress_mono_ns == 0) return 0;
    uint64_t now = comp_mono_now();
    if (now < lane->last_progress_mono_ns) return 1;
    return (now - lane->last_progress_mono_ns) <= RX_LANE_READY_TIMEOUT_NS;
}

int rx_compositor_ready_count(const rx_compositor* comp) {
    if (!comp) return 0;
    int n = 0;
    for (int i = 0; i < comp->lane_count; i++)
        n += rx_compositor_lane_ready(comp, i) ? 1 : 0;
    return n;
}

int rx_compositor_layer_count(const rx_compositor* comp) {
    if (!comp) return 0;
    return comp->layout.layer_count;
}

int rx_compositor_audio_running(const rx_compositor* comp) {
    if (!comp) return 0;
    return comp->audio_rx ? 1 : 0;
}

int rx_compositor_set_background(rx_compositor* comp, const uint8_t* px,
                                  int w, int h) {
    if (!comp || !comp->renderer) return -1;
    return drm_renderer_set_background(comp->renderer, px, w, h);
}

int rx_compositor_audio_select(rx_compositor* comp, const char* sip,
                               uint16_t audio_port) {
    if (!comp) return -1;
    if (comp->lane_count == 0)
        return (!sip || !sip[0] || audio_port == 0) ? 0 : -1;
    if (!sip || !sip[0] || audio_port == 0) {
        if (comp->audio_rx) {
            mtl_rx_audio_stop(comp->audio_rx);
            mtl_rx_audio_free(comp->audio_rx);
            comp->audio_rx = NULL;
        }
        return 0;
    }
    if (!comp->alsa || !comp->cfg.mt) return -1;
    mtl_rx_audio_params arp = {
        .st             = comp->cfg.mt,
        .rx_port        = comp->cfg.rx_port,
        .rx_sip         = sip,
        .audio_udp_port = audio_port,
        .payload_type   = 98,
        .alsa           = comp->alsa,
        .sync           = &comp->sync,
        .cpu            = comp->cfg.cpu_audio,
    };
    mtl_rx_audio* fresh = mtl_rx_audio_create(&arp);
    if (!fresh) return -1;
    if (mtl_rx_audio_start(fresh) != 0) {
        mtl_rx_audio_free(fresh);
        return -1;
    }
    if (comp->audio_rx) {
        mtl_rx_audio_stop(comp->audio_rx);
        mtl_rx_audio_free(comp->audio_rx);
    }
    comp->audio_rx = fresh;
    return 0;
}

unsigned long long rx_compositor_lane_lost_frames(const rx_compositor* comp, int idx) {
    if (!comp || idx < 0 || idx >= comp->lane_count) return 0;
    uint64_t received = 0;
    uint32_t drops = 0, corrupt = 0, incomplete = 0;
    mtl_rx_mp4_get_stats(comp->lanes[idx].mrx, &received, &drops, &corrupt,
                         &incomplete);
    return (unsigned long long)(drops + corrupt + incomplete);
}

unsigned long long rx_compositor_lane_last_pts_ns(const rx_compositor* comp, int idx) {
    if (!comp || idx < 0 || idx >= comp->lane_count) return 0;
    return (unsigned long long)comp->lanes[idx].last_rx_pts;
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
        uint32_t dec_p50 = 0, dec_p99 = 0;
        rx_dec_lat_percentiles(&lane->dec_lat, &dec_p50, &dec_p99);

        rx_e2e_stats_print(&lane->e2e, lane->stats_label, true, d_stamp,
                           comp->cfg.time_now != NULL);
        printf("  RX[%s]: mtl_rx=+%llu bridge=+%llu dec=+%llu "
               "mtl_drop=+%u br_drop=+%u corrupt=%u incomplete=%u "
               "dec_p50=%uus dec_p99=%uus\n",
               lane->stats_label,
               (unsigned long long)d_mtl, (unsigned long long)d_br,
               (unsigned long long)d_dec, d_mtl_drop, d_br_drop, corrupt,
               incomplete, dec_p50, dec_p99);
        uint64_t overrun = playout_buffer_overrun_drops(&lane->pb);
        printf("  RX[%s] total: pkt=%llu dec=%llu overrun=%llu\n",
               lane->stats_label,
               (unsigned long long)lane->stat_received,
               (unsigned long long)lane->stat_decoded,
               (unsigned long long)overrun);
        if (lane->bridge) {
            uint32_t work_ema = 0, wait_ema = 0, work_p50 = 0, work_p99 = 0;
            int ring_max = 0;
            mtl_rx_bridge_get_timing(lane->bridge, &work_ema, &wait_ema,
                                     &work_p50, &work_p99, &ring_max);
            printf("  BR[%s]: work_ema=%uus work_p50=%uus work_p99=%uus "
                   "wait_ema=%uus ring_max=%d\n",
                   lane->stats_label, work_ema, work_p50, work_p99, wait_ema,
                   ring_max);
        }

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
    /* Render cost baseline: period avg distinguishes a slow submit path from
     * intentional newest-drop skips (skip_main/sub below). */
    uint64_t d_comp = comp->stat_composite - comp->snap_composite;
    uint64_t d_sum  = comp->stat_render_ns_sum - comp->snap_render_sum;
    uint64_t d_cnt  = comp->stat_render_count - comp->snap_render_count;
    uint64_t d_slow = comp->stat_render_slow - comp->snap_render_slow;
    uint64_t d_skip_m = comp->stat_skip_main - comp->snap_skip_main;
    uint64_t d_skip_s = comp->stat_skip_sub - comp->snap_skip_sub;
    double period_avg_ms = d_cnt ? (double)d_sum / (double)d_cnt / 1e6 : 0.0;
    double total_avg_ms = comp->stat_render_count
        ? (double)comp->stat_render_ns_sum / (double)comp->stat_render_count /
              1e6
        : 0.0;
    printf("  composite_frames=+%llu total=%llu\n",
           (unsigned long long)d_comp,
           (unsigned long long)comp->stat_composite);
    printf("  render: period_cnt=%llu period_avg=%.3fms total_avg=%.3fms "
           "max=%.3fms slow(>25ms)=+%llu total_slow=%llu\n",
           (unsigned long long)d_cnt, period_avg_ms, total_avg_ms,
           (double)comp->stat_render_ns_max / 1e6,
           (unsigned long long)d_slow,
           (unsigned long long)comp->stat_render_slow);
    printf("  skip_newest: main=+%llu sub=+%llu total_main=%llu total_sub=%llu\n",
           (unsigned long long)d_skip_m, (unsigned long long)d_skip_s,
           (unsigned long long)comp->stat_skip_main,
           (unsigned long long)comp->stat_skip_sub);
    if (reset_period) {
        comp->snap_composite    = comp->stat_composite;
        comp->snap_render_sum   = comp->stat_render_ns_sum;
        comp->snap_render_count = comp->stat_render_count;
        comp->snap_render_slow  = comp->stat_render_slow;
        comp->snap_skip_main    = comp->stat_skip_main;
        comp->snap_skip_sub     = comp->stat_skip_sub;
    }
}
