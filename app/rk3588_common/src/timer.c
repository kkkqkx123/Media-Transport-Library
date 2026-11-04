#include "timer.h"
#if defined(HAVE_MTL) && __has_include(<mtl_api.h>)
#include <mtl_api.h>
#define RK3588_COMMON_HAVE_MTL 1
#endif
#include <stdlib.h>
#include <time.h>

struct monotonic_timer {
  void* mtl_handle;
  uint64_t start_ns;
};

struct monotonic_timer* monotonic_timer_create(void* mtl_handle) {
  struct monotonic_timer* t = calloc(1, sizeof(*t));
  if (!t) return NULL;
  t->mtl_handle = mtl_handle;

  if (mtl_handle) {
#ifdef RK3588_COMMON_HAVE_MTL
    t->start_ns = mtl_ptp_read_time(mtl_handle);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->start_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
  } else {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->start_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
  }
  return t;
}

void monotonic_timer_free(struct monotonic_timer* t) {
  free(t);
}

uint64_t monotonic_timer_now_ns(struct monotonic_timer* t) {
  if (t && t->mtl_handle) {
#ifdef RK3588_COMMON_HAVE_MTL
    return mtl_ptp_read_time(t->mtl_handle);
#else
    (void)t;
#endif
  }

  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void monotonic_timer_wait_frame(struct monotonic_timer* t, float fps, uint64_t frame_count) {
  uint64_t target_ns = t->start_ns + (uint64_t)((double)frame_count / fps * 1e9);
  uint64_t now_ns    = monotonic_timer_now_ns(t);
  if (now_ns >= target_ns) return;

  struct timespec req = {
      .tv_sec  = (time_t)((target_ns - now_ns) / 1000000000ULL),
      .tv_nsec = (long)((target_ns - now_ns) % 1000000000ULL),
  };
  clock_nanosleep(CLOCK_MONOTONIC, 0, &req, NULL);
}
