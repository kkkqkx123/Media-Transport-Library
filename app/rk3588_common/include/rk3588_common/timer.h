#ifndef _RK3588_COMMON_TIMER_H_
#define _RK3588_COMMON_TIMER_H_

#include <stdint.h>

/** Opaque timer context */
struct monotonic_timer;

/**
 * Create timer. If mtl_handle is NULL, falls back to CLOCK_MONOTONIC.
 * @param mtl_handle  MTL instance handle (Phase 5), NULL for Phase 1-4
 */
struct monotonic_timer* monotonic_timer_create(void* mtl_handle);
void monotonic_timer_free(struct monotonic_timer* t);

/** Get current timestamp in nanoseconds (PTP or MONOTONIC) */
uint64_t monotonic_timer_now_ns(struct monotonic_timer* t);

/**
 * Sleep until the next frame boundary (approximate, for framerate control).
 * @param t    timer
 * @param fps  target framerate
 * @param frame_count  frames processed so far
 */
void monotonic_timer_wait_frame(struct monotonic_timer* t, float fps, uint64_t frame_count);

#endif /* _RK3588_COMMON_TIMER_H_ */
