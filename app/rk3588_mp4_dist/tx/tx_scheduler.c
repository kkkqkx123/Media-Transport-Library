/* SPDX-License-Identifier: BSD-3-Clause
 *
 * TX Scheduler — session pull model: N video lanes + optional audio on main lane.
 * MTL get_next_frame drives wire pacing (no PTS sleep, no TX rings).
 */

#define _GNU_SOURCE
#include "tx_scheduler.h"
#include "mp4_playlist.h"
#include "extradata_handler.h"
#include "mtl_tx_video.h"
#include "../shared/au_trace.h"
#include "../shared/e2e_stamp.h"
#include "mtl_tx_audio.h"
#include <errno.h>
#include <libavutil/avutil.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct tx_video_lane {
    mp4_dist_tx_stream   spec;
    mp4_playlist*        playlist;
    extradata_handler*   ext_hdl;
    struct mtl_tx_video* vtx;
    pthread_t            video_tid;
    uint64_t             stat_video_frames;
};

struct tx_scheduler {
    mtl_handle          mt;
    mp4_dist_cfg        cfg;
    int                 lane_count;
    int                 main_lane_idx;
    struct tx_video_lane lanes[MP4_PIP_MAX_LANES];

    struct mtl_tx_audio* atx;

    /* audio decode (audio producer thread only) */
    AVCodecContext*     aac_ctx;
    AVFrame*            aac_frame;
    bool                has_audio;

    pthread_t           audio_tid;
    bool                threads_started;

    uint64_t            stat_audio_frames;
};

typedef struct {
    tx_scheduler*        sched;
    struct tx_video_lane* lane;
    atomic_bool*         running;
} tx_lane_thread_args;

static void aac_decode_init(tx_scheduler* s, mp4_demux_ctx* demux) {
    const mp4_stream_info* info = mp4_demux_get_audio_stream(demux);
    const AVCodecParameters* par = mp4_demux_get_audio_codecpar(demux);
    if (!info || !par) return;

    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        fprintf(stderr, "tx_sched: AAC decoder not found\n");
        return;
    }

    s->aac_ctx = avcodec_alloc_context3(codec);
    if (!s->aac_ctx) return;

    if (avcodec_parameters_to_context(s->aac_ctx, par) < 0) {
        avcodec_free_context(&s->aac_ctx);
        return;
    }
    s->aac_ctx->request_sample_fmt = AV_SAMPLE_FMT_S16;

    if (avcodec_open2(s->aac_ctx, codec, NULL) < 0) {
        avcodec_free_context(&s->aac_ctx);
        return;
    }

    s->aac_frame = av_frame_alloc();
    s->has_audio = true;
    printf("tx_sched: AAC decoder ready (%dHz %dch)\n",
           info->sample_rate, info->channels);
}

static void aac_decode_close(tx_scheduler* s) {
    if (s->aac_frame) av_frame_free(&s->aac_frame);
    if (s->aac_ctx)   avcodec_free_context(&s->aac_ctx);
}

static int aac_decode_packet(tx_scheduler* s, AVPacket* pkt,
                             uint8_t** pcm_out, size_t* pcm_size) {
    if (!s->has_audio) return -ENOSYS;

    int ret = avcodec_send_packet(s->aac_ctx, pkt);
    if (ret < 0) return ret;

    ret = avcodec_receive_frame(s->aac_ctx, s->aac_frame);
    if (ret < 0) return ret;

    int samples = s->aac_frame->nb_samples;
    int ch      = s->aac_ctx->channels;
    size_t bytes = (size_t)samples * (size_t)ch * 2;
    uint8_t* buf = malloc(bytes);
    if (!buf) return -ENOMEM;

    if (av_sample_fmt_is_planar(s->aac_ctx->sample_fmt)) {
        int16_t* dst = (int16_t*)buf;
        for (int c = 0; c < ch; c++) {
            int16_t* src = ((int16_t*)s->aac_frame->data[c]);
            for (int i = 0; i < samples; i++)
                dst[i * ch + c] = src[i];
        }
    } else {
        memcpy(buf, s->aac_frame->data[0], bytes);
    }

    *pcm_out  = buf;
    *pcm_size = bytes;
    return 0;
}

static uint64_t tx_capture_tai_ns(tx_scheduler* s) {
    if (s->cfg.ptp_mode != MP4_DIST_PTP_MONOTONIC && s->mt)
        return mtl_ptp_read_time(s->mt);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int tx_publish_video_frame(tx_scheduler* s, struct tx_video_lane* lane,
                                  size_t codestream_max, const uint8_t* data,
                                  size_t size, int64_t demux_pts) {
    uint16_t idx;
    void*    addr;
    int ret = mtl_tx_video_acquire_fb(lane->vtx, &idx, &addr);
    if (ret < 0) return ret;

    size_t stamp_sz = e2e_stamp_wire_size();
    if (size + stamp_sz > codestream_max) {
        mtl_tx_video_abort_fb(lane->vtx, idx);
        return -EFBIG;
    }

    uint64_t capture_tai = tx_capture_tai_ns(s);
    e2e_stamp_write(addr, capture_tai);
    memcpy((uint8_t*)addr + stamp_sz, data, size);
    size_t wire_size = size + stamp_sz;

    static int stamp_log_once;
    if (!stamp_log_once++) {
        printf("tx_sched: E2E in-band stamp enabled (+%zu B), capture_tai=%llu\n",
               stamp_sz, (unsigned long long)capture_tai);
    }

    au_trace_record(AU_TRACE_TX_PUBLISH, data, size, demux_pts);
    ret = mtl_tx_video_publish_fb(lane->vtx, idx, wire_size);
    if (ret < 0) {
        mtl_tx_video_abort_fb(lane->vtx, idx);
        return ret;
    }
    lane->stat_video_frames++;
    return 0;
}

static bool h264_packet_has_sps(const uint8_t* data, size_t size) {
    for (size_t i = 0; i + 4 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 &&
            (data[i + 3] & 0x1f) == 7)
            return true;
    }
    return false;
}

static int tx_read_one_video(tx_scheduler* s, struct tx_video_lane* lane,
                             size_t codestream_max, mp4_demux_ctx* demux,
                             AVPacket* pkt) {
    int ret = mp4_demux_read_video(demux, pkt);
    if (ret < 0) return ret;

    const uint8_t* send_data = pkt->data;
    size_t         send_size = (size_t)pkt->size;
    uint8_t*       spliced   = NULL;

    if (lane->ext_hdl) {
        bool idr     = extradata_is_idr(pkt->data, pkt->size);
        bool has_sps = h264_packet_has_sps(pkt->data, pkt->size);
        if (idr && !has_sps) {
            spliced = extradata_splice_idr(lane->ext_hdl, pkt->data, pkt->size,
                                           &send_size);
            if (spliced) send_data = spliced;
        }
    }

    ret = tx_publish_video_frame(s, lane, codestream_max, send_data, send_size, pkt->pts);
    free(spliced);
    av_packet_unref(pkt);
    return ret;
}

static void tx_stream_spec_from_cfg(const mp4_dist_cfg* cfg, int idx,
                                    mp4_dist_tx_stream* out) {
    memset(out, 0, sizeof(*out));
    if (cfg->tx_stream_count > 0) {
        *out = cfg->tx_streams[idx];
        return;
    }
    if (idx != 0)
        return;

    out->main      = true;
    out->udp_port  = cfg->udp_port;
    out->audio_udp_port = cfg->audio_udp_port;
    out->width     = cfg->width;
    out->height    = cfg->height;
    out->fps       = cfg->fps;
    out->codestream_size = cfg->codestream_size;
    out->loop      = cfg->loop;
    out->seek_ms   = cfg->seek_ms;
    if (cfg->mp4_count > 0)
        snprintf(out->mp4_file, sizeof(out->mp4_file), "%s", cfg->mp4_files[0]);
    else if (cfg->mp4_file[0])
        snprintf(out->mp4_file, sizeof(out->mp4_file), "%s", cfg->mp4_file);
}

static int tx_lane_open_playlist(const mp4_dist_cfg* cfg, int lane_idx,
                                 const mp4_dist_tx_stream* spec,
                                 mp4_playlist** out) {
    if (cfg->tx_stream_count > 0) {
        mp4_dist_cfg mini;
        memset(&mini, 0, sizeof(mini));
        mini.mp4_count = 1;
        snprintf(mini.mp4_files[0], sizeof(mini.mp4_files[0]), "%s", spec->mp4_file);
        mini.loop    = spec->loop ? spec->loop : cfg->loop;
        mini.seek_ms = spec->seek_ms ? spec->seek_ms : cfg->seek_ms;
        return mp4_playlist_create(&mini, out);
    }

    if (lane_idx != 0)
        return -EINVAL;
    return mp4_playlist_create(cfg, out);
}

static void tx_lane_free(struct tx_video_lane* lane) {
    if (!lane) return;
    if (lane->vtx) mtl_tx_video_free(lane->vtx);
    if (lane->ext_hdl) extradata_handler_free(lane->ext_hdl);
    if (lane->playlist) mp4_playlist_free(lane->playlist);
    memset(lane, 0, sizeof(*lane));
}

static int tx_lane_init(tx_scheduler* s, int idx, const mp4_dist_tx_stream* spec) {
    struct tx_video_lane* lane = &s->lanes[idx];
    lane->spec = *spec;

    int ret = tx_lane_open_playlist(&s->cfg, idx, spec, &lane->playlist);
    if (ret < 0)
        return ret;

    const mp4_stream_info* vs = mp4_playlist_video_info(lane->playlist);
    if (!vs) {
        tx_lane_free(lane);
        return -EINVAL;
    }

    if (vs->extradata && vs->extradata_size > 0)
        lane->ext_hdl = extradata_handler_create(vs->extradata, vs->extradata_size);

    bool tai_ts = (s->cfg.ptp_mode != MP4_DIST_PTP_MONOTONIC);
    size_t cs   = spec->codestream_size > 0 ? spec->codestream_size
                                            : s->cfg.codestream_size;
    if (!cs)
        cs = 1024 * 1024;

    struct mtl_tx_video_params vp = {
        .st                = s->mt,
        .framebuff_cnt     = 8,
        .framebuff_max     = cs,
        .width             = spec->width > 0 ? spec->width : vs->width,
        .height            = spec->height > 0 ? spec->height : vs->height,
        .fps               = spec->fps > 0 ? spec->fps : vs->fps,
        .udp_port          = spec->udp_port,
        .dip_addr          = s->cfg.tx_dip,
        .port_name         = s->cfg.tx_port,
        .payload_type      = 112,
        .use_tai_timestamp = tai_ts,
    };
    lane->vtx = mtl_tx_video_create(&vp);
    if (!lane->vtx) {
        tx_lane_free(lane);
        return -EIO;
    }

    printf("tx_sched: lane[%d] %s → %s:%u %dx%d@%.3ffps%s\n",
           idx, spec->mp4_file[0] ? spec->mp4_file : "(playlist)",
           s->cfg.tx_dip, spec->udp_port, vp.width, vp.height, (double)vp.fps,
           spec->main ? " main" : "");
    return 0;
}

static void* tx_lane_video_thread(void* arg) {
    tx_lane_thread_args* a = arg;
    tx_scheduler* s        = a->sched;
    struct tx_video_lane* lane    = a->lane;
    atomic_bool* running   = a->running;
    AVPacket* pkt          = av_packet_alloc();
    size_t cs = lane->spec.codestream_size > 0 ? lane->spec.codestream_size
                                             : s->cfg.codestream_size;
    if (!cs)
        cs = 1024 * 1024;

    printf("tx_sched: lane video producer started (udp=%u)\n", lane->spec.udp_port);

    while (atomic_load(running)) {
        mp4_demux_ctx* demux = mp4_playlist_video_demux(lane->playlist);
        int ret = tx_read_one_video(s, lane, cs, demux, pkt);
        if (ret == AVERROR_EOF) {
            ret = mp4_playlist_advance_video(lane->playlist);
            if (ret < 0) {
                printf("tx_sched: lane udp=%u playlist end\n", lane->spec.udp_port);
                break;
            }
            continue;
        }
        if (ret < 0) {
            if (ret == -EAGAIN) {
                usleep(100);
                continue;
            }
            fprintf(stderr, "tx_sched: lane udp=%u video error %d\n",
                    lane->spec.udp_port, ret);
            break;
        }
    }

    av_packet_free(&pkt);
    printf("tx_sched: lane udp=%u exit (%llu AU pkts)\n",
           lane->spec.udp_port, (unsigned long long)lane->stat_video_frames);
    return NULL;
}

static int tx_fill_audio_fb(tx_scheduler* s, void* fb, size_t frame_bytes,
                            uint8_t** pcm_buf, size_t* pcm_len, size_t* pcm_off,
                            mp4_demux_ctx* demux, AVPacket* pkt) {
    size_t filled = 0;
    uint8_t* dst = fb;

    while (filled < frame_bytes) {
        if (*pcm_off >= *pcm_len) {
            free(*pcm_buf);
            *pcm_buf = NULL;
            *pcm_len = 0;
            *pcm_off = 0;

            int ret = mp4_demux_read_audio(demux, pkt);
            if (ret == AVERROR_EOF) return ret;
            if (ret < 0) return ret;

            ret = aac_decode_packet(s, pkt, pcm_buf, pcm_len);
            av_packet_unref(pkt);
            if (ret < 0) return ret;
        }

        size_t avail = *pcm_len - *pcm_off;
        size_t need  = frame_bytes - filled;
        size_t copy  = avail < need ? avail : need;
        memcpy(dst + filled, *pcm_buf + *pcm_off, copy);
        *pcm_off += copy;
        filled   += copy;
    }
    return 0;
}

static void* tx_audio_thread(void* arg) {
    void**        args    = arg;
    tx_scheduler* s       = args[0];
    atomic_bool*  running = args[1];
    struct tx_video_lane* main   = &s->lanes[s->main_lane_idx];
    AVPacket*     pkt     = av_packet_alloc();
    uint8_t*      pcm_buf = NULL;
    size_t        pcm_len = 0;
    size_t        pcm_off = 0;
    size_t        frame_bytes = mtl_tx_audio_frame_bytes(s->atx);

    printf("tx_sched: audio producer started (main lane udp=%u)\n",
           main->spec.udp_port);

    while (atomic_load(running)) {
        uint16_t idx;
        void*    addr;
        int ret = mtl_tx_audio_acquire_fb(s->atx, &idx, &addr);
        if (ret < 0) break;

        mp4_demux_ctx* demux = mp4_playlist_audio_demux(main->playlist);
        ret = tx_fill_audio_fb(s, addr, frame_bytes, &pcm_buf, &pcm_len, &pcm_off,
                               demux, pkt);
        if (ret == AVERROR_EOF) {
            mtl_tx_audio_abort_fb(s->atx, idx);
            ret = mp4_playlist_advance_audio(main->playlist);
            if (ret < 0) break;
            continue;
        }
        if (ret < 0) {
            mtl_tx_audio_abort_fb(s->atx, idx);
            usleep(1000);
            continue;
        }

        ret = mtl_tx_audio_publish_fb(s->atx, idx, frame_bytes);
        if (ret < 0) {
            mtl_tx_audio_abort_fb(s->atx, idx);
            continue;
        }
        s->stat_audio_frames++;
    }

    free(pcm_buf);
    av_packet_free(&pkt);
    printf("tx_sched: audio producer exit (%llu frames)\n",
           (unsigned long long)s->stat_audio_frames);
    return NULL;
}

int tx_scheduler_create(mtl_handle mt, const mp4_dist_cfg* cfg,
                        tx_scheduler** out) {
    *out = NULL;
    if (!cfg) return -EINVAL;
    if (cfg->tx_stream_count <= 0 && cfg->mp4_count <= 0)
        return -EINVAL;

    tx_scheduler* s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;

    s->mt  = mt;
    s->cfg = *cfg;
    s->lane_count = cfg->tx_stream_count > 0 ? cfg->tx_stream_count : 1;

    for (int i = 0; i < s->lane_count; i++) {
        mp4_dist_tx_stream spec;
        tx_stream_spec_from_cfg(cfg, i, &spec);
        if (spec.main)
            s->main_lane_idx = i;
        int ret = tx_lane_init(s, i, &spec);
        if (ret < 0) {
            tx_scheduler_close(s);
            return ret;
        }
    }

    struct tx_video_lane* main = &s->lanes[s->main_lane_idx];
    bool tai_ts = (cfg->ptp_mode != MP4_DIST_PTP_MONOTONIC);

    if (mp4_playlist_audio_info(main->playlist)) {
        uint16_t audio_port = main->spec.audio_udp_port;
        if (!audio_port)
            audio_port = cfg->audio_udp_port;

        struct mtl_tx_audio_params ap = {
            .st                = mt,
            .framebuff_cnt     = 32,
            .udp_port          = audio_port,
            .dip_addr          = cfg->tx_dip,
            .port_name         = cfg->tx_port,
            .payload_type      = 98,
            .fmt               = ST30_FMT_PCM16,
            .sampling          = ST30_SAMPLING_48K,
            .ptime             = ST30_PTIME_1MS,
            .channel           = 2,
            .use_tai_timestamp = tai_ts,
        };
        s->atx = mtl_tx_audio_create(&ap);
        if (s->atx)
            aac_decode_init(s, mp4_playlist_audio_demux(main->playlist));
    }

    printf("tx_sched: %d video lane(s), loop=%s, audio=%s\n",
           s->lane_count, cfg->loop ? "on" : "off", s->atx ? "on" : "off");
    *out = s;
    return 0;
}

void tx_scheduler_run(tx_scheduler* s, atomic_bool* running) {
    if (!s) return;

    for (int i = 0; i < s->lane_count; i++)
        mtl_tx_video_start(s->lanes[i].vtx);
    if (s->atx)
        mtl_tx_audio_start(s->atx);

    tx_lane_thread_args lane_args[MP4_PIP_MAX_LANES];
    for (int i = 0; i < s->lane_count; i++) {
        lane_args[i].sched   = s;
        lane_args[i].lane    = &s->lanes[i];
        lane_args[i].running = running;
        if (pthread_create(&s->lanes[i].video_tid, NULL, tx_lane_video_thread,
                           &lane_args[i]) != 0) {
            fprintf(stderr, "tx_sched: lane[%d] video thread create failed\n", i);
            atomic_store(running, false);
            goto stop_all;
        }
    }
    s->threads_started = true;

    void* audio_args[2] = { s, running };
    if (s->atx &&
        pthread_create(&s->audio_tid, NULL, tx_audio_thread, audio_args) != 0) {
        fprintf(stderr, "tx_sched: audio thread create failed\n");
        atomic_store(running, false);
        goto stop_all;
    }

    for (int i = 0; i < s->lane_count; i++)
        pthread_join(s->lanes[i].video_tid, NULL);
    if (s->atx)
        pthread_join(s->audio_tid, NULL);

stop_all:
    for (int i = 0; i < s->lane_count; i++)
        mtl_tx_video_stop(s->lanes[i].vtx);
    if (s->atx)
        mtl_tx_audio_stop(s->atx);

    uint64_t total_video = 0;
    for (int i = 0; i < s->lane_count; i++)
        total_video += s->lanes[i].stat_video_frames;

    printf("tx_sched: done — %llu video AU pkts (%d lanes), %llu audio frames\n",
           (unsigned long long)total_video, s->lane_count,
           (unsigned long long)s->stat_audio_frames);
}

void tx_scheduler_close(tx_scheduler* s) {
    if (!s) return;

    aac_decode_close(s);
    if (s->atx) mtl_tx_audio_free(s->atx);
    for (int i = 0; i < s->lane_count; i++)
        tx_lane_free(&s->lanes[i]);
    au_trace_close();
    free(s);
}
