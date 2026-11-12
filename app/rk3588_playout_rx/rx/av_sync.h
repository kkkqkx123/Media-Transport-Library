#ifndef _AV_SYNC_H_
#define _AV_SYNC_H_

#include "alsa_audio.h"
#include "playout_buffer.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/** Fixed-delay playout sync with startup watermark and occupancy servo.
 * Video timing uses the configured delay plus a bounded adaptive offset
 * that grows buffering when the queue starves and shrinks it when the
 * queue builds; the audio TAI anchor below is observability only.
 * The servo fields are owned by the single render thread. */
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

    /* Adaptive offset applied to the target (render thread only). */
    int64_t         servo_offset_ns;
    /* True once the startup watermark has been reached. */
    bool            startup_anchored;
    /* Queue occupancy tracker for the servo. */
    double          occ_ema;
    int             low_streak;
    int             high_streak;
    uint64_t        frame_period_ns;
    uint64_t        stat_servo_jumps;
} av_sync;

void av_sync_init(av_sync* sync, alsa_audio* audio, playout_buffer* pb,
                  int delay_ms, uint64_t (*time_now)(void* priv), void* time_priv);

/** Called from audio RX thread after each frame is written to ALSA. */
void av_sync_note_audio_frame(av_sync* sync, uint64_t tai_ns);

/** Frame period used for single-step servo corrections (render thread only). */
void av_sync_set_frame_period(av_sync* sync, uint64_t period_ns);

/** Startup gate: hold rendering until the queue reaches the watermark.
 * Returns true once steady rendering may proceed. */
bool av_sync_startup_gate(av_sync* sync, int depth);

/** Feed one occupancy sample; performs a single-step timeline correction
 * when starvation or buildup persists (render thread only). */
void av_sync_track_occupancy(av_sync* sync, int depth);

/** Effective buffering delay: configured delay adjusted by the servo. */
int64_t av_sync_effective_delay_ns(const av_sync* sync);

/**
 * Playout target PTS in the same domain as video PTS.
 * target = now - effective_delay (now from time_now or monotonic).
 */
uint64_t av_sync_playout_target_ns(const av_sync* sync);

void av_sync_print_stats(const av_sync* sync);

#endif /* _AV_SYNC_H_ */
