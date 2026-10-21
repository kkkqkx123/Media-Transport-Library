/* SPDX-License-Identifier: BSD-3-Clause
 *
 * PTP/TAI time helpers for mp4_dist (linuxptp + MTL external time source).
 */

#ifndef _MP4_PTP_TIME_H_
#define _MP4_PTP_TIME_H_

#include <stdint.h>

#define MP4_NS_PER_S 1000000000ULL

/** Runtime PTP settings (shared by MTL ptp_get_time_fn). */
typedef struct mp4_ptp_time_ctx {
    int utc_offset_sec; /* subtract from CLOCK_TAI when GM reports UTC (RxTxApp compat) */
} mp4_ptp_time_ctx;

/** Initialize context (utc_offset_sec usually 0 after set_tai_offset). */
void mp4_ptp_time_ctx_init(mp4_ptp_time_ctx* ctx, int utc_offset_sec);

/** Read TAI nanoseconds (CLOCK_TAI, fallback REALTIME + kernel TAI offset). */
uint64_t mp4_ptp_read_tai_ns(const mp4_ptp_time_ctx* ctx);

/** MTL ptp_get_time_fn callback — pass ctx as priv. */
uint64_t mp4_ptp_mtl_time_fn(void* priv);

/** Log current TAI and whether CLOCK_TAI is usable. Returns 0 if TAI ok. */
int mp4_ptp_log_status(const mp4_ptp_time_ctx* ctx, const char* tag);

#endif /* _MP4_PTP_TIME_H_ */
