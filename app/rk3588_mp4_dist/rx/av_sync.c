/* SPDX-License-Identifier: BSD-3-Clause
 *
 * A/V sync: video frame TAI PTS vs audio TAI timeline (ALSA elapsed + anchor).
 */

#include "av_sync.h"
#include <stdio.h>
#include <time.h>

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t sync_now_ns(const av_sync* sync) {
    if (sync && sync->time_now)
        return sync->time_now(sync->time_priv);
    return monotonic_ns();
}

void av_sync_init(av_sync* sync, alsa_audio* audio, playout_buffer* pb,
                  int delay_ms, uint64_t (*time_now)(void* priv), void* time_priv) {
    sync->audio             = audio;
    sync->pb                = pb;
    sync->threshold_max_ns  =  40000000;
    sync->threshold_min_ns  = -20000000;
    sync->playout_delay_ns  = (uint64_t)delay_ms * 1000000ULL;
    sync->audio_first_tai   = 0;
    sync->audio_started     = false;
    sync->stat_dropped      = 0;
    sync->stat_repeated     = 0;
    sync->stat_rendered     = 0;
    sync->time_now          = time_now;
    sync->time_priv         = time_priv;
    pthread_mutex_init(&sync->lock, NULL);
    printf("av_sync: audio=%s delay=%dms clock=%s thresholds=[%lld, %lld]ns\n",
           audio ? "on" : "off", delay_ms,
           time_now ? "ptp" : "monotonic",
           (long long)sync->threshold_min_ns,
           (long long)sync->threshold_max_ns);
}

void av_sync_note_audio_frame(av_sync* sync, uint64_t tai_ns) {
    if (!sync || !tai_ns) return;

    pthread_mutex_lock(&sync->lock);
    if (!sync->audio_started) {
        sync->audio_first_tai = tai_ns;
        sync->audio_started   = true;
        printf("av_sync: audio anchor tai=%llu\n",
               (unsigned long long)tai_ns);
    }
    pthread_mutex_unlock(&sync->lock);
}

uint64_t av_sync_playout_target_ns(const av_sync* sync) {
    uint64_t now   = sync_now_ns(sync);
    uint64_t delay = sync ? sync->playout_delay_ns : 0;
    return now > delay ? now - delay : 0;
}

enum av_sync_action av_sync_decide_for_pts(av_sync* sync, uint64_t video_tai_ns) {
    if (!sync)
        return AV_SYNC_RENDER;

    /* pts==0: legacy / no timestamp — render immediately. */
    if (!video_tai_ns) {
        sync->stat_rendered++;
        return AV_SYNC_RENDER;
    }

    /* No audio master clock — show every decoded frame (E2E behaviour). */
    if (!sync->audio) {
        sync->stat_rendered++;
        return AV_SYNC_RENDER;
    }

    pthread_mutex_lock(&sync->lock);

    /* Wait for first audio before applying sync (avoid bogus drop storm). */
    if (!sync->audio_started) {
        sync->stat_rendered++;
        pthread_mutex_unlock(&sync->lock);
        return AV_SYNC_RENDER;
    }

    double audio_sec = alsa_audio_clock_get(sync->audio);
    uint64_t audio_tai = sync->audio_first_tai +
        (uint64_t)(audio_sec * 1e9);
    uint64_t target_tai = audio_tai + sync->playout_delay_ns;
    int64_t  diff       = (int64_t)(video_tai_ns - target_tai);

    enum av_sync_action action;
    /* Safety valve: misaligned clocks → render, never mass-drop. */
    if (diff > 1000000000LL || diff < -1000000000LL) {
        sync->stat_rendered++;
        action = AV_SYNC_RENDER;
    } else if (diff > sync->threshold_max_ns) {
        sync->stat_dropped++;
        action = AV_SYNC_DROP;
    } else if (diff < sync->threshold_min_ns) {
        sync->stat_repeated++;
        action = AV_SYNC_REPEAT;
    } else {
        sync->stat_rendered++;
        action = AV_SYNC_RENDER;
    }

    pthread_mutex_unlock(&sync->lock);
    return action;
}

enum av_sync_action av_sync_decide(av_sync* sync) {
    if (!sync || !sync->pb)
        return AV_SYNC_REPEAT;

    if (!playout_buffer_has_frames(sync->pb))
        return AV_SYNC_REPEAT;

    uint64_t pts = playout_buffer_peek_pts(sync->pb);
    return av_sync_decide_for_pts(sync, pts);
}

void av_sync_print_stats(const av_sync* sync) {
    printf("av_sync: rendered=%llu dropped=%llu repeated=%llu audio_started=%d\n",
           (unsigned long long)sync->stat_rendered,
           (unsigned long long)sync->stat_dropped,
           (unsigned long long)sync->stat_repeated,
           sync->audio_started ? 1 : 0);
}
