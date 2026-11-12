#ifndef _MP4_PTP_TIME_H_
#define _MP4_PTP_TIME_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define MP4_NS_PER_S 1000000000ULL

/** Runtime PTP settings (shared by MTL ptp_get_time_fn). */
typedef struct mp4_ptp_time_ctx {
    int utc_offset_sec; /* subtract from CLOCK_TAI when GM reports UTC (RxTxApp compat) */
    /* Virtual TAI correction from the one-way PTP slave (lock-free). */
    atomic_int_least64_t corr_offset_ns; /* added to raw TAI on every read */
    atomic_int_least64_t drift_ppb;      /* frequency estimate, info only */
    atomic_bool locked;                  /* servo converged on master */
    atomic_bool holdover;                /* master lost, extrapolating */
    atomic_uint_least64_t corr_seq;      /* increments per slave update */
} mp4_ptp_time_ctx;

/** Initialize context (utc_offset_sec usually 0 after set_tai_offset). */
void mp4_ptp_time_ctx_init(mp4_ptp_time_ctx* ctx, int utc_offset_sec);

/** Kernel TAI offset in seconds (adjtimex tai, 37 fallback). */
int mp4_ptp_tai_offset_sec(void);

/** Read TAI nanoseconds (CLOCK_TAI, fallback REALTIME + kernel TAI offset). */
uint64_t mp4_ptp_read_tai_ns(const mp4_ptp_time_ctx* ctx);

/** Slave actuator: publish filtered offset (ns), drift (ppb), lock state. */
void mp4_ptp_apply_correction(mp4_ptp_time_ctx* ctx, int64_t offset_ns,
                               int64_t drift_ppb, bool locked, bool holdover);

/** True once the slave servo has converged on the master. */
bool mp4_ptp_time_locked(const mp4_ptp_time_ctx* ctx);

/** MTL ptp_get_time_fn callback — pass ctx as priv. */
uint64_t mp4_ptp_mtl_time_fn(void* priv);

/** Log current TAI and whether CLOCK_TAI is usable. Returns 0 if TAI ok. */
int mp4_ptp_log_status(const mp4_ptp_time_ctx* ctx, const char* tag);

#endif /* _MP4_PTP_TIME_H_ */
