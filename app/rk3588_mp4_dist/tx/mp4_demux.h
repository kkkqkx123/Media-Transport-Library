/* SPDX-License-Identifier: BSD-3-Clause
 *
 * FFmpeg-based MP4 file demuxer.
 * Opens an MP4 file, reads AVPackets without video decoding.
 * Video packets are returned in Annex B format (FFmpeg auto-converts).
 */

#ifndef _MP4_DEMUX_H_
#define _MP4_DEMUX_H_

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Demux context — opaque to caller. */
typedef struct mp4_demux_ctx mp4_demux_ctx;

/** Summary of a stream discovered in the MP4 file. */
typedef struct mp4_stream_info {
    int      index;         /* stream_index in AVFormatContext */
    int      codec_id;      /* AVCodecID */
    int      width;         /* video only, 0 for audio */
    int      height;        /* video only, 0 for audio */
    float    fps;           /* video only, 0 for audio */
    int      sample_rate;   /* audio only, 0 for video */
    int      channels;      /* audio only, 0 for video */
    int      time_base_num; /* AVRational.num */
    int      time_base_den; /* AVRational.den */
    uint8_t* extradata;     /* codec extradata (SPS+PPS for H.264) */
    int      extradata_size;
} mp4_stream_info;

/**
 * Open an MP4 file and probe streams.
 * @param path     Filesystem path to the .mp4 file
 * @param ctx_out  [out] receives the demux context
 * @return 0 on success, negative errno on failure
 */
int mp4_demux_open(const char* path, mp4_demux_ctx** ctx_out);

/**
 * Get stream info for the first video stream found.
 * @return pointer to static info, valid until mp4_demux_close()
 */
const mp4_stream_info* mp4_demux_get_video_stream(mp4_demux_ctx* ctx);

/**
 * Get stream info for the first audio stream found.
 * @return pointer to static info, or NULL if no audio stream
 */
const mp4_stream_info* mp4_demux_get_audio_stream(mp4_demux_ctx* ctx);

/**
 * Get FFmpeg codec parameters for the audio stream (for avcodec_parameters_to_context).
 * @return pointer valid until mp4_demux_close(), or NULL if no audio stream
 */
const AVCodecParameters* mp4_demux_get_audio_codecpar(mp4_demux_ctx* ctx);

/** FFmpeg codec parameters for the video stream. */
const AVCodecParameters* mp4_demux_get_video_codecpar(mp4_demux_ctx* ctx);

/**
 * Read the next packet from the MP4 file.
 * @param ctx         demux context
 * @param pkt         [in/out] caller-allocated AVPacket (av_packet_alloc)
 * @param is_video    [out] true if this is a video packet
 * @return 0 on success, AVERROR_EOF at end, negative on error
 */
int mp4_demux_read_packet(mp4_demux_ctx* ctx, AVPacket* pkt, bool* is_video);

/** Read next video packet only (skips audio/other streams). */
int mp4_demux_read_video(mp4_demux_ctx* ctx, AVPacket* pkt);

/** Read next audio packet only (skips video/other streams). */
int mp4_demux_read_audio(mp4_demux_ctx* ctx, AVPacket* pkt);

/**
 * Probe MP4 file without opening a full demux session.
 * @return 0 on success, negative errno on failure
 */
int mp4_probe_file(const char* path, int* width, int* height, float* fps);

/**
 * Seek to the beginning of the file (for looping).
 * @return 0 on success
 */
int mp4_demux_seek_start(mp4_demux_ctx* ctx);

/** Seek to position in milliseconds (applies to all streams). */
int mp4_demux_seek_ms(mp4_demux_ctx* ctx, int64_t ms);

/** Close the demuxer and free all resources. */
void mp4_demux_close(mp4_demux_ctx* ctx);

#endif /* _MP4_DEMUX_H_ */
