/* Wall still decode (see base_image.h). FFmpeg software path only. */

#include "base_image.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_xrgb(unsigned char* dst, int r, int g, int b) {
    dst[0] = (unsigned char)b;
    dst[1] = (unsigned char)g;
    dst[2] = (unsigned char)r;
    dst[3] = 0xFF;
}

static int convert_frame(const AVFrame* f, unsigned char* rgb,
                         int w, int h) {
    if (!f || !rgb || w <= 0 || h <= 0) return -1;
    enum AVPixelFormat fmt = (enum AVPixelFormat)f->format;
    if (fmt == AV_PIX_FMT_RGB24) {
        for (int y = 0; y < h; y++) {
            const unsigned char* s = f->data[0] + (size_t)y * f->linesize[0];
            unsigned char* d = rgb + (size_t)y * w * 4;
            for (int x = 0; x < w; x++)
                put_xrgb(d + (size_t)x * 4, s[x * 3], s[x * 3 + 1],
                         s[x * 3 + 2]);
        }
        return 0;
    }
    if (fmt == AV_PIX_FMT_RGBA) {
        for (int y = 0; y < h; y++) {
            const unsigned char* s = f->data[0] + (size_t)y * f->linesize[0];
            unsigned char* d = rgb + (size_t)y * w * 4;
            for (int x = 0; x < w; x++)
                put_xrgb(d + (size_t)x * 4, s[x * 4], s[x * 4 + 1],
                         s[x * 4 + 2]);
        }
        return 0;
    }
    if (fmt == AV_PIX_FMT_BGR24) {
        for (int y = 0; y < h; y++) {
            const unsigned char* s = f->data[0] + (size_t)y * f->linesize[0];
            unsigned char* d = rgb + (size_t)y * w * 4;
            for (int x = 0; x < w; x++)
                put_xrgb(d + (size_t)x * 4, s[x * 3 + 2], s[x * 3 + 1],
                         s[x * 3]);
        }
        return 0;
    }
    if (fmt == AV_PIX_FMT_GRAY8) {
        for (int y = 0; y < h; y++) {
            const unsigned char* s = f->data[0] + (size_t)y * f->linesize[0];
            unsigned char* d = rgb + (size_t)y * w * 4;
            for (int x = 0; x < w; x++)
                put_xrgb(d + (size_t)x * 4, s[x], s[x], s[x]);
        }
        return 0;
    }
    if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P) {
        int full = (fmt == AV_PIX_FMT_YUVJ420P);
        for (int y = 0; y < h; y++) {
            const unsigned char* yp = f->data[0] + (size_t)y * f->linesize[0];
            const unsigned char* up =
                f->data[1] + (size_t)(y / 2) * f->linesize[1];
            const unsigned char* vp =
                f->data[2] + (size_t)(y / 2) * f->linesize[2];
            unsigned char* d = rgb + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                int Y = yp[x], U = up[x / 2], V = vp[x / 2];
                int c, dd, e;
                if (full) {
                    c = Y;
                    dd = U - 128;
                    e = V - 128;
                } else {
                    c = Y - 16;
                    dd = U - 128;
                    e = V - 128;
                }
                int r = (298 * c + 409 * e + 128) >> 8;
                int g = (298 * c - 100 * dd - 208 * e + 128) >> 8;
                int b = (298 * c + 516 * dd + 128) >> 8;
                if (r < 0) r = 0;
                if (r > 255) r = 255;
                if (g < 0) g = 0;
                if (g > 255) g = 255;
                if (b < 0) b = 0;
                if (b > 255) b = 255;
                put_xrgb(d + (size_t)x * 4, r, g, b);
            }
        }
        return 0;
    }
    return -1;
}

int dvs_base_decode(const char* path,
                    int sub_x, int sub_y, int sub_w, int sub_h,
                    int canvas_w, int canvas_h,
                    unsigned char** px_out,
                    char* err, size_t errlen) {
    if (px_out) *px_out = NULL;
    if (!path || !path[0]) {
        if (err && errlen > 0) snprintf(err, errlen, "empty still path");
        return -1;
    }
    if (canvas_w <= 0 || canvas_h <= 0 || canvas_w > DVS_BASE_MAX_W ||
        canvas_h > DVS_BASE_MAX_H) {
        if (err && errlen > 0)
            snprintf(err, errlen, "bad canvas size %dx%d (max %dx%d)",
                     canvas_w, canvas_h, DVS_BASE_MAX_W, DVS_BASE_MAX_H);
        return -1;
    }
    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) != 0 || !fmt) {
        if (err && errlen > 0) snprintf(err, errlen, "cannot open still");
        return -1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "no stream info");
        avformat_close_input(&fmt);
        return -1;
    }
    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "no video stream");
        avformat_close_input(&fmt);
        return -1;
    }
    const AVCodec* dec =
        avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    if (!dec) {
        if (err && errlen > 0) snprintf(err, errlen, "unsupported still codec");
        avformat_close_input(&fmt);
        return -1;
    }
    AVCodecContext* cc = avcodec_alloc_context3(dec);
    if (!cc) {
        avformat_close_input(&fmt);
        return -1;
    }
    if (avcodec_parameters_to_context(cc, fmt->streams[vs]->codecpar) != 0 ||
        avcodec_open2(cc, dec, NULL) != 0) {
        if (err && errlen > 0) snprintf(err, errlen, "decoder open failed");
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return -1;
    }
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    AVFrame* got = NULL;
    int rc = -1;
    if (frame && pkt) {
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index != vs) {
                av_packet_unref(pkt);
                continue;
            }
            if (avcodec_send_packet(cc, pkt) != 0) {
                av_packet_unref(pkt);
                break;
            }
            av_packet_unref(pkt);
            while (avcodec_receive_frame(cc, frame) == 0) {
                got = frame;
                break;
            }
            if (got) break;
        }
    }
    unsigned char* rgb = NULL;
    if (got && got->width > 0 && got->height > 0) {
        rgb = (unsigned char*)malloc((size_t)got->width * got->height * 4);
        if (rgb && convert_frame(got, rgb, got->width, got->height) == 0)
            rc = 0;
        else {
            int bad_fmt = got ? (int)got->format : -1;
            free(rgb);
            rgb = NULL;
            rc = -1;
            if (err && errlen > 0)
                snprintf(err, errlen,
                         "unsupported still pixel format %d", bad_fmt);
        }
    } else if (err && errlen > 0) {
        snprintf(err, errlen, "no still frame decoded");
    }
    int iw = got ? got->width : 0, ih = got ? got->height : 0;
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    if (rc != 0 || !rgb) {
        free(rgb);
        return -1;
    }
    /* Clamp the crop into the decoded image; fall back to full frame. */
    if (sub_w <= 0 || sub_h <= 0) {
        sub_x = 0;
        sub_y = 0;
        sub_w = iw;
        sub_h = ih;
    }
    if (sub_x < 0) sub_x = 0;
    if (sub_y < 0) sub_y = 0;
    if (sub_x + sub_w > iw) sub_w = iw - sub_x;
    if (sub_y + sub_h > ih) sub_h = ih - sub_y;
    if (sub_w <= 0 || sub_h <= 0) {
        free(rgb);
        if (err && errlen > 0) snprintf(err, errlen, "still crop outside image");
        return -1;
    }
    /* Nearest-neighbor stretch of the crop to the canvas. */
    unsigned char* out =
        (unsigned char*)malloc((size_t)canvas_w * canvas_h * 4);
    if (!out) {
        free(rgb);
        return -1;
    }
    for (int y = 0; y < canvas_h; y++) {
        int sy = sub_y + (int)((long)y * sub_h / canvas_h);
        if (sy >= ih) sy = ih - 1;
        for (int x = 0; x < canvas_w; x++) {
            int sx = sub_x + (int)((long)x * sub_w / canvas_w);
            if (sx >= iw) sx = iw - 1;
            memcpy(out + ((size_t)y * canvas_w + x) * 4,
                   rgb + ((size_t)sy * iw + sx) * 4, 4);
        }
    }
    free(rgb);
    *px_out = out;
    return 0;
}
