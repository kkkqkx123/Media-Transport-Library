/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "timer.h"
#include <stdlib.h>
#include <time.h>

struct e2e_timer {
  void* mtl_handle;
  uint64_t start_ns;
};

struct e2e_timer* e2e_timer_create(void* mtl_handle) {
  struct e2e_timer* t = calloc(1, sizeof(*t));
  if (!t) return NULL;
  t->mtl_handle = mtl_handle;

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  t->start_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
  return t;
}

void e2e_timer_free(struct e2e_timer* t) {
  free(t);
}

uint64_t e2e_timer_now_ns(struct e2e_timer* t) {
  (void)t;
  /* Phase 1-4: use CLOCK_MONOTONIC.
   * Phase 5: if (t->mtl_handle) return mtl_ptp_read_time(t->mtl_handle);
   */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void e2e_timer_wait_frame(struct e2e_timer* t, float fps, uint64_t frame_count) {
  uint64_t target_ns = t->start_ns + (uint64_t)((double)frame_count / fps * 1e9);
  uint64_t now_ns    = e2e_timer_now_ns(t);
  if (now_ns >= target_ns) return;

  struct timespec req = {
      .tv_sec  = (time_t)((target_ns - now_ns) / 1000000000ULL),
      .tv_nsec = (long)((target_ns - now_ns) % 1000000000ULL),
  };
  clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
}
