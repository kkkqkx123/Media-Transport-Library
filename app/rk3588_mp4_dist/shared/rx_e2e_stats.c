/* SPDX-License-Identifier: BSD-3-Clause */

#include "rx_e2e_stats.h"
#include <stdio.h>
#include <string.h>

static void stats_get_locked(const rx_e2e_stats* s, bool period, uint64_t* min_ns,
                             uint64_t* avg_ns, uint64_t* max_ns, uint64_t* count) {
    uint64_t c, sum, minv, maxv;

    if (period) {
        c   = s->p_count;
        sum = s->p_sum_ns;
        minv = s->p_min_ns;
        maxv = s->p_max_ns;
    } else {
        c   = s->count;
        sum = s->sum_ns;
        minv = s->min_ns;
        maxv = s->max_ns;
    }

    if (count)
        *count = c;
    if (!c) {
        if (min_ns)
            *min_ns = 0;
        if (avg_ns)
            *avg_ns = 0;
        if (max_ns)
            *max_ns = 0;
        return;
    }
    if (min_ns)
        *min_ns = minv;
    if (max_ns)
        *max_ns = maxv;
    if (avg_ns)
        *avg_ns = sum / c;
}

void rx_e2e_stats_init(rx_e2e_stats* s) {
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);
}

void rx_e2e_stats_destroy(rx_e2e_stats* s) {
    if (!s)
        return;
    pthread_mutex_destroy(&s->lock);
}

void rx_e2e_stats_record(rx_e2e_stats* s, uint64_t capture_tai_ns,
                         uint64_t present_ns, const char* label) {
    if (!s || !capture_tai_ns)
        return;

    if (present_ns < capture_tai_ns) {
        uint64_t diff_ns = capture_tai_ns - present_ns;
        pthread_mutex_lock(&s->lock);
        s->skew_reject++;
        s->p_skew_reject++;
        if (diff_ns > s->skew_max_diff_ns)
            s->skew_max_diff_ns = diff_ns;
        if (diff_ns > s->p_skew_max_diff_ns)
            s->p_skew_max_diff_ns = diff_ns;
        if (!s->skew_logged) {
            s->skew_logged = 1;
            fprintf(stderr,
                    "e2e: skew[%s] capture=%llu present=%llu diff_ms=%.3f "
                    "(TX stamp ahead of RX present — check PTP tai_clock TX/RX)\n",
                    label ? label : "?",
                    (unsigned long long)capture_tai_ns,
                    (unsigned long long)present_ns,
                    (double)diff_ns / 1000000.0);
        }
        pthread_mutex_unlock(&s->lock);
        return;
    }

    uint64_t lat = present_ns - capture_tai_ns;

    pthread_mutex_lock(&s->lock);

    if (!s->count) {
        s->min_ns = lat;
        s->max_ns = lat;
    } else {
        if (lat < s->min_ns)
            s->min_ns = lat;
        if (lat > s->max_ns)
            s->max_ns = lat;
    }
    s->sum_ns += lat;
    s->count++;

    if (!s->p_count) {
        s->p_min_ns = lat;
        s->p_max_ns = lat;
    } else {
        if (lat < s->p_min_ns)
            s->p_min_ns = lat;
        if (lat > s->p_max_ns)
            s->p_max_ns = lat;
    }
    s->p_sum_ns += lat;
    s->p_count++;

    pthread_mutex_unlock(&s->lock);
}

void rx_e2e_stats_get(const rx_e2e_stats* s, uint64_t* min_ns, uint64_t* avg_ns,
                      uint64_t* max_ns, uint64_t* count) {
    if (!s)
        return;
    pthread_mutex_lock((pthread_mutex_t*)&s->lock);
    stats_get_locked(s, false, min_ns, avg_ns, max_ns, count);
    pthread_mutex_unlock((pthread_mutex_t*)&s->lock);
}

void rx_e2e_stats_get_period(const rx_e2e_stats* s, uint64_t* min_ns,
                             uint64_t* avg_ns, uint64_t* max_ns, uint64_t* count) {
    if (!s)
        return;
    pthread_mutex_lock((pthread_mutex_t*)&s->lock);
    stats_get_locked(s, true, min_ns, avg_ns, max_ns, count);
    pthread_mutex_unlock((pthread_mutex_t*)&s->lock);
}

void rx_e2e_stats_reset_period(rx_e2e_stats* s) {
    if (!s)
        return;
    pthread_mutex_lock(&s->lock);
    s->p_min_ns       = 0;
    s->p_max_ns       = 0;
    s->p_sum_ns       = 0;
    s->p_count        = 0;
    s->p_skew_reject  = 0;
    s->p_skew_max_diff_ns = 0;
    pthread_mutex_unlock(&s->lock);
}

uint64_t rx_e2e_stats_skew_reject_period(const rx_e2e_stats* s) {
    if (!s)
        return 0;
    pthread_mutex_lock((pthread_mutex_t*)&s->lock);
    uint64_t n = s->p_skew_reject;
    pthread_mutex_unlock((pthread_mutex_t*)&s->lock);
    return n;
}

static uint64_t rx_e2e_stats_skew_max_diff_period(const rx_e2e_stats* s) {
    if (!s)
        return 0;
    pthread_mutex_lock((pthread_mutex_t*)&s->lock);
    uint64_t n = s->p_skew_max_diff_ns;
    pthread_mutex_unlock((pthread_mutex_t*)&s->lock);
    return n;
}

void rx_e2e_stats_print(const rx_e2e_stats* s, const char* label, bool period_only,
                        uint64_t stamp_delta, bool has_time_now) {
    uint64_t min_ns, avg_ns, max_ns, count;
    uint64_t skew = rx_e2e_stats_skew_reject_period(s);
    uint64_t skew_max_diff_ns = rx_e2e_stats_skew_max_diff_period(s);

    if (period_only)
        rx_e2e_stats_get_period(s, &min_ns, &avg_ns, &max_ns, &count);
    else
        rx_e2e_stats_get(s, &min_ns, &avg_ns, &max_ns, &count);

    if (count) {
        printf("  E2E-C[%s]: n=%llu min=%llu avg=%llu max=%llu us\n",
               label ? label : "video",
               (unsigned long long)count,
               (unsigned long long)(min_ns / 1000),
               (unsigned long long)(avg_ns / 1000),
               (unsigned long long)(max_ns / 1000));
        return;
    }

    const char* hint = "check TX/RX build + PTP";
    if (!has_time_now)
        hint = "need ptp_mode external/builtin on RX";
    else if (!stamp_delta)
        hint = "no in-band stamp (rebuild+restart mp4_dist_tx)";
    else if (skew)
        hint = "PTP skew (TX clock ahead of RX?)";

    printf("  E2E-C[%s]: (no samples) stamps=+%llu skew_rej=+%llu",
           label ? label : "video",
           (unsigned long long)stamp_delta,
           (unsigned long long)skew);
    if (skew && skew_max_diff_ns)
        printf(" skew_max_diff_ms=%.1f", (double)skew_max_diff_ns / 1000000.0);
    printf(" %s\n", hint);
}
