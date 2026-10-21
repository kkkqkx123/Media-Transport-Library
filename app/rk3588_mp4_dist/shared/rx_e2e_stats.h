/* SPDX-License-Identifier: BSD-3-Clause
 *
 * RX E2E-C latency stats: present_ptp - in-band capture_tai_ns.
 */

#ifndef _RX_E2E_STATS_H_
#define _RX_E2E_STATS_H_

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct rx_e2e_stats {
    pthread_mutex_t lock;
    uint64_t        min_ns;
    uint64_t        max_ns;
    uint64_t        sum_ns;
    uint64_t        count;
    uint64_t        p_min_ns;
    uint64_t        p_max_ns;
    uint64_t        p_sum_ns;
    uint64_t        p_count;
    uint64_t        skew_reject;
    uint64_t        p_skew_reject;
    uint64_t        skew_max_diff_ns;   /* max(capture - present) on reject */
    uint64_t        p_skew_max_diff_ns;
    int             skew_logged;
} rx_e2e_stats;

void rx_e2e_stats_init(rx_e2e_stats* s);
void rx_e2e_stats_destroy(rx_e2e_stats* s);

/** Record one sample when capture_tai_ns > 0 and present_ns >= capture_tai_ns. */
void rx_e2e_stats_record(rx_e2e_stats* s, uint64_t capture_tai_ns,
                         uint64_t present_ns, const char* label);

void rx_e2e_stats_get(const rx_e2e_stats* s, uint64_t* min_ns, uint64_t* avg_ns,
                      uint64_t* max_ns, uint64_t* count);

void rx_e2e_stats_get_period(const rx_e2e_stats* s, uint64_t* min_ns,
                             uint64_t* avg_ns, uint64_t* max_ns, uint64_t* count);

/** Clear period counters after a periodic report. */
void rx_e2e_stats_reset_period(rx_e2e_stats* s);

uint64_t rx_e2e_stats_skew_reject_period(const rx_e2e_stats* s);

/**
 * Print E2E-C line (always). @stamp_delta = in-band stamps parsed this period.
 * @has_time_now false when RX cannot read PTP for present time.
 */
void rx_e2e_stats_print(const rx_e2e_stats* s, const char* label, bool period_only,
                        uint64_t stamp_delta, bool has_time_now);

#endif /* _RX_E2E_STATS_H_ */
