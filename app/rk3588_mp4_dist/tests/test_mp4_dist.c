/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Progressive test harness for MP4 Distribution pipeline.
 * Each phase validates one component independently.
 *
 * Usage:
 *   test_mp4_dist phase1 <mp4_file>
 *   test_mp4_dist phase2 <mp4_file>
 *   test_mp4_dist phase3 <mp4_file> [frame_count]
 *   test_mp4_dist phase4 <mp4_file> [max_packets=1440]
 *   test_mp4_dist phase4_neg <mp4_file> [max_packets=500]
 */

#include "../tx/mp4_demux.h"
#include "../tx/extradata_handler.h"
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failed = 0;
#define CHECK(cond, msg, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: " msg "\n", ##__VA_ARGS__); failed++; } \
    else { printf("  ✓ " msg "\n", ##__VA_ARGS__); } \
} while(0)

/* ================================================================
 * Phase 1: MP4 Demux basic validation
 * ================================================================ */
static int test_phase1_demux(const char* mp4_file) {
    printf("\n══════════ Phase 1: MP4 Demux Validation ══════════\n");
    printf("  File: %s\n\n", mp4_file);

    mp4_demux_ctx* ctx = NULL;
    int ret = mp4_demux_open(mp4_file, &ctx);
    CHECK(ret == 0, "mp4_demux_open() returns 0", 0);

    const mp4_stream_info* vs = mp4_demux_get_video_stream(ctx);
    CHECK(vs != NULL, "video stream found", 0);
    CHECK(vs->codec_id == 27, "video codec is H.264 (AV_CODEC_ID_H264=27), got %d", vs->codec_id);
    CHECK(vs->width == 1920, "video width = %d (expected 1920)", vs->width);
    CHECK(vs->height == 1080, "video height = %d (expected 1080)", vs->height);
    CHECK(vs->fps > 20.0f && vs->fps < 25.0f, "fps = %.3f (expected ~23.976)", (double)vs->fps);
    printf("  Video stream: %dx%d @ %.3ffps  codec=%d  extradata=%d bytes\n",
           vs->width, vs->height, (double)vs->fps, vs->codec_id, vs->extradata_size);

    const mp4_stream_info* as = mp4_demux_get_audio_stream(ctx);
    if (as) {
        CHECK(as->sample_rate == 48000, "audio sample_rate = %d (expected 48000)", as->sample_rate);
        CHECK(as->channels == 2, "audio channels = %d (expected 2)", as->channels);
        printf("  Audio stream: %dHz %dch  codec=%d\n",
               as->sample_rate, as->channels, as->codec_id);
    } else {
        printf("  Audio stream: none\n");
    }

    /* read first 30 video frames */
    printf("\n  Reading first 30 video packets...\n");
    int video_count = 0, idr_count = 0, nal_ok = 0;
    AVPacket* pkt = av_packet_alloc();
    bool is_video;

    for (int i = 0; i < 500 && video_count < 30; i++) {
        ret = mp4_demux_read_packet(ctx, pkt, &is_video);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) { printf("  read error %d at packet %d\n", ret, i); break; }
        if (!is_video) { av_packet_unref(pkt); continue; }

        video_count++;
        CHECK(pkt->data != NULL, "video pkt[%d] data non-null", video_count);
        CHECK(pkt->size > 0, "video pkt[%d] size=%d > 0", video_count, (int)pkt->size);

        /* check Annex B start code and log ALL NAL types */
        if (pkt->size >= 4 &&
            pkt->data[0] == 0x00 && pkt->data[1] == 0x00) {
            nal_ok++;
            if (video_count <= 3) {
                printf("    pkt[%d]: size=%d PTS=%lld NALs:",
                       video_count, (int)pkt->size, (long long)pkt->pts);
                /* scan all NAL units */
                for (size_t j = 0; j + 4 < (size_t)pkt->size; j++) {
                    if (pkt->data[j] == 0x00 && pkt->data[j+1] == 0x00 &&
                        (pkt->data[j+2] == 0x01 ||
                         (pkt->data[j+2] == 0x00 && pkt->data[j+3] == 0x01))) {
                        int nal_offs = (pkt->data[j+2] == 0x01) ? (j+3) : (j+4);
                        if (nal_offs < (int)pkt->size)
                            printf(" %d", pkt->data[nal_offs] & 0x1f);
                    }
                }
                printf("\n");
            }
            /* IDR check: scan all NALs */
            for (size_t j = 0; j + 4 < (size_t)pkt->size; j++) {
                if (pkt->data[j] == 0x00 && pkt->data[j+1] == 0x00 &&
                    (pkt->data[j+2] == 0x01 ||
                     (pkt->data[j+2] == 0x00 && pkt->data[j+3] == 0x01))) {
                    int nal_offs = (pkt->data[j+2] == 0x01) ? (j+3) : (j+4);
                    if (nal_offs < (int)pkt->size &&
                        (pkt->data[nal_offs] & 0x1f) == 5) {
                        idr_count++;
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    CHECK(video_count == 30, "read 30 video frames (got %d)", video_count);
    CHECK(nal_ok > 0, "Annex B start codes detected in %d/%d packets", nal_ok, video_count);
    CHECK(idr_count > 0, "IDR frames found: %d (GOP ≈ %d frames)",
          idr_count, idr_count > 0 ? (video_count / idr_count) : 0);

    mp4_demux_close(ctx);
    printf("\n  Phase 1 result: %d checks failed\n", failed);
    return failed;
}

/* ================================================================
 * Phase 2: Extradata Handler validation
 * ================================================================ */
static int test_phase2_extradata(const char* mp4_file) {
    printf("\n══════════ Phase 2: Extradata Handler Validation ══════════\n\n");

    mp4_demux_ctx* ctx = NULL;
    int ret = mp4_demux_open(mp4_file, &ctx);
    CHECK(ret == 0, "mp4_demux_open()", 0);

    const mp4_stream_info* vs = mp4_demux_get_video_stream(ctx);
    CHECK(vs != NULL, "video stream found", 0);
    CHECK(vs->extradata != NULL, "extradata present", 0);
    CHECK(vs->extradata_size > 0, "extradata_size = %d > 0", vs->extradata_size);

    printf("  extradata: %d bytes (AVCC format)\n", vs->extradata_size);
    printf("  AVCC header: ");
    for (int i = 0; i < 8 && i < vs->extradata_size; i++)
        printf("%02x ", vs->extradata[i]);
    printf("\n");

    /* AVCC extradata format check: first byte must be 0x01 (version) */
    CHECK(vs->extradata[0] == 0x01,
          "AVCC extradata version = 0x%02x (expected 0x01)", vs->extradata[0]);

    /* create handler — it converts AVCC→Annex B internally */
    extradata_handler* eh = extradata_handler_create(vs->extradata, vs->extradata_size);
    CHECK(eh != NULL, "extradata_handler_create() success", 0);
    CHECK(extradata_get_size(eh) > 0, "Annex B extradata size = %zu (>0)", extradata_get_size(eh));

    /* verify SPS and PPS in Annex B converted data */
    const uint8_t* annexb = extradata_get_data(eh);
    size_t annexb_size = extradata_get_size(eh);
    int sps_found = 0, pps_found = 0;
    for (size_t i = 0; i + 4 < annexb_size; i++) {
        if (annexb[i] == 0x00 && annexb[i+1] == 0x00 &&
            (annexb[i+2] == 0x01 || (annexb[i+2] == 0x00 && annexb[i+3] == 0x01))) {
            int offset = (annexb[i+2] == 0x01) ? (i+3) : (i+4);
            if (offset < (int)annexb_size) {
                int nal = annexb[offset] & 0x1f;
                if (nal == 7) sps_found++;
                if (nal == 8) pps_found++;
            }
        }
    }
    CHECK(sps_found >= 1, "SPS (NAL type=7) in Annex B conversion: %d", sps_found);
    CHECK(pps_found >= 1, "PPS (NAL type=8) in Annex B conversion: %d", pps_found);

    /* find first IDR frame and test splice */
    printf("\n  Searching for first IDR frame...\n");
    AVPacket* pkt = av_packet_alloc();
    bool is_video;
    int idr_idx = -1, count = 0;

    for (int i = 0; i < 600; i++) {
        ret = mp4_demux_read_packet(ctx, pkt, &is_video);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) break;
        if (!is_video) { av_packet_unref(pkt); continue; }
        count++;

        if (extradata_is_idr(pkt->data, pkt->size)) {
            idr_idx = count;
            printf("  Found IDR at video frame #%d  size=%d PTS=%lld\n",
                   count, (int)pkt->size, (long long)pkt->pts);

            CHECK(pkt->size > 0, "IDR frame size > 0", 0);

            /* test splice */
            size_t spliced_size = 0;
            uint8_t* spliced = extradata_splice_idr(eh, pkt->data, pkt->size,
                                                    &spliced_size);
            CHECK(spliced != NULL, "extradata_splice_idr() success", 0);
            CHECK(spliced_size == annexb_size + (size_t)pkt->size,
                  "spliced size = %zu (expected %zu + %d)",
                  spliced_size, annexb_size, (int)pkt->size);

            /* verify extradata prefix in spliced buffer */
            int match = (memcmp(spliced, annexb, annexb_size) == 0);
            CHECK(match, "spliced buffer starts with Annex B extradata", 0);

            free(spliced);
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    CHECK(idr_idx > 0, "first IDR found at video frame #%d", idr_idx);

    extradata_handler_free(eh);
    mp4_demux_close(ctx);
    printf("\n  Phase 2 result: %d checks failed\n", failed);
    return failed;
}

/* ================================================================
 * Phase 3: TX Scheduler Dry-Run (no MTL network send)
 * ================================================================ */
static int test_phase3_scheduler(const char* mp4_file, int max_frames) {
    printf("\n══════════ Phase 3: TX Scheduler Dry-Run ══════════\n");
    printf("  File: %s  max_frames: %d\n\n", mp4_file, max_frames);

    mp4_demux_ctx* ctx = NULL;
    int ret = mp4_demux_open(mp4_file, &ctx);
    CHECK(ret == 0, "mp4_demux_open()", 0);

    const mp4_stream_info* vs = mp4_demux_get_video_stream(ctx);
    const mp4_stream_info* as = mp4_demux_get_audio_stream(ctx);

    /* create extradata handler */
    extradata_handler* eh = NULL;
    if (vs && vs->extradata) {
        eh = extradata_handler_create(vs->extradata, vs->extradata_size);
    }

    /* track PTS timeline */
    int64_t first_video_pts = -1, last_video_pts = 0;
    int64_t first_audio_pts = -1, last_audio_pts = 0;
    int video_count = 0, audio_count = 0, idr_count = 0;
    size_t total_video_bytes = 0, max_frame_bytes = 0;

    AVPacket* pkt = av_packet_alloc();
    bool is_video;
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    while (video_count < max_frames) {
        ret = mp4_demux_read_packet(ctx, pkt, &is_video);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) break;

        if (is_video) {
            video_count++;
            total_video_bytes += pkt->size;
            if (pkt->size > max_frame_bytes) max_frame_bytes = pkt->size;
            if (first_video_pts < 0) first_video_pts = pkt->pts;
            last_video_pts = pkt->pts;

            if (eh && extradata_is_idr(pkt->data, pkt->size)) {
                idr_count++;
                size_t spliced_size;
                uint8_t* spliced = extradata_splice_idr(eh, pkt->data,
                                                        pkt->size, &spliced_size);
                if (spliced) {
                    /* simulate MTL send: would call mtl_tx_mp4_send_video(spliced, spliced_size, tai_ns) */
                    free(spliced);
                }
            }

            if (video_count <= 3 || video_count == max_frames) {
                printf("  video[%d]: size=%d PTS=%lld key=%d\n",
                       video_count, (int)pkt->size, (long long)pkt->pts,
                       extradata_is_idr(pkt->data, pkt->size) ? 1 : 0);
            }
        } else if (as) {
            audio_count++;
            if (first_audio_pts < 0) first_audio_pts = pkt->pts;
            last_audio_pts = pkt->pts;
            /* simulate AAC→PCM decode + MTL send */
        }
        av_packet_unref(pkt);

        if (video_count % 100 == 0 && video_count > 0)
            printf("  ... processed %d video frames\n", video_count);
    }
    av_packet_free(&pkt);

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed_s = (t_end.tv_sec - t_start.tv_sec) +
                       (t_end.tv_nsec - t_start.tv_nsec) / 1e9;
    double fps_actual = video_count / elapsed_s;

    printf("\n  Stats:\n");
    printf("    Video frames:  %d (%d IDR)\n", video_count, idr_count);
    printf("    Audio packets: %d\n", audio_count);
    printf("    Total video:   %.1f MB (avg %.1f KB/frame, max %.1f KB)\n",
           total_video_bytes / 1e6,
           total_video_bytes / (double)video_count / 1024.0,
           max_frame_bytes / 1024.0);
    printf("    PTS range:     video [%lld..%lld]  audio [%lld..%lld]\n",
           (long long)first_video_pts, (long long)last_video_pts,
           (long long)first_audio_pts, (long long)last_audio_pts);

    /* validate PTS monotonic */
    CHECK(first_video_pts >= 0, "first video PTS non-negative (%lld)", (long long)first_video_pts);
    CHECK(last_video_pts > first_video_pts, "PTS monotonically increasing", 0);

    /* PTS interval check (23.976fps → ~41708 PTS units per frame at 1/24000 time_base) */
    int64_t pts_per_frame = (last_video_pts - first_video_pts) / (video_count - 1);
    printf("    PTS interval:  %lld (expected ~1001 @ time_base=1/24000 for 23.976fps)\n",
           (long long)pts_per_frame);
    CHECK(pts_per_frame >= 900 && pts_per_frame <= 1100,
          "PTS interval ~1001 (23.976fps @ 1/24000), got %lld", (long long)pts_per_frame);

    printf("    Processing:    %.1f fps (%.1fx real-time)\n",
           fps_actual, fps_actual / 23.976);
    CHECK(fps_actual > 100.0, "processing speed > 100fps (no MTL backpressure), got %.1f", fps_actual);

    if (eh) extradata_handler_free(eh);
    mp4_demux_close(ctx);
    printf("\n  Phase 3 result: %d checks failed\n", failed);
    return failed;
}

static bool h264_pkt_has_sps(const uint8_t* data, size_t size) {
    for (size_t i = 0; i + 4 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 &&
            (data[i + 3] & 0x1f) == 7)
            return true;
    }
    return false;
}

static AVCodecContext* ffmpeg_dec_open(const AVCodecParameters* par) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return NULL;

    AVCodecContext* dec = avcodec_alloc_context3(codec);
    if (!dec) return NULL;

    if (avcodec_parameters_to_context(dec, par) < 0) {
        avcodec_free_context(&dec);
        return NULL;
    }

    if (avcodec_open2(dec, codec, NULL) < 0) {
        avcodec_free_context(&dec);
        return NULL;
    }
    return dec;
}

static void ffmpeg_dec_close(AVCodecContext** dec) {
    if (!dec || !*dec) return;
    avcodec_free_context(dec);
    *dec = NULL;
}

/* Drain all frames produced by the most recent send_packet. */
static int ffmpeg_dec_feed(AVCodecContext* dec, AVPacket* pkt, AVFrame* frame,
                           int* frames_out, int* recv_err) {
    int ret = avcodec_send_packet(dec, pkt);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        return ret;
    }

    while (1) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            (*recv_err)++;
            break;
        }
        (*frames_out)++;
        av_frame_unref(frame);
    }
    return 0;
}

/* ================================================================
 * Phase 4: FFmpeg soft decode — same demux+BSF path as TX/RX
 *
 * Proves the Annex B stream is decodable when one decoder keeps
 * state across ST22-sized packets (continuous mode).
 * phase4_neg is the negative control: new decoder per packet.
 * ================================================================ */
static int test_phase4_ffmpeg_decode(const char* mp4_file, int max_packets,
                                     bool isolated) {
    printf("\n══════════ Phase 4: FFmpeg %s Decode ══════════\n",
           isolated ? "Isolated (negative control)" : "Continuous");
    printf("  File: %s  max_packets: %d\n\n", mp4_file, max_packets);

    mp4_demux_ctx* ctx = NULL;
    int ret = mp4_demux_open(mp4_file, &ctx);
    CHECK(ret == 0, "mp4_demux_open()", 0);
    if (ret < 0) return failed;

    const mp4_stream_info* vs = mp4_demux_get_video_stream(ctx);
    const AVCodecParameters* par = mp4_demux_get_video_codecpar(ctx);
    CHECK(vs != NULL && par != NULL, "video stream / codecpar present", 0);

    extradata_handler* eh = NULL;
    if (vs->extradata && vs->extradata_size > 0)
        eh = extradata_handler_create(vs->extradata, vs->extradata_size);

    AVCodecContext* dec = isolated ? NULL : ffmpeg_dec_open(par);
    if (!isolated)
        CHECK(dec != NULL, "ffmpeg decoder open (h264)", 0);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int pkt_in = 0, frames_out = 0, send_err = 0, recv_err = 0;
    int idr_splice = 0, small_pkts = 0;

    while (pkt_in < max_packets) {
        ret = mp4_demux_read_video(ctx, pkt);
        if (ret == AVERROR_EOF) break;
        if (ret < 0) {
            fprintf(stderr, "  read error %d at pkt %d\n", ret, pkt_in);
            break;
        }

        const uint8_t* send_data = pkt->data;
        size_t send_size = (size_t)pkt->size;
        uint8_t* spliced = NULL;

        if ((size_t)pkt->size < 200) small_pkts++;

        /* Match TX: splice SPS/PPS before IDR when BSF did not embed them */
        if (eh && extradata_is_idr(pkt->data, pkt->size) &&
            !h264_pkt_has_sps(pkt->data, pkt->size)) {
            spliced = extradata_splice_idr(eh, pkt->data, pkt->size, &send_size);
            if (spliced) {
                send_data = spliced;
                idr_splice++;
            }
        }

        AVPacket feed;
        av_init_packet(&feed);
        feed.data = (uint8_t*)send_data;
        feed.size = (int)send_size;

        if (isolated) {
            AVCodecContext* one = ffmpeg_dec_open(par);
            if (!one) {
                send_err++;
            } else {
                if (ffmpeg_dec_feed(one, &feed, frame, &frames_out, &recv_err) < 0)
                    send_err++;
                ffmpeg_dec_close(&one);
            }
        } else {
            if (ffmpeg_dec_feed(dec, &feed, frame, &frames_out, &recv_err) < 0)
                send_err++;
        }

        free(spliced);
        av_packet_unref(pkt);
        pkt_in++;
    }

    /* Flush continuous decoder */
    if (!isolated && dec) {
        AVPacket flush;
        av_init_packet(&flush);
        flush.data = NULL;
        flush.size = 0;
        avcodec_send_packet(dec, &flush);
        while (avcodec_receive_frame(dec, frame) == 0) {
            frames_out++;
            av_frame_unref(frame);
        }
    }

    double yield = (pkt_in > 0) ? (100.0 * frames_out / pkt_in) : 0.0;

    printf("\n  Results (same path as mp4_dist TX: demux + h264_mp4toannexb");
    if (idr_splice > 0) printf(" + IDR splice");
    printf("):\n");
    printf("    packets_in:   %d\n", pkt_in);
    printf("    frames_out:   %d\n", frames_out);
    printf("    yield:        %.1f%%  (frames / packets)\n", yield);
    printf("    send_errors:  %d\n", send_err);
    printf("    recv_errors:  %d\n", recv_err);
    printf("    idr_splice:   %d\n", idr_splice);
    printf("    pkts <200B:   %d (%.1f%%)\n", small_pkts,
           pkt_in > 0 ? 100.0 * small_pkts / pkt_in : 0.0);
    printf("    source fps:   %.3f\n", (double)vs->fps);

    if (isolated) {
        CHECK(frames_out < pkt_in / 4,
              "isolated decode yields poorly (got %.1f%%, expect <<25%%)", yield);
    } else {
        CHECK(yield >= 80.0,
              "continuous decode yield >= 80%% (got %.1f%%)", yield);
        CHECK(send_err == 0, "no send errors (got %d)", send_err);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    ffmpeg_dec_close(&dec);
    if (eh) extradata_handler_free(eh);
    mp4_demux_close(ctx);

    printf("\n  Phase 4 result: %d checks failed\n", failed);
    return failed;
}

/* ================================================================
 * main
 * ================================================================ */
int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage:\n"
            "  %s phase1 <mp4_file>\n"
            "  %s phase2 <mp4_file>\n"
            "  %s phase3 <mp4_file> [max_frames=300]\n"
            "  %s phase4 <mp4_file> [max_packets=1440]\n"
            "  %s phase4_neg <mp4_file> [max_packets=500]\n",
            argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    const char* phase = argv[1];
    const char* file  = argv[2];

    printf("═══ MP4 Distribution Test Harness ═══\n");
    printf("Phase: %s  File: %s\n", phase, file);

    if (strcmp(phase, "phase1") == 0) {
        return test_phase1_demux(file);
    } else if (strcmp(phase, "phase2") == 0) {
        return test_phase2_extradata(file);
    } else if (strcmp(phase, "phase3") == 0) {
        int max_frames = (argc > 3) ? atoi(argv[3]) : 300;
        return test_phase3_scheduler(file, max_frames);
    } else if (strcmp(phase, "phase4") == 0) {
        int max_packets = (argc > 3) ? atoi(argv[3]) : 1440;
        return test_phase4_ffmpeg_decode(file, max_packets, false);
    } else if (strcmp(phase, "phase4_neg") == 0) {
        int max_packets = (argc > 3) ? atoi(argv[3]) : 500;
        return test_phase4_ffmpeg_decode(file, max_packets, true);
    } else {
        fprintf(stderr, "Unknown phase: %s\n", phase);
        return 1;
    }
}
