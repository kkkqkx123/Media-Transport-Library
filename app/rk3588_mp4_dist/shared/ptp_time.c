/* SPDX-License-Identifier: BSD-3-Clause */

#include "ptp_time.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef __linux__
#include <sys/timex.h>
#endif

static int kernel_tai_offset_sec(void) {
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

void mp4_ptp_time_ctx_init(mp4_ptp_time_ctx* ctx, int utc_offset_sec) {
    if (!ctx) return;
    ctx->utc_offset_sec = utc_offset_sec;
}

uint64_t mp4_ptp_read_tai_ns(const mp4_ptp_time_ctx* ctx) {
    struct timespec ts;
    int off = ctx ? ctx->utc_offset_sec : 0;

    if (clock_gettime(CLOCK_TAI, &ts) == 0) {
        if (off)
            ts.tv_sec -= off;
        return (uint64_t)ts.tv_sec * MP4_NS_PER_S + (uint64_t)ts.tv_nsec;
    }

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        int tai_off = kernel_tai_offset_sec();
        if (tai_off <= 0)
            tai_off = 37;
        ts.tv_sec += (time_t)tai_off;
        if (off)
            ts.tv_sec -= off;
        return (uint64_t)ts.tv_sec * MP4_NS_PER_S + (uint64_t)ts.tv_nsec;
    }

    return 0;
}

uint64_t mp4_ptp_mtl_time_fn(void* priv) {
    return mp4_ptp_read_tai_ns((const mp4_ptp_time_ctx*)priv);
}

int mp4_ptp_log_status(const mp4_ptp_time_ctx* ctx, const char* tag) {
    struct timespec tai_ts, rt_ts;
    int tai_ok = (clock_gettime(CLOCK_TAI, &tai_ts) == 0);
    int rt_ok  = (clock_gettime(CLOCK_REALTIME, &rt_ts) == 0);
    int koff   = kernel_tai_offset_sec();
    uint64_t ns = mp4_ptp_read_tai_ns(ctx);

    printf("PTP[%s]: tai_clock=%s kernel_tai_offset=%d utc_offset=%d now=%llu ns",
           tag ? tag : "?",
           tai_ok ? "ok" : "fallback",
           koff,
           ctx ? ctx->utc_offset_sec : 0,
           (unsigned long long)ns);

    if (rt_ok)
        printf(" realtime=%lld.%09ld",
               (long long)rt_ts.tv_sec, rt_ts.tv_nsec);
    printf("\n");

    if (!tai_ok && koff <= 0) {
        fprintf(stderr,
                "PTP[%s]: WARN — CLOCK_TAI unavailable and kernel TAI offset unset.\n"
                "  Run: sudo tools/set_tai_offset/build/set_tai_offset\n"
                "  and ensure ptp4l + phc2sys are running before mp4_dist.\n",
                tag ? tag : "?");
        return -1;
    }
    return 0;
}
