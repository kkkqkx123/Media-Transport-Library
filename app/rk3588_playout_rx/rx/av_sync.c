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
    sync->playout_delay_ns  = (uint64_t)delay_ms * 1000000ULL;
    sync->audio_first_tai   = 0;
    sync->audio_started     = false;
    sync->stat_dropped      = 0;
    sync->stat_repeated     = 0;
    sync->stat_rendered     = 0;
    sync->time_now          = time_now;
    sync->time_priv         = time_priv;
    pthread_mutex_init(&sync->lock, NULL);
    printf("av_sync: delay=%dms clock=%s (fixed-delay playout; audio anchor is observability only)\n",
           delay_ms, time_now ? "ptp" : "monotonic");
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

void av_sync_print_stats(const av_sync* sync) {
    printf("av_sync: rendered=%llu dropped=%llu repeated=%llu audio_started=%d\n",
           (unsigned long long)sync->stat_rendered,
           (unsigned long long)sync->stat_dropped,
           (unsigned long long)sync->stat_repeated,
           sync->audio_started ? 1 : 0);
}
