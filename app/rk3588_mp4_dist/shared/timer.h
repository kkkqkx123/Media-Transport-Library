/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * High-resolution timer for latency measurement.
 * Phase 1-4: CLOCK_MONOTONIC fallback (no MTL dependency).
 * Phase 5: wraps MTL's mtl_ptp_read_time() when MTL handle is available.
 */

#ifndef _E2E_TIMER_H_
#define _E2E_TIMER_H_

#include <stdint.h>

/** Opaque timer context */
struct e2e_timer;

/**
 * Create timer. If mtl_handle is NULL, falls back to CLOCK_MONOTONIC.
 * @param mtl_handle  MTL instance handle (Phase 5), NULL for Phase 1-4
 */
struct e2e_timer* e2e_timer_create(void* mtl_handle);
void e2e_timer_free(struct e2e_timer* t);

/** Get current timestamp in nanoseconds (PTP or MONOTONIC) */
uint64_t e2e_timer_now_ns(struct e2e_timer* t);

/**
 * Sleep until the next frame boundary (approximate, for framerate control).
 * @param t    timer
 * @param fps  target framerate
 * @param frame_count  frames processed so far
 */
void e2e_timer_wait_frame(struct e2e_timer* t, float fps, uint64_t frame_count);

#endif /* _E2E_TIMER_H_ */
