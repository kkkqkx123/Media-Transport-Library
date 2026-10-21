/* SPDX-License-Identifier: BSD-3-Clause
 *
 * FFmpeg-based MP4 file demuxer implementation.
 */

#include "mp4_demux.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mp4_demux_ctx {
    AVFormatContext* fmt_ctx;
    AVBSFContext*    bsf_ctx;       /* h264_mp4toannexb bitstream filter */
    mp4_stream_info   video_info;
    mp4_stream_info   audio_info;
    bool              has_video;
    bool              has_audio;
};

int mp4_demux_open(const char* path, mp4_demux_ctx** ctx_out) {
    *ctx_out = NULL;

    mp4_demux_ctx* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -ENOMEM;

    int ret = avformat_open_input(&ctx->fmt_ctx, path, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "demux: cannot open %s: %d\n", path, ret);
        free(ctx);
        return -ENOENT;
    }

    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "demux: cannot find stream info: %d\n", ret);
        avformat_close_input(&ctx->fmt_ctx);
        free(ctx);
        return -EIO;
    }

    /* find video stream */
    int vs = av_find_best_stream(ctx->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs >= 0) {
        AVStream* st  = ctx->fmt_ctx->streams[vs];
        AVCodecParameters* par = st->codecpar;

        ctx->has_video = true;
        ctx->video_info.index          = vs;
        ctx->video_info.codec_id       = par->codec_id;
        ctx->video_info.width          = par->width;
        ctx->video_info.height         = par->height;
        ctx->video_info.fps            = (float)st->avg_frame_rate.num /
                                         (float)st->avg_frame_rate.den;
        ctx->video_info.time_base_num  = st->time_base.num;
        ctx->video_info.time_base_den  = st->time_base.den;

        if (par->extradata && par->extradata_size > 0) {
            ctx->video_info.extradata_size = par->extradata_size;
            ctx->video_info.extradata = malloc(par->extradata_size);
            memcpy(ctx->video_info.extradata, par->extradata, par->extradata_size);
        }

        printf("demux: video stream #%d: %dx%d %.1ffps codec=%d\n",
               vs, par->width, par->height,
               (double)ctx->video_info.fps, par->codec_id);
    }

    /* find audio stream */
    int as = av_find_best_stream(ctx->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (as >= 0) {
        AVStream* st  = ctx->fmt_ctx->streams[as];
        AVCodecParameters* par = st->codecpar;

        ctx->has_audio = true;
        ctx->audio_info.index         = as;
        ctx->audio_info.codec_id      = par->codec_id;
        ctx->audio_info.sample_rate   = par->sample_rate;
        ctx->audio_info.channels      = par->channels;
        ctx->audio_info.time_base_num = st->time_base.num;
        ctx->audio_info.time_base_den = st->time_base.den;

        printf("demux: audio stream #%d: %dHz %dch codec=%d\n",
               as, par->sample_rate, par->channels, par->codec_id);
    }

    if (!ctx->has_video) {
        fprintf(stderr, "demux: no video stream found in %s\n", path);
        mp4_demux_close(ctx);
        return -EINVAL;
    }

    /* create h264_mp4toannexb bitstream filter to convert AVCC→Annex B */
    const AVBitStreamFilter* bsf = av_bsf_get_by_name("h264_mp4toannexb");
    if (bsf) {
        ret = av_bsf_alloc(bsf, &ctx->bsf_ctx);
        if (ret >= 0) {
            avcodec_parameters_copy(ctx->bsf_ctx->par_in,
                                    ctx->fmt_ctx->streams[vs]->codecpar);
            ret = av_bsf_init(ctx->bsf_ctx);
            if (ret >= 0) {
                printf("demux: h264_mp4toannexb BSF initialized\n");
            } else {
                fprintf(stderr, "demux: BSF init failed: %d\n", ret);
                av_bsf_free(&ctx->bsf_ctx);
                ctx->bsf_ctx = NULL;
            }
        } else {
            fprintf(stderr, "demux: BSF alloc failed: %d\n", ret);
        }
    } else {
        fprintf(stderr, "demux: h264_mp4toannexb BSF not available\n");
    }

    *ctx_out = ctx;
    return 0;
}

const mp4_stream_info* mp4_demux_get_video_stream(mp4_demux_ctx* ctx) {
    return ctx->has_video ? &ctx->video_info : NULL;
}

const mp4_stream_info* mp4_demux_get_audio_stream(mp4_demux_ctx* ctx) {
    return ctx->has_audio ? &ctx->audio_info : NULL;
}

const AVCodecParameters* mp4_demux_get_audio_codecpar(mp4_demux_ctx* ctx) {
    if (!ctx || !ctx->has_audio) return NULL;
    return ctx->fmt_ctx->streams[ctx->audio_info.index]->codecpar;
}

const AVCodecParameters* mp4_demux_get_video_codecpar(mp4_demux_ctx* ctx) {
    if (!ctx || !ctx->has_video) return NULL;
    return ctx->fmt_ctx->streams[ctx->video_info.index]->codecpar;
}

int mp4_probe_file(const char* path, int* width, int* height, float* fps) {
    AVFormatContext* fmt = NULL;
    int ret = avformat_open_input(&fmt, path, NULL, NULL);
    if (ret < 0) return -ENOENT;

    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        avformat_close_input(&fmt);
        return -EIO;
    }

    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs < 0) {
        avformat_close_input(&fmt);
        return -EINVAL;
    }

    AVStream* st = fmt->streams[vs];
    AVCodecParameters* par = st->codecpar;
    if (width)  *width  = par->width;
    if (height) *height = par->height;
    if (fps) {
        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
            *fps = (float)st->avg_frame_rate.num / (float)st->avg_frame_rate.den;
        else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
            *fps = (float)st->r_frame_rate.num / (float)st->r_frame_rate.den;
        else
            *fps = 0.0f;
    }

    avformat_close_input(&fmt);
    return 0;
}

static int mp4_demux_read_stream(mp4_demux_ctx* ctx, AVPacket* pkt, bool want_video) {
    int ret;
    while (1) {
        ret = av_read_frame(ctx->fmt_ctx, pkt);
        if (ret < 0) return ret;

        if (want_video) {
            if (pkt->stream_index != ctx->video_info.index) {
                av_packet_unref(pkt);
                continue;
            }
            if (ctx->bsf_ctx) {
                ret = av_bsf_send_packet(ctx->bsf_ctx, pkt);
                if (ret < 0) {
                    av_packet_unref(pkt);
                    return ret;
                }
                ret = av_bsf_receive_packet(ctx->bsf_ctx, pkt);
                if (ret < 0 && ret != AVERROR(EAGAIN)) {
                    av_packet_unref(pkt);
                    return ret;
                }
                if (ret == AVERROR(EAGAIN)) {
                    av_packet_unref(pkt);
                    continue;
                }
            }
            return 0;
        }

        if (ctx->has_audio && pkt->stream_index == ctx->audio_info.index)
            return 0;

        av_packet_unref(pkt);
    }
}

int mp4_demux_read_video(mp4_demux_ctx* ctx, AVPacket* pkt) {
    if (!ctx || !ctx->has_video) return -EINVAL;
    return mp4_demux_read_stream(ctx, pkt, true);
}

int mp4_demux_read_audio(mp4_demux_ctx* ctx, AVPacket* pkt) {
    if (!ctx || !ctx->has_audio) return -EINVAL;
    return mp4_demux_read_stream(ctx, pkt, false);
}

int mp4_demux_read_packet(mp4_demux_ctx* ctx, AVPacket* pkt, bool* is_video) {
    int ret;
    while (1) {
        ret = av_read_frame(ctx->fmt_ctx, pkt);
        if (ret < 0) return ret;  /* AVERROR_EOF or error */

        if (pkt->stream_index == ctx->video_info.index) {
            *is_video = true;

            /* apply h264_mp4toannexb BSF to convert AVCC→Annex B */
            if (ctx->bsf_ctx) {
                ret = av_bsf_send_packet(ctx->bsf_ctx, pkt);
                if (ret < 0) {
                    fprintf(stderr, "demux: BSF send failed: %d\n", ret);
                    av_packet_unref(pkt);
                    return ret;
                }
                ret = av_bsf_receive_packet(ctx->bsf_ctx, pkt);
                if (ret < 0 && ret != AVERROR(EAGAIN)) {
                    /* may return EAGAIN if BSF buffers internally */
                    av_packet_unref(pkt);
                    return ret;
                }
                if (ret == AVERROR(EAGAIN)) {
                    av_packet_unref(pkt);
                    continue;  /* try again */
                }
            }
            return 0;
        } else if (ctx->has_audio && pkt->stream_index == ctx->audio_info.index) {
            *is_video = false;
            return 0;
        }
        /* skip other streams */
        av_packet_unref(pkt);
    }
}

int mp4_demux_seek_start(mp4_demux_ctx* ctx) {
    return av_seek_frame(ctx->fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
}

int mp4_demux_seek_ms(mp4_demux_ctx* ctx, int64_t ms) {
    if (!ctx || !ctx->fmt_ctx) return -EINVAL;
    int64_t ts = ms * AV_TIME_BASE / 1000;
    return av_seek_frame(ctx->fmt_ctx, -1, ts, AVSEEK_FLAG_BACKWARD);
}

void mp4_demux_close(mp4_demux_ctx* ctx) {
    if (!ctx) return;
    if (ctx->bsf_ctx) av_bsf_free(&ctx->bsf_ctx);
    if (ctx->fmt_ctx) avformat_close_input(&ctx->fmt_ctx);
    free(ctx->video_info.extradata);
    free(ctx);
}
