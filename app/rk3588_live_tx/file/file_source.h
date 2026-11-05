#ifndef _LIVE_INGEST_FILE_SOURCE_H_
#define _LIVE_INGEST_FILE_SOURCE_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mtl_tx_audio;
struct tx_mtl_sink_ops;

/* File frame-source plugin (phase2): MP4 demux + playlist + send scheduling.
 * Single video lane; feeds the encode-side send sink and the ST30 session.
 * Self-contained FFmpeg usage, no cross-package sources. */
struct file_source;

#define FILE_SOURCE_MAX_FILES 16
#define FILE_SOURCE_MAX_PATH 256

struct file_source_params {
    void* st; /* mtl_handle, kept void to avoid MTL header cascade */
    struct tx_mtl_sink_ops* video_sink;
    struct mtl_tx_audio*    audio_tx;
    char   files[FILE_SOURCE_MAX_FILES][FILE_SOURCE_MAX_PATH];
    int    file_count;
    bool   loop;
    int64_t seek_ms;
    size_t codestream_max; /* must match mtl_tx framebuff_max */
    int    width;
    int    height;
    float  fps; /* 0 = follow first file probe */
};

struct file_source* file_source_create(const struct file_source_params* p);
void file_source_free(struct file_source* fs);

/* Start feeder threads (non-blocking). Returns 0 on success. */
int  file_source_start(struct file_source* fs, atomic_bool* running);
/* Signal stop and join threads. */
void file_source_stop(struct file_source* fs);

/* Probe first file for width/height/fps (for config alignment). */
int file_source_probe(const char* path, int* width, int* height, float* fps);

/* Cumulative counters (lock-free reads). */
void file_source_stats(const struct file_source* fs, uint64_t* video_frames,
                       uint64_t* audio_frames, uint64_t* video_drops);

#endif
