#include "ptp_time.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#include <sys/timex.h>
#endif

static int kernel_tai_offset_raw(void) {
#ifdef __linux__
    struct timex tx;
    memset(&tx, 0, sizeof(tx));
    if (adjtimex(&tx) < 0)
        return 0;
    return tx.tai;
#else
    return 0;
#endif
}

int mp4_ptp_tai_offset_sec(void) {
    int tai = kernel_tai_offset_raw();
    return tai > 0 ? tai : 37;
}

void mp4_ptp_time_ctx_init(mp4_ptp_time_ctx* ctx, int utc_offset_sec) {
    if (!ctx) return;
    ctx->utc_offset_sec = utc_offset_sec;
    atomic_store(&ctx->corr_offset_ns, 0);
    atomic_store(&ctx->drift_ppb, 0);
    atomic_store(&ctx->locked, false);
    atomic_store(&ctx->holdover, false);
    atomic_store(&ctx->corr_seq, 0);
}

uint64_t mp4_ptp_read_tai_ns(const mp4_ptp_time_ctx* ctx) {
    struct timespec ts;
    int off = ctx ? ctx->utc_offset_sec : 0;
    int64_t corr = ctx ? atomic_load(&ctx->corr_offset_ns) : 0;
    uint64_t raw;
    int tai_ready = (kernel_tai_offset_raw() > 0) && (clock_gettime(CLOCK_TAI, &ts) == 0);

    if (tai_ready) {
        if (off)
            ts.tv_sec -= off;
        raw = (uint64_t)ts.tv_sec * MP4_NS_PER_S + (uint64_t)ts.tv_nsec;
    } else if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        ts.tv_sec += (time_t)mp4_ptp_tai_offset_sec();
        if (off)
            ts.tv_sec -= off;
        raw = (uint64_t)ts.tv_sec * MP4_NS_PER_S + (uint64_t)ts.tv_nsec;
    } else {
        return 0;
    }
    return (uint64_t)((int64_t)raw + corr);
}

void mp4_ptp_apply_correction(mp4_ptp_time_ctx* ctx, int64_t offset_ns,
                               int64_t drift_ppb, bool locked, bool holdover) {
    if (!ctx) return;
    atomic_store(&ctx->corr_offset_ns, -offset_ns);
    atomic_store(&ctx->drift_ppb, drift_ppb);
    atomic_store(&ctx->locked, locked);
    atomic_store(&ctx->holdover, holdover);
    atomic_fetch_add(&ctx->corr_seq, 1);
}

bool mp4_ptp_time_locked(const mp4_ptp_time_ctx* ctx) {
    if (!ctx) return false;
    return atomic_load(&ctx->locked);
}

uint64_t mp4_ptp_mtl_time_fn(void* priv) {
    return mp4_ptp_read_tai_ns((const mp4_ptp_time_ctx*)priv);
}

int mp4_ptp_log_status(const mp4_ptp_time_ctx* ctx, const char* tag) {
    struct timespec tai_ts, rt_ts;
    int tai_ok = (clock_gettime(CLOCK_TAI, &tai_ts) == 0);
    int rt_ok  = (clock_gettime(CLOCK_REALTIME, &rt_ts) == 0);
    int koff   = kernel_tai_offset_raw();
    uint64_t ns = mp4_ptp_read_tai_ns(ctx);

    int64_t corr = ctx ? atomic_load(&ctx->corr_offset_ns) : 0;
    bool locked = ctx ? atomic_load(&ctx->locked) : false;
    bool holdover = ctx ? atomic_load(&ctx->holdover) : false;
    printf("PTP[%s]: tai_clock=%s kernel_tai_offset=%d utc_offset=%d now=%llu ns"
           " corr=%lld ns %s%s",
           tag ? tag : "?",
           tai_ok ? "ok" : "fallback",
           koff,
           ctx ? ctx->utc_offset_sec : 0,
           (unsigned long long)ns,
           (long long)corr,
           locked ? " locked" : " free",
           holdover ? "+holdover" : "");

    if (rt_ok)
        printf(" realtime=%lld.%09ld",
               (long long)rt_ts.tv_sec, rt_ts.tv_nsec);
    printf("\n");

    if (!tai_ok && koff <= 0) {
        fprintf(stderr,
                "PTP[%s]: WARN — CLOCK_TAI unavailable and kernel TAI offset unset.\n"
                "  Run: sudo tools/set_tai_offset/build/set_tai_offset\n"
                "  and ensure the PTP master is reachable (one-way slave recovers on its own).\n",
                tag ? tag : "?");
        return -1;
    }
    return 0;
}
