#ifndef _AV_SYNC_H_
#define _AV_SYNC_H_

#include "alsa_audio.h"
#include "playout_buffer.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/** Fixed-delay playout sync (no audio-master drop/repeat).
 * Video timing uses playout_target_ns + delay; the audio TAI anchor below
 * is observability only. */
typedef struct av_sync {
    alsa_audio*     audio;
    playout_buffer* pb;
    uint64_t        playout_delay_ns;

    /* TAI anchor from first played audio frame (matches TX PTP timeline) */
    uint64_t        audio_first_tai;
    bool            audio_started;

    pthread_mutex_t lock;

    uint64_t        stat_dropped;
    uint64_t        stat_repeated;
    uint64_t        stat_rendered;

    /** Wall clock for playout scheduling (PTP/TAI); NULL → CLOCK_MONOTONIC. */
    uint64_t        (*time_now)(void* priv);
    void*           time_priv;
} av_sync;

void av_sync_init(av_sync* sync, alsa_audio* audio, playout_buffer* pb,
                  int delay_ms, uint64_t (*time_now)(void* priv), void* time_priv);

/** Called from audio RX thread after each frame is written to ALSA. */
void av_sync_note_audio_frame(av_sync* sync, uint64_t tai_ns);

/**
 * Playout target PTS in the same domain as video PTS.
 * target = now - playout_delay (now from time_now or monotonic).
 */
uint64_t av_sync_playout_target_ns(const av_sync* sync);

void av_sync_print_stats(const av_sync* sync);

#endif /* _AV_SYNC_H_ */
