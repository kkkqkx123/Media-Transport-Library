#define _GNU_SOURCE

#include "tx_pipeline_priv.h"

#include "ring_buffer.h"
#include "slab_pool.h"
#include "codec_plugin.h"
#include "../codec/mpp_encoder.h"
#include "../io/v4l2_capturer.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SUPERVISE_TICK_S 1
#define HANG_THRESHOLD_NS (3000000000ULL)
#define REBUILD_GRACE_NS (5000000000ULL)
#define MAX_CONSECUTIVE_REBUILDS 5

void tx_supervise_error_cb(void* u) {
    struct tx_pipeline* tp = (struct tx_pipeline*)u;
    atomic_store(&tp->flag_capture_error, true);
}

void tx_supervise_signal_cb(void* u) {
    struct tx_pipeline* tp = (struct tx_pipeline*)u;
    atomic_store(&tp->flag_signal_change, true);
}

int tx_supervise_rebuild(struct tx_pipeline* tp) {
    printf("tx_pipe: rebuilding capture+encoder (streak=%u)\n",
           tp->consecutive_rebuilds + 1);

    if (tp->ring_cap2enc) ring_wake_all(tp->ring_cap2enc);
    if (tp->capture) v4l2_capturer_stop(tp->capture);
    if (tp->thd_encode) {
        pthread_join(tp->thd_encode, NULL);
        tp->thd_encode = 0;
    }
    if (tp->encoder) tp->encoder->stop(tp->encoder->priv);
    tx_pipeline_drain_cap2enc(tp);

    if (tp->capture) {
        v4l2_capturer_free(tp->capture);
        tp->capture = NULL;
    }
    if (tp->encoder) {
        tp->encoder->free(tp->encoder->priv);
        free(tp->encoder);
        tp->encoder = NULL;
    }

    struct codec_config codec_cfg = {
        .format = tp->cfg.codec,
        .width = tp->cfg.width,
        .height = tp->cfg.height,
        .fps = tp->cfg.fps,
        .bitrate_bps = tp->cfg.bitrate_bps,
        .gop_size = tp->cfg.gop_size,
        .b_frames = 0,
        .low_latency = true,
        .num_slices = tp->cfg.num_slices,
        .intra_refresh_mode = tp->cfg.intra_refresh_mode,
        .intra_refresh_arg = tp->cfg.intra_refresh_arg,
    };
    tp->encoder = mpp_encoder_create(&codec_cfg, tp->slab);
    if (!tp->encoder) {
        fprintf(stderr, "tx_pipe: rebuild encoder create failed\n");
        return -1;
    }
    if (tp->encoder->start(tp->encoder->priv) != 0) {
        fprintf(stderr, "tx_pipe: rebuild encoder start failed\n");
        return -1;
    }

    tp->capture = v4l2_capturer_create(
        tp->cfg.v4l2_device, tp->cfg.width, tp->cfg.height, tp->cfg.fps,
        tp->cfg.v4l2_buf_count, tx_encode_on_frame, tp);
    if (!tp->capture) {
        fprintf(stderr, "tx_pipe: rebuild capture create failed\n");
        return -1;
    }
    v4l2_capturer_set_error_cb(tp->capture, tx_supervise_error_cb, tp);
    v4l2_capturer_set_signal_change_cb(tp->capture, tx_supervise_signal_cb,
                                       tp);
    if (v4l2_capturer_start(tp->capture) != 0) {
        fprintf(stderr, "tx_pipe: rebuild capture start failed\n");
        return -1;
    }

    if (!atomic_load(&tp->running)) return -1;
    if (pthread_create(&tp->thd_encode, NULL, tx_encode_thread, tp) != 0) {
        tp->thd_encode = 0;
        return -1;
    }
    cpu_set_t cset;
    CPU_ZERO(&cset);
    CPU_SET(tp->cfg.cpu_encode, &cset);
    pthread_setaffinity_np(tp->thd_encode, sizeof(cset), &cset);

    tp->rebuild_done_ns = tx_pipeline_now_ns();
    tp->last_capture_progress_ns = tp->rebuild_done_ns;
    printf("tx_pipe: rebuild done\n");
    return 0;
}

void* tx_supervise_thread(void* arg) {
    struct tx_pipeline* tp = (struct tx_pipeline*)arg;
    struct tx_tick_state tick;
    memset(&tick, 0, sizeof(tick));

    while (atomic_load(&tp->running) && !tp->disabled) {
        sleep(SUPERVISE_TICK_S);
        if (!atomic_load(&tp->running) || tp->disabled) break;

        uint64_t now = tx_pipeline_now_ns();
        uint64_t captured_now = tp->stat_captured;
        bool got_error = atomic_exchange(&tp->flag_capture_error, false);
        bool got_signal = atomic_exchange(&tp->flag_signal_change, false);
        bool trigger = got_error || got_signal;
        if (!tp->capture || !tp->encoder) trigger = true;

        bool in_grace = (now - tp->rebuild_done_ns < REBUILD_GRACE_NS) ||
                        (tp->stat_encoded < 5);
        bool cap_hang = false;
        if (!trigger && !in_grace) {
            if (captured_now > atomic_load(&tp->last_captured_seen)) {
                tp->last_capture_progress_ns = now;
            } else if (now - tp->last_capture_progress_ns > HANG_THRESHOLD_NS) {
                fprintf(stderr,
                        "tx_pipe: capture hang detected "
                        "(no frames for >%lldms), rebuilding\n",
                        (long long)(HANG_THRESHOLD_NS / 1000000));
                trigger = true;
                cap_hang = true;
            }
        } else if (captured_now > atomic_load(&tp->last_captured_seen)) {
            tp->last_capture_progress_ns = now;
        }
        bool enc_hang = false;
        if (!trigger && !cap_hang && !in_grace &&
            captured_now > atomic_load(&tp->last_captured_seen)) {
            uint64_t last_out = 0;
            bool have_out =
                tp->encoder && tp->encoder->get_last_output_ns &&
                tp->encoder->get_last_output_ns(tp->encoder->priv, &last_out) ==
                    0;
            if (have_out && now - last_out > HANG_THRESHOLD_NS) {
                fprintf(stderr,
                        "tx_pipe: encoder hang detected "
                        "(no output for >%lldms), rebuilding\n",
                        (long long)(HANG_THRESHOLD_NS / 1000000));
                trigger = true;
                enc_hang = true;
            }
        }
        atomic_store(&tp->last_captured_seen, captured_now);

        tx_stats_log_1s(tp, &tick);

        if (!trigger) {
            tp->consecutive_rebuilds = 0;
            continue;
        }

        if (got_error) tp->rebuild_errors++;
        if (got_signal) tp->rebuild_signals++;
        if (cap_hang) tp->rebuild_cap_hang++;
        if (enc_hang) tp->rebuild_enc_hang++;
        tp->consecutive_rebuilds++;
        if (tp->consecutive_rebuilds >= MAX_CONSECUTIVE_REBUILDS) {
            tp->disabled = true;
            fprintf(stderr,
                    "tx_pipe: %u consecutive rebuild triggers — "
                    "pipeline disabled\n",
                    tp->consecutive_rebuilds);
            break;
        }
        if (tx_supervise_rebuild(tp) != 0) {
            fprintf(stderr, "tx_pipe: rebuild failed (streak=%u)\n",
                    tp->consecutive_rebuilds);
        }
    }
    return NULL;
}
