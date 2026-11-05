#ifndef _LIVE_INGEST_TX_PIPELINE_PRIV_H_
#define _LIVE_INGEST_TX_PIPELINE_PRIV_H_

#include "tx_pipeline.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

struct v4l2_capturer;
struct rga_converter;
struct codec_plugin;
struct slab_pool;
struct ring_buffer;
struct v4l2_frame;

struct tx_pipeline {
    struct tx_pipeline_params cfg;

    struct v4l2_capturer* capture;
    struct rga_converter* rga;
    struct codec_plugin* encoder;
    struct slab_pool* slab;
    struct ring_buffer* ring_cap2enc;
    struct ring_buffer* ring_enc2dec;

    pthread_t thd_encode;
    pthread_t thd_supervise;
    atomic_bool running;

    atomic_bool flag_capture_error;
    atomic_bool flag_signal_change;
    atomic_ullong last_captured_seen;
    uint64_t last_capture_progress_ns;
    uint64_t rebuild_done_ns;
    uint32_t consecutive_rebuilds;
    uint64_t rebuild_errors, rebuild_signals;
    uint64_t rebuild_cap_hang, rebuild_enc_hang;
    bool disabled;

    uint64_t stat_captured;
    uint64_t stat_encoded;
    uint64_t stat_encoded_bytes;
    uint32_t stat_ring_drops;
    uint32_t stat_sink_drops;
    uint64_t stat_send_fails;
    uint64_t stat_recv_timeouts;
    uint64_t stat_payload_oversize;
    uint64_t stat_invalid_frames;
    uint64_t stat_mtl_acquire_fails;
    uint64_t stat_mtl_publish_fails;
    uint64_t stat_enc2dec_full;
};

struct tx_tick_state {
    uint64_t prev_captured, prev_encoded, prev_bytes;
    uint64_t prev_rga_calls, prev_rga_fails, prev_exhausted;
    uint64_t prev_cpu_fb, prev_full_drops;
    uint64_t prev_send, prev_recv, prev_oversize, prev_invalid;
    uint64_t prev_acq, prev_pub, prev_e2e;
};

static inline uint64_t tx_pipeline_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void tx_pipeline_drain_cap2enc(struct tx_pipeline* tp);

/* Data path: capture ingress plus RGA and encode and publish. */
void tx_encode_on_frame(void* u, const struct v4l2_frame* vf);
void* tx_encode_thread(void* arg);
size_t tx_encode_vui_fixup(struct tx_pipeline* tp, uint8_t* data, size_t size,
                           int is_key);
int tx_encode_recv(struct tx_pipeline* tp, uint8_t** data, size_t* size,
                   int* key);

/* Supervision: error signals plus hang guard plus rebuild. */
void tx_supervise_error_cb(void* u);
void tx_supervise_signal_cb(void* u);
int tx_supervise_rebuild(struct tx_pipeline* tp);
void* tx_supervise_thread(void* arg);

/* Observability: one second aggregation log driven by the supervisor. */
void tx_stats_log_1s(struct tx_pipeline* tp, struct tx_tick_state* tick);

#endif
