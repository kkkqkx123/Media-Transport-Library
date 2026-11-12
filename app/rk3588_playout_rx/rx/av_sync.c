#include "av_sync.h"
#include <stdio.h>
#include <time.h>

/* Hold the first frame until a second frame is decoded, so the steady
 * timeline does not start from a single cold-start sample. */
#define AV_SYNC_STARTUP_WATERMARK 2
/* Adaptive buffering stays within the historic worst case. */
#define AV_SYNC_MAX_EFFECTIVE_NS (80ULL * 1000000ULL)
/* Occupancy tracker tuning: mostly empty vs clearly building, with enough
 * persistence to turn chronic per-frame loss into one single-step jump. */
#define AV_SYNC_OCC_ALPHA 0.125
#define AV_SYNC_OCC_LOW 0.7
#define AV_SYNC_OCC_HIGH 3.0
#define AV_SYNC_STREAK_N 60

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

static int64_t servo_min_ns(const av_sync* sync) {
    uint64_t base = sync ? sync->playout_delay_ns : 0;
    uint64_t cap = base > AV_SYNC_MAX_EFFECTIVE_NS ? base : AV_SYNC_MAX_EFFECTIVE_NS;
    return (int64_t)base - (int64_t)cap;
}

static int64_t servo_max_ns(const av_sync* sync) {
    return (int64_t)(sync ? sync->playout_delay_ns : 0);
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
    sync->servo_offset_ns   = 0;
    sync->startup_anchored  = false;
    sync->occ_ema           = 0.0;
    sync->low_streak        = 0;
    sync->high_streak       = 0;
    sync->frame_period_ns   = 0;
    sync->stat_servo_jumps  = 0;
    pthread_mutex_init(&sync->lock, NULL);
    printf("av_sync: delay=%dms clock=%s (startup watermark plus occupancy servo)\n",
           delay_ms, time_now ? "ptp" : "monotonic");
}

void av_sync_set_frame_period(av_sync* sync, uint64_t period_ns) {
    if (!sync)
        return;
    sync->frame_period_ns = period_ns;
}

bool av_sync_startup_gate(av_sync* sync, int depth) {
    if (!sync)
        return true;
    if (sync->startup_anchored)
        return true;
    if (depth < AV_SYNC_STARTUP_WATERMARK)
        return false;
    sync->startup_anchored = true;
    sync->occ_ema          = (double)depth;
    sync->low_streak       = 0;
    sync->high_streak      = 0;
    return true;
}

void av_sync_track_occupancy(av_sync* sync, int depth) {
    if (!sync || !sync->startup_anchored)
        return;
    if (sync->frame_period_ns == 0)
        return;

    sync->occ_ema += ((double)depth - sync->occ_ema) * AV_SYNC_OCC_ALPHA;

    if (sync->occ_ema < AV_SYNC_OCC_LOW) {
        sync->low_streak++;
        sync->high_streak = 0;
    } else if (sync->occ_ema > AV_SYNC_OCC_HIGH) {
        sync->high_streak++;
        sync->low_streak = 0;
    } else {
        sync->low_streak  = 0;
        sync->high_streak = 0;
        return;
    }

    int64_t step = (int64_t)sync->frame_period_ns;
    if (sync->low_streak >= AV_SYNC_STREAK_N) {
        int64_t next = sync->servo_offset_ns - step;
        int64_t lo = servo_min_ns(sync);
        if (next < lo)
            next = lo;
        if (next != sync->servo_offset_ns) {
            sync->servo_offset_ns = next;
            sync->stat_servo_jumps++;
        }
        sync->low_streak  = 0;
        sync->high_streak = 0;
    } else if (sync->high_streak >= AV_SYNC_STREAK_N) {
        int64_t next = sync->servo_offset_ns + step;
        int64_t hi = servo_max_ns(sync);
        if (next > hi)
            next = hi;
        if (next != sync->servo_offset_ns) {
            sync->servo_offset_ns = next;
            sync->stat_servo_jumps++;
        }
        sync->low_streak  = 0;
        sync->high_streak = 0;
    }
}

int64_t av_sync_effective_delay_ns(const av_sync* sync) {
    if (!sync)
        return 0;
    uint64_t base = sync->playout_delay_ns;
    uint64_t cap = base > AV_SYNC_MAX_EFFECTIVE_NS ? base : AV_SYNC_MAX_EFFECTIVE_NS;
    int64_t eff = (int64_t)base - sync->servo_offset_ns;
    if (eff < 0)
        return 0;
    if ((uint64_t)eff > cap)
        return (int64_t)cap;
    return eff;
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
    uint64_t now = sync_now_ns(sync);
    int64_t eff = av_sync_effective_delay_ns(sync);
    uint64_t delay = eff > 0 ? (uint64_t)eff : 0;
    return now > delay ? now - delay : 0;
}

void av_sync_print_stats(const av_sync* sync) {
    int64_t eff = av_sync_effective_delay_ns(sync);
    printf("av_sync: rendered=%llu dropped=%llu repeated=%llu audio_started=%d "
           "servo_jumps=%llu effective_delay_ms=%lld\n",
           (unsigned long long)sync->stat_rendered,
           (unsigned long long)sync->stat_dropped,
           (unsigned long long)sync->stat_repeated,
           sync->audio_started ? 1 : 0,
           (unsigned long long)sync->stat_servo_jumps,
           (long long)(eff / 1000000LL));
}
