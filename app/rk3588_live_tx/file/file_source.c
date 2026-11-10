#define _GNU_SOURCE
/* SPDX-License-Identifier: BSD-3-Clause
 *
 * File frame-source plugin for live_ingest (phase2).
 * MP4 demux + playlist + send scheduling feeding the encode-side send sink
 * (tx_mtl_sink_ops) and the ST30 audio session. Single video lane.
 */

#include "file_source.h"
#include "../pipeline/tx_pipeline.h"
#include "../mtl_tx.h"
#include "../mtl_tx_audio.h"
#include "au_trace.h"
#include "extradata_handler.h"
#include "sei_timestamp.h"
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <mtl_api.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct demux_slot {
    AVFormatContext* fmt;
    int video_idx;
    int audio_idx;
    AVBSFContext* bsf;
};

struct file_source {
    struct file_source_params cfg;
    mtl_handle st;

    struct demux_slot video;
    struct demux_slot audio;
    extradata_handler* ext;
    pthread_mutex_t pl_lock;
    int pl_video_pos;
    int pl_audio_pos;
    bool seek_pending;

    AVCodecContext* aac_ctx;
    AVFrame* aac_frame;
    struct SwrContext* swr;
    int swr_src_rate;
    int swr_src_ch;
    enum AVSampleFormat swr_src_fmt;
    int swr_dst_ch; /* ST30 session channels (1..2) */
    bool has_audio;

    pthread_t video_tid;
    pthread_t audio_tid;
    bool threads_started;
    atomic_bool* running;

    uint64_t stat_video;
    uint64_t stat_audio;
    uint64_t stat_drops;
    uint32_t frame_seq;
};

static uint64_t file_now_ns(struct file_source* fs) {
    if (fs->st) {
        uint64_t t = mtl_ptp_read_time(fs->st);
        if (t) return t;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void slot_close(struct demux_slot* s) {
    if (!s) return;
    if (s->bsf) av_bsf_free(&s->bsf);
    if (s->fmt) avformat_close_input(&s->fmt);
    memset(s, 0, sizeof(*s));
    s->video_idx = -1;
    s->audio_idx = -1;
}

static int slot_open(struct file_source* fs, struct demux_slot* s, int pos) {
    slot_close(s);
    if (pos < 0 || pos >= fs->cfg.file_count) return -EINVAL;
    const char* path = fs->cfg.files[pos];
    AVFormatContext* fmt = NULL;
    int ret = avformat_open_input(&fmt, path, NULL, NULL);
    if (ret < 0) return ret;
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        avformat_close_input(&fmt);
        return ret;
    }
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vi < 0) {
        avformat_close_input(&fmt);
        return -EINVAL;
    }
    int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    AVBSFContext* bsf = NULL;
    const AVBitStreamFilter* filt = av_bsf_get_by_name("h264_mp4toannexb");
    if (filt) {
        if (av_bsf_alloc(filt, &bsf) == 0) {
            avcodec_parameters_copy(bsf->par_in,
                                    fmt->streams[vi]->codecpar);
            if (av_bsf_init(bsf) < 0) {
                av_bsf_free(&bsf);
                bsf = NULL;
            }
        }
    }

    if (fs->seek_pending && fs->cfg.seek_ms > 0)
        av_seek_frame(fmt, -1, (int64_t)fs->cfg.seek_ms * AV_TIME_BASE / 1000,
                      AVSEEK_FLAG_BACKWARD);

    s->fmt = fmt;
    s->video_idx = vi;
    s->audio_idx = ai;
    s->bsf = bsf;
    printf("file_source: [%d/%d] %s (v=%d a=%d)\n",
           pos + 1, fs->cfg.file_count, path, vi, ai);
    return 0;
}

static int playlist_advance(struct file_source* fs, int* pos,
                            struct demux_slot* slot, const char* kind) {
    pthread_mutex_lock(&fs->pl_lock);
    int next = *pos + 1;
    if (next >= fs->cfg.file_count) {
        if (!fs->cfg.loop) {
            pthread_mutex_unlock(&fs->pl_lock);
            return -ENOENT;
        }
        next = 0;
    }
    int ret = slot_open(fs, slot, next);
    if (ret < 0) {
        pthread_mutex_unlock(&fs->pl_lock);
        return ret;
    }
    *pos = next;
    fs->seek_pending = false;
    pthread_mutex_unlock(&fs->pl_lock);
    if (!strcmp(kind, "video") && fs->cfg.video_sink)
        mtl_tx_tai_reset((struct mtl_tx*)fs->cfg.video_sink->priv);
    if (!strcmp(kind, "audio") && fs->cfg.audio_tx)
        mtl_tx_audio_tai_reset(fs->cfg.audio_tx);
    return 0;
}

static int read_one_video(struct file_source* fs, AVPacket* pkt) {
    struct demux_slot* s = &fs->video;
    while (1) {
        int ret = av_read_frame(s->fmt, pkt);
        if (ret < 0) return ret;
        if (pkt->stream_index != s->video_idx) {
            av_packet_unref(pkt);
            continue;
        }
        if (s->bsf) {
            ret = av_bsf_send_packet(s->bsf, pkt);
            if (ret < 0) {
                av_packet_unref(pkt);
                return ret;
            }
            ret = av_bsf_receive_packet(s->bsf, pkt);
            if (ret == AVERROR(EAGAIN)) {
                av_packet_unref(pkt);
                continue;
            }
            if (ret < 0) {
                av_packet_unref(pkt);
                return ret;
            }
        }
        return 0;
    }
}

static bool pkt_is_sps(const uint8_t* data, size_t size) {
    for (size_t i = 0; i + 4 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 &&
            (data[i + 3] & 0x1f) == 7)
            return true;
    }
    return false;
}

static void* video_thread(void* arg) {
    struct file_source* fs = arg;
    atomic_bool* running = fs->running;
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return NULL;

    printf("file_source: video feeder started\n");
    while (atomic_load(running)) {
        int ret = read_one_video(fs, pkt);
        if (ret == AVERROR_EOF) {
            if (playlist_advance(fs, &fs->pl_video_pos, &fs->video, "video") < 0)
                break;
            continue;
        }
        if (ret < 0) {
            if (ret == -EAGAIN) {
                usleep(100);
                continue;
            }
            fprintf(stderr, "file_source: video read %d\n", ret);
            break;
        }

        const uint8_t* send_data = pkt->data;
        size_t send_size = (size_t)pkt->size;
        uint8_t* spliced = NULL;
        if (fs->ext && extradata_is_idr(pkt->data, pkt->size) &&
            !pkt_is_sps(pkt->data, (size_t)pkt->size)) {
            spliced = extradata_splice_idr(fs->ext, pkt->data, pkt->size,
                                           &send_size);
            if (spliced) send_data = spliced;
        }

        size_t cap = fs->cfg.codestream_max ? fs->cfg.codestream_max : 512 * 1024;
        if (send_size + SEI_TS_NAL_SIZE > cap) {
            fs->stat_drops++;
            free(spliced);
            av_packet_unref(pkt);
            continue;
        }

        uint16_t idx;
        void* addr;
        if (fs->cfg.video_sink->acquire_fb(fs->cfg.video_sink->priv, &idx,
                                           &addr) != 0) {
            fs->stat_drops++;
            free(spliced);
            av_packet_unref(pkt);
            continue;
        }
        uint64_t cap_tai = file_now_ns(fs);
        sei_timestamp_build((uint8_t*)addr, fs->frame_seq++, cap_tai);
        memcpy((uint8_t*)addr + SEI_TS_NAL_SIZE, send_data, send_size);
        size_t total = send_size + SEI_TS_NAL_SIZE;
        au_trace_record(AU_TRACE_TX_PUBLISH, send_data, send_size, pkt->pts);
        if (fs->cfg.video_sink->publish_fb(fs->cfg.video_sink->priv, idx,
                                           total) != 0) {
            fs->cfg.video_sink->abort_fb(fs->cfg.video_sink->priv, idx);
            fs->stat_drops++;
        } else {
            fs->stat_video++;
        }
        free(spliced);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    printf("file_source: video exit (%llu frames, %llu drops)\n",
           (unsigned long long)fs->stat_video,
           (unsigned long long)fs->stat_drops);
    return NULL;
}

static void aac_close(struct file_source* fs) {
    if (fs->swr) {
        swr_free(&fs->swr);
        fs->swr = NULL;
    }
    fs->swr_src_rate = 0;
    fs->swr_src_ch = 0;
    if (fs->aac_frame) {
        av_frame_free(&fs->aac_frame);
        fs->aac_frame = NULL;
    }
    if (fs->aac_ctx) {
        avcodec_free_context(&fs->aac_ctx);
        fs->aac_ctx = NULL;
    }
}

static int aac_open(struct file_source* fs) {
    if (fs->audio.audio_idx < 0 || !fs->audio.fmt) return -ENOSYS;
    AVStream* st = fs->audio.fmt->streams[fs->audio.audio_idx];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) return -ENOSYS;
    fs->aac_ctx = avcodec_alloc_context3(dec);
    if (!fs->aac_ctx) return -ENOMEM;
    if (avcodec_parameters_to_context(fs->aac_ctx, st->codecpar) < 0) {
        avcodec_free_context(&fs->aac_ctx);
        return -EIO;
    }
    fs->aac_ctx->request_sample_fmt = AV_SAMPLE_FMT_S16;
    if (avcodec_open2(fs->aac_ctx, dec, NULL) < 0) {
        avcodec_free_context(&fs->aac_ctx);
        return -EIO;
    }
    fs->aac_frame = av_frame_alloc();
    if (!fs->aac_frame) {
        avcodec_free_context(&fs->aac_ctx);
        return -ENOMEM;
    }
    /* Resampler is initialized lazily on the first decoded frame (source
     * rate/channels/format known only then); direct copy when already
     * 48kHz S16 with matching channels. */
    fs->swr = NULL;
    fs->swr_src_rate = 0;
    fs->swr_src_ch = 0;
    fs->swr_src_fmt = AV_SAMPLE_FMT_NONE;
    fs->swr_dst_ch = fs->cfg.audio_channels ? fs->cfg.audio_channels : 2;
    fs->has_audio = true;
    return 0;
}

static int aac_decode_packet(struct file_source* fs, AVPacket* pkt,
                             uint8_t** out, size_t* out_len) {
    int ret = avcodec_send_packet(fs->aac_ctx, pkt);
    if (ret < 0) return ret;
    ret = avcodec_receive_frame(fs->aac_ctx, fs->aac_frame);
    if (ret < 0) return ret;
    int samples = fs->aac_frame->nb_samples;
    int src_rate = fs->aac_frame->sample_rate;
    int src_ch = fs->aac_frame->channels;
    enum AVSampleFormat src_fmt = fs->aac_frame->format;
    int dst_ch = fs->swr_dst_ch ? fs->swr_dst_ch : 2;
    if (src_rate == 48000 && src_ch == dst_ch &&
        (src_fmt == AV_SAMPLE_FMT_S16 || src_fmt == AV_SAMPLE_FMT_S16P)) {
        int ch = src_ch;
        size_t bytes = (size_t)samples * (size_t)ch * 2;
        uint8_t* buf = malloc(bytes ? bytes : 1);
        if (!buf) return -ENOMEM;
        if (src_fmt == AV_SAMPLE_FMT_S16P) {
            int16_t* dst = (int16_t*)buf;
            for (int c = 0; c < ch; c++) {
                int16_t* src = (int16_t*)fs->aac_frame->data[c];
                for (int i = 0; i < samples; i++)
                    dst[i * ch + c] = src[i];
            }
        } else {
            memcpy(buf, fs->aac_frame->data[0], bytes);
        }
        *out = buf;
        *out_len = bytes;
        return 0;
    }
    if (!fs->swr || fs->swr_src_rate != src_rate || fs->swr_src_ch != src_ch ||
        fs->swr_src_fmt != src_fmt) {
        swr_free(&fs->swr);
        fs->swr = swr_alloc_set_opts(NULL,
            av_get_default_channel_layout(dst_ch), AV_SAMPLE_FMT_S16, 48000,
            av_get_default_channel_layout(src_ch), src_fmt, src_rate,
            0, NULL);
        if (!fs->swr) return -ENOMEM;
        if (swr_init(fs->swr) < 0) {
            swr_free(&fs->swr);
            return -EIO;
        }
        fs->swr_src_rate = src_rate;
        fs->swr_src_ch = src_ch;
        fs->swr_src_fmt = src_fmt;
        printf("file_source: audio resample %dHz/%dch/%s -> 48000Hz/%dch/s16\n",
               src_rate, src_ch, av_get_sample_fmt_name(src_fmt), dst_ch);
    }
    int dst_nb = av_rescale_rnd(swr_get_delay(fs->swr, src_rate) + samples,
                                48000, src_rate, AV_ROUND_UP);
    size_t bytes = (size_t)dst_nb * (size_t)dst_ch * 2;
    uint8_t* buf = malloc(bytes ? bytes : 1);
    if (!buf) return -ENOMEM;
    uint8_t* out_arr[1] = { buf };
    int got = swr_convert(fs->swr, out_arr, dst_nb,
                          (const uint8_t**)fs->aac_frame->data, samples);
    if (got < 0) {
        free(buf);
        return -EIO;
    }
    *out = buf;
    *out_len = (size_t)got * (size_t)dst_ch * 2;
    return 0;
}

static void* audio_thread(void* arg) {
    struct file_source* fs = arg;
    atomic_bool* running = fs->running;
    if (!fs->cfg.audio_tx || !fs->has_audio) return NULL;
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return NULL;
    size_t frame_bytes = mtl_tx_audio_frame_bytes(fs->cfg.audio_tx);
    uint8_t* pcm = NULL;
    size_t pcm_len = 0, pcm_off = 0;

    printf("file_source: audio feeder started (%zu B/frame)\n", frame_bytes);
    while (atomic_load(running)) {
        uint16_t idx;
        void* addr;
        if (mtl_tx_audio_acquire_fb(fs->cfg.audio_tx, &idx, &addr) != 0) break;
        size_t filled = 0;
        int rc = 0;
        while (filled < frame_bytes) {
            if (pcm_off >= pcm_len) {
                free(pcm);
                pcm = NULL;
                pcm_len = pcm_off = 0;
                int ret = 0;
                while (1) {
                    ret = av_read_frame(fs->audio.fmt, pkt);
                    if (ret < 0) break;
                    if (pkt->stream_index == fs->audio.audio_idx) break;
                    av_packet_unref(pkt);
                }
                if (ret == AVERROR_EOF) {
                    rc = AVERROR_EOF;
                    break;
                }
                if (ret < 0) {
                    rc = ret;
                    break;
                }
                ret = aac_decode_packet(fs, pkt, &pcm, &pcm_len);
                av_packet_unref(pkt);
                if (ret < 0) {
                    rc = ret;
                    break;
                }
                pcm_off = 0;
            }
            size_t avail = pcm_len - pcm_off;
            size_t need = frame_bytes - filled;
            size_t copy = avail < need ? avail : need;
            memcpy((uint8_t*)addr + filled, pcm + pcm_off, copy);
            pcm_off += copy;
            filled += copy;
        }
        if (rc == AVERROR_EOF) {
            mtl_tx_audio_abort_fb(fs->cfg.audio_tx, idx);
            if (playlist_advance(fs, &fs->pl_audio_pos, &fs->audio, "audio") < 0)
                break;
            aac_close(fs);
            fs->has_audio = false;
            if (aac_open(fs) < 0) break;
            continue;
        }
        if (rc < 0 || filled != frame_bytes) {
            mtl_tx_audio_abort_fb(fs->cfg.audio_tx, idx);
            usleep(1000);
            continue;
        }
        if (mtl_tx_audio_publish_fb(fs->cfg.audio_tx, idx, frame_bytes) != 0)
            mtl_tx_audio_abort_fb(fs->cfg.audio_tx, idx);
        else
            fs->stat_audio++;
    }
    free(pcm);
    av_packet_free(&pkt);
    printf("file_source: audio exit (%llu frames)\n",
           (unsigned long long)fs->stat_audio);
    return NULL;
}

struct file_source* file_source_create(const struct file_source_params* p) {
    if (!p || !p->video_sink || p->file_count <= 0) return NULL;
    if (p->audio_tx && p->audio_channels != 0 &&
        (p->audio_channels < 1 || p->audio_channels > 2)) return NULL;
    struct file_source* fs = calloc(1, sizeof(*fs));
    if (!fs) return NULL;
    fs->cfg = *p;
    fs->st = (mtl_handle)p->st;
    fs->video.video_idx = -1;
    fs->audio.video_idx = -1;
    fs->audio.audio_idx = -1;
    fs->pl_video_pos = 0;
    fs->pl_audio_pos = 0;
    fs->seek_pending = (p->seek_ms > 0);
    pthread_mutex_init(&fs->pl_lock, NULL);

    if (slot_open(fs, &fs->video, 0) < 0) goto fail;
    if (slot_open(fs, &fs->audio, 0) < 0) {
        slot_close(&fs->audio);
        fs->audio.audio_idx = -1;
    }

    AVStream* vs = fs->video.fmt->streams[fs->video.video_idx];
    AVCodecParameters* vpar = vs->codecpar;
    if (vpar->extradata && vpar->extradata_size > 0)
        fs->ext = extradata_handler_create(vpar->extradata,
                                           vpar->extradata_size);
    if (fs->cfg.fps <= 0) {
        if (vs->avg_frame_rate.num > 0 && vs->avg_frame_rate.den > 0)
            fs->cfg.fps = (float)vs->avg_frame_rate.num / (float)vs->avg_frame_rate.den;
        else if (vs->r_frame_rate.num > 0 && vs->r_frame_rate.den > 0)
            fs->cfg.fps = (float)vs->r_frame_rate.num / (float)vs->r_frame_rate.den;
    }

    if (fs->cfg.audio_tx && fs->audio.audio_idx >= 0) {
        if (aac_open(fs) < 0) {
            fs->has_audio = false;
        }
    }
    fs->seek_pending = false;

    size_t cap = fs->cfg.codestream_max ? fs->cfg.codestream_max : 512 * 1024;
    printf("file_source: %d file(s) %dx%d@%.3f cap=%zu loop=%s audio=%s\n",
           fs->cfg.file_count,
           fs->cfg.width, fs->cfg.height, (double)fs->cfg.fps, cap,
           fs->cfg.loop ? "on" : "off", fs->has_audio ? "on" : "off");
    return fs;

fail:
    slot_close(&fs->video);
    slot_close(&fs->audio);
    pthread_mutex_destroy(&fs->pl_lock);
    free(fs);
    return NULL;
}

void file_source_free(struct file_source* fs) {
    if (!fs) return;
    file_source_stop(fs);
    if (fs->ext) extradata_handler_free(fs->ext);
    aac_close(fs);
    slot_close(&fs->video);
    slot_close(&fs->audio);
    pthread_mutex_destroy(&fs->pl_lock);
    au_trace_close();
    free(fs);
}

int file_source_start(struct file_source* fs, atomic_bool* running) {
    if (!fs || !running) return -EINVAL;
    fs->running = running;
    if (pthread_create(&fs->video_tid, NULL, video_thread, fs) != 0)
        return -EIO;
    fs->threads_started = true;
    if (fs->cfg.audio_tx && fs->has_audio) {
        if (pthread_create(&fs->audio_tid, NULL, audio_thread, fs) != 0) {
            atomic_store(running, false);
            pthread_join(fs->video_tid, NULL);
            fs->threads_started = false;
            return -EIO;
        }
    }
    return 0;
}

void file_source_stop(struct file_source* fs) {
    if (!fs || !fs->threads_started) return;
    pthread_join(fs->video_tid, NULL);
    if (fs->cfg.audio_tx && fs->has_audio)
        pthread_join(fs->audio_tid, NULL);
    fs->threads_started = false;
}

int file_source_probe(const char* path, int* width, int* height, float* fps) {
    AVFormatContext* fmt = NULL;
    int ret = avformat_open_input(&fmt, path, NULL, NULL);
    if (ret < 0) return ret;
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        avformat_close_input(&fmt);
        return ret;
    }
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vi < 0) {
        avformat_close_input(&fmt);
        return -EINVAL;
    }
    AVCodecParameters* par = fmt->streams[vi]->codecpar;
    if (width) *width = par->width;
    if (height) *height = par->height;
    if (fps) {
        AVStream* vs = fmt->streams[vi];
        float avg_fps = 0.0f, r_fps = 0.0f;
        if (vs->avg_frame_rate.num > 0 && vs->avg_frame_rate.den > 0)
            avg_fps = (float)vs->avg_frame_rate.num / (float)vs->avg_frame_rate.den;
        if (vs->r_frame_rate.num > 0 && vs->r_frame_rate.den > 0)
            r_fps = (float)vs->r_frame_rate.num / (float)vs->r_frame_rate.den;
        *fps = avg_fps > 0.0f ? avg_fps : r_fps;
        if (avg_fps > 0.0f && r_fps > 0.0f) {
            float diff = avg_fps - r_fps;
            if (diff < 0.0f) diff = -diff;
            if (diff > 0.5f && diff / avg_fps > 0.05f)
                fprintf(stderr,
                        "file_source: avg_fps=%.3f != r_fps=%.3f, possible VFR, "
                        "only CFR sources are supported\n",
                        (double)avg_fps, (double)r_fps);
        }
    }
    avformat_close_input(&fmt);
    return 0;
}

void file_source_stats(const struct file_source* fs, uint64_t* video_frames,
                       uint64_t* audio_frames, uint64_t* video_drops) {
    if (video_frames) *video_frames = fs ? fs->stat_video : 0;
    if (audio_frames) *audio_frames = fs ? fs->stat_audio : 0;
    if (video_drops) *video_drops = fs ? fs->stat_drops : 0;
}
