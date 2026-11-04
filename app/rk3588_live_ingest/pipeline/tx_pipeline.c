#define _GNU_SOURCE
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * TX Pipeline implementation — see tx_pipeline.h for architecture.
 *
 * Thread model (Phase 4):
 *   Core cpu_capture: V4L2 DQBUF → ring_push(CAP2ENC)
 *   Core cpu_encode:  ring_pop(CAP2ENC) → RGA → MPP enc → SEI → ring_push(ENC2DEC)
 *
 * The ENC2DEC ring is consumed by rx_pipeline (Phase 4) or MTL ST22 (Phase 5).
 */

#include "tx_pipeline.h"
#include "ring_buffer.h"
#include "slab_pool.h"
#include "../hal/sei_timestamp.h"
#include "../codec/rga_converter.h"
#include "../codec/mpp_encoder.h"
#include "../codec/sps_vui.h"
#include "../io/v4l2_capturer.h"
#include <linux/videodev2.h>
#include <mtl_api.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct tx_pipeline {
    struct tx_pipeline_params cfg;

    /* resources */
    struct v4l2_capturer*  capture;
    struct rga_converter*  rga;
    struct codec_plugin*   encoder;
    struct slab_pool*      slab;
    struct ring_buffer*    ring_cap2enc;  /* dmabuf_frame */
    struct ring_buffer*    ring_enc2dec;  /* codec_packet — output */

    /* threads */
    pthread_t              thd_encode;
    pthread_t              thd_supervise;
    atomic_bool            running;

    /* supervision state (owned by the supervisor thread) */
    atomic_bool            flag_capture_error;
    atomic_bool            flag_signal_change;
    atomic_ullong          last_captured_seen; /* hang-guard: capture alive */
    uint64_t               last_capture_progress_ns;
    uint64_t               rebuild_done_ns;    /* grace window after rebuild */
    uint32_t               consecutive_rebuilds;
    uint64_t               rebuild_errors, rebuild_signals;
    uint64_t               rebuild_cap_hang, rebuild_enc_hang;
    bool                   disabled;           /* anti-storm tripped */

    /* stats: ring_drop is CAP2ENC full only; sink_drop is the total of the
     * split counters below (kept for log compatibility) */
    uint64_t               stat_captured;
    uint64_t               stat_encoded;
    uint64_t               stat_encoded_bytes;
    uint32_t               stat_ring_drops;
    uint32_t               stat_sink_drops;
    uint64_t               stat_send_fails;
    uint64_t               stat_recv_timeouts;
    uint64_t               stat_payload_oversize;
    uint64_t               stat_invalid_frames;
    uint64_t               stat_mtl_acquire_fails;
    uint64_t               stat_mtl_publish_fails;
    uint64_t               stat_enc2dec_full;
};

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ---- capture supervision callbacks (run in capture thread — post only) ---- */

static void capture_error_cb(void* u) {
    struct tx_pipeline* tp = (struct tx_pipeline*)u;
    atomic_store(&tp->flag_capture_error, true);
}

static void signal_change_cb(void* u) {
    struct tx_pipeline* tp = (struct tx_pipeline*)u;
    atomic_store(&tp->flag_signal_change, true);
}

/* ---- supervisor: anti-storm rebuild + encoder hang guard ---- */

#define SUPERVISE_TICK_S 1
#define HANG_THRESHOLD_NS (3000000000ULL)  /* 3s without encoder output */
#define REBUILD_GRACE_NS  (5000000000ULL)  /* startup window after rebuild */
#define MAX_CONSECUTIVE_REBUILDS 5

static void tx_pipeline_drain_cap2enc(struct tx_pipeline* tp);
static void capture_on_frame(void* u, const struct v4l2_frame* vf);
static void* encode_thread(void* arg);
static size_t tx_vui_fixup(struct tx_pipeline* tp, uint8_t* data, size_t size,
                           int is_key);

/* Recreate capture + encoder (RGA/slab/rings kept). Must be called from the
 * supervisor thread only — encode thread is joined/restarted here.
 * running stays owned by start/stop; rebuild must not resurrect it. */
static int tx_pipeline_rebuild(struct tx_pipeline* tp) {
    printf("tx_pipe: rebuilding capture+encoder (streak=%u)\n",
           tp->consecutive_rebuilds + 1);

    /* stop the data path, keep the MTL sink untouched */
    if (tp->ring_cap2enc) ring_wake_all(tp->ring_cap2enc);
    if (tp->capture) v4l2_capturer_stop(tp->capture);
    if (tp->thd_encode) { pthread_join(tp->thd_encode, NULL); tp->thd_encode = 0; }
    if (tp->encoder) tp->encoder->stop(tp->encoder->priv);
    tx_pipeline_drain_cap2enc(tp);

    /* free the components being replaced */
    if (tp->capture) { v4l2_capturer_free(tp->capture); tp->capture = NULL; }
    if (tp->encoder) {
        tp->encoder->free(tp->encoder->priv);
        free(tp->encoder);
        tp->encoder = NULL;
    }

    /* encoder */
    struct codec_config codec_cfg = {
        .format       = CODEC_FMT_H264,
        .width        = tp->cfg.width, .height = tp->cfg.height,
        .fps          = tp->cfg.fps,
        .bitrate_bps  = tp->cfg.bitrate_bps,
        .gop_size     = tp->cfg.gop_size,
        .b_frames     = 0,
        .low_latency  = true,
        .num_slices   = tp->cfg.num_slices,
        .intra_refresh_mode = tp->cfg.intra_refresh_mode,
        .intra_refresh_arg  = tp->cfg.intra_refresh_arg,
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

    /* capture */
    tp->capture = v4l2_capturer_create(tp->cfg.v4l2_device, tp->cfg.width,
                                       tp->cfg.height, tp->cfg.fps,
                                       tp->cfg.v4l2_buf_count,
                                       capture_on_frame, tp);
    if (!tp->capture) {
        fprintf(stderr, "tx_pipe: rebuild capture create failed\n");
        return -1;
    }
    v4l2_capturer_set_error_cb(tp->capture, capture_error_cb, tp);
    v4l2_capturer_set_signal_change_cb(tp->capture, signal_change_cb, tp);
    if (v4l2_capturer_start(tp->capture) != 0) {
        fprintf(stderr, "tx_pipe: rebuild capture start failed\n");
        return -1;
    }

    if (!atomic_load(&tp->running)) return -1;
    if (pthread_create(&tp->thd_encode, NULL, encode_thread, tp) != 0) {
        tp->thd_encode = 0;
        return -1;
    }
    cpu_set_t cset;
    CPU_ZERO(&cset);
    CPU_SET(tp->cfg.cpu_encode, &cset);
    pthread_setaffinity_np(tp->thd_encode, sizeof(cset), &cset);

    tp->rebuild_done_ns = now_ns();
    tp->last_capture_progress_ns = tp->rebuild_done_ns;
    printf("tx_pipe: rebuild done\n");
    return 0;
}

static void* supervise_thread(void* arg) {
    struct tx_pipeline* tp = (struct tx_pipeline*)arg;

    /* per-tick stats deltas for the 1s aggregation log */
    uint64_t prev_captured = 0, prev_encoded = 0, prev_bytes = 0;
    uint64_t prev_rga_calls = 0, prev_rga_fails = 0, prev_exhausted = 0;
    uint64_t prev_cpu_fb = 0, prev_full_drops = 0, prev_packets = 0;
    uint64_t prev_send = 0, prev_recv = 0, prev_oversize = 0, prev_invalid = 0;
    uint64_t prev_acq = 0, prev_pub = 0, prev_e2e = 0;

    while (atomic_load(&tp->running) && !tp->disabled) {
        sleep(SUPERVISE_TICK_S);
        if (!atomic_load(&tp->running) || tp->disabled) break;

        uint64_t now = now_ns();
        uint64_t captured_now = tp->stat_captured;
        bool got_error = atomic_exchange(&tp->flag_capture_error, false);
        bool got_signal = atomic_exchange(&tp->flag_signal_change, false);
        bool trigger = got_error || got_signal;
        const char* reason = got_error ? "capture-error"
                             : got_signal ? "signal-change" : "";
        if (!tp->capture || !tp->encoder) {
            trigger = true;
            reason = "missing-component";
        }

        bool in_grace = (now - tp->rebuild_done_ns < REBUILD_GRACE_NS) ||
                        (tp->stat_encoded < 5);
        /* capture arm: no new frames dequeued for a full hang window */
        bool cap_hang = false;
        if (!trigger && !in_grace) {
            if (captured_now > atomic_load(&tp->last_captured_seen)) {
                tp->last_capture_progress_ns = now;
            } else if (now - tp->last_capture_progress_ns > HANG_THRESHOLD_NS) {
                fprintf(stderr, "tx_pipe: capture hang detected "
                        "(no frames for >%lldms), rebuilding\n",
                        (long long)(HANG_THRESHOLD_NS / 1000000));
                trigger = true;
                reason = "capture-hang";
                cap_hang = true;
            }
        } else if (captured_now > atomic_load(&tp->last_captured_seen)) {
            tp->last_capture_progress_ns = now;
        }
        /* encode arm: capture progresses but no encoder output */
        bool enc_hang = false;
        if (!trigger && !cap_hang && !in_grace &&
            captured_now > atomic_load(&tp->last_captured_seen)) {
            uint64_t last_out = 0;
            bool have_out = tp->encoder && tp->encoder->get_last_output_ns &&
                            tp->encoder->get_last_output_ns(tp->encoder->priv,
                                                            &last_out) == 0;
            if (have_out && now - last_out > HANG_THRESHOLD_NS) {
                fprintf(stderr, "tx_pipe: encoder hang detected "
                        "(no output for >%lldms), rebuilding\n",
                        (long long)(HANG_THRESHOLD_NS / 1000000));
                trigger = true;
                reason = "encode-hang";
                enc_hang = true;
            }
        }
        atomic_store(&tp->last_captured_seen, captured_now);

        {
            struct rga_stats rs;
            struct codec_encode_stats es;
            uint64_t packets_win = 0;
            memset(&es, 0, sizeof(es));
            es.avg_qp = es.start_qp = es.bps_rt = es.pskip = -1;
            es.sse = -1;
            rga_converter_stats(tp->rga, &rs);
            uint64_t full_drops = 0;
            if (tp->encoder && tp->encoder->get_full_drops)
                tp->encoder->get_full_drops(tp->encoder->priv, &full_drops);
            if (tp->encoder && tp->encoder->get_encode_stats)
                tp->encoder->get_encode_stats(tp->encoder->priv, &es);
            if (tp->encoder && tp->encoder->take_packets_in_window)
                tp->encoder->take_packets_in_window(tp->encoder->priv,
                                                     &packets_win);
            int rc_bitrate = 0, rc_gop = 0, rc_qp = 0, rc_pending = 0;
            if (tp->encoder && tp->encoder->get_rc_state)
                tp->encoder->get_rc_state(tp->encoder->priv, &rc_bitrate,
                                           &rc_gop, &rc_qp, &rc_pending);
            printf("tx_pipe[1s]: cap=%llu/s enc=%llu/s %llukbps "
                   "ring_drop=%u sink_drop=%u "
                   "(send=%llu recv=%llu oversize=%llu invalid=%llu "
                   "acq=%llu pub=%llu e2e_full=%llu) "
                   "cap_fps=%u cap_int=%lluus eff_fps=%d out=%d peak=%d full_ev=%llu "
                   "ts_clamp=%llu ts_fb=%llu enc_fps=%u enc_cost=%uus "
                   "lat=%u/%u/%uus qp=%d/%d bps=%d sse=%lld pskip=%d "
                   "mpp=%u/%uB win_pkts=%llu "
                   "enc_full=%llu streak=%u rc_pend=%d | "
                   "rga ok=%llu fail=%llu pool_full=%llu cpu_fb=%llu "
                   "cpu_fail=%llu use=%u/%u deg=%d ema=%lluus rb=%llu/%llu/%llu/%llu\n",
                   (unsigned long long)(captured_now - prev_captured),
                   (unsigned long long)(tp->stat_encoded - prev_encoded),
                   (unsigned long long)((tp->stat_encoded_bytes - prev_bytes) * 8 / 1000),
                   tp->stat_ring_drops,
                   tp->stat_sink_drops,
                   (unsigned long long)(tp->stat_send_fails - prev_send),
                   (unsigned long long)(tp->stat_recv_timeouts - prev_recv),
                   (unsigned long long)(tp->stat_payload_oversize - prev_oversize),
                   (unsigned long long)(tp->stat_invalid_frames - prev_invalid),
                   (unsigned long long)(tp->stat_mtl_acquire_fails - prev_acq),
                   (unsigned long long)(tp->stat_mtl_publish_fails - prev_pub),
                   (unsigned long long)(tp->stat_enc2dec_full - prev_e2e),
                   tp->capture ? v4l2_capturer_video_fps(tp->capture) : 0,
                   tp->capture ? (unsigned long long)v4l2_capturer_cap_interval_us(tp->capture) : 0ULL,
                   tp->capture ? v4l2_capturer_effective_fps(tp->capture) : 0,
                   tp->capture ? v4l2_capturer_outstanding(tp->capture) : -1,
                   tp->capture ? v4l2_capturer_peak_outstanding(tp->capture) : -1,
                   tp->capture ? (unsigned long long)v4l2_capturer_full_events(tp->capture) : 0ULL,
                   tp->capture ? (unsigned long long)v4l2_capturer_ts_clamp_count(tp->capture) : 0ULL,
                   tp->capture ? (unsigned long long)v4l2_capturer_ts_fallback_count(tp->capture) : 0ULL,
                   (unsigned)es.encode_fps,
                   (unsigned)es.encode_cost_us,
                   (unsigned)es.latency_p50_us, (unsigned)es.latency_p90_us,
                   (unsigned)es.latency_p99_us,
                   (int)es.avg_qp, (int)es.start_qp, (int)es.bps_rt,
                   (long long)es.sse, (int)es.pskip,
                   (unsigned)es.mpp_buf_now, (unsigned)es.mpp_buf_peak,
                   (unsigned long long)packets_win,
                   (unsigned long long)(full_drops - prev_full_drops),
                   tp->consecutive_rebuilds, rc_pending,
                   (unsigned long long)(rs.calls - prev_rga_calls),
                   (unsigned long long)(rs.fails - prev_rga_fails),
                   (unsigned long long)(rs.pool_exhausted - prev_exhausted),
                   (unsigned long long)(rs.cpu_fallbacks - prev_cpu_fb),
                   (unsigned long long)rs.cpu_fails,
                   (unsigned)rs.pool_in_use, (unsigned)rs.pool_size,
                   (int)rs.degraded,
                   (unsigned long long)rs.time_ema_us,
                   (unsigned long long)tp->rebuild_errors,
                   (unsigned long long)tp->rebuild_signals,
                   (unsigned long long)tp->rebuild_cap_hang,
                   (unsigned long long)tp->rebuild_enc_hang);
            prev_captured   = captured_now;
            prev_encoded    = tp->stat_encoded;
            prev_bytes      = tp->stat_encoded_bytes;
            prev_rga_calls  = rs.calls;
            prev_rga_fails  = rs.fails;
            prev_exhausted  = rs.pool_exhausted;
            prev_cpu_fb     = rs.cpu_fallbacks;
            prev_full_drops = full_drops;
            prev_packets    = packets_win;
            prev_send       = tp->stat_send_fails;
            prev_recv       = tp->stat_recv_timeouts;
            prev_oversize   = tp->stat_payload_oversize;
            prev_invalid    = tp->stat_invalid_frames;
            prev_acq        = tp->stat_mtl_acquire_fails;
            prev_pub        = tp->stat_mtl_publish_fails;
            prev_e2e        = tp->stat_enc2dec_full;
            (void)prev_packets;
            (void)reason;
        }

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
            fprintf(stderr, "tx_pipe: %u consecutive rebuild triggers — "
                    "pipeline disabled\n", tp->consecutive_rebuilds);
            break;
        }
        if (tx_pipeline_rebuild(tp) != 0) {
            fprintf(stderr, "tx_pipe: rebuild failed (streak=%u)\n",
                    tp->consecutive_rebuilds);
        }
    }
    return NULL;
}

/* VUI colour fix shared by both sink paths. Returns the (possibly grown)
 * packet size held in data. */
static size_t tx_vui_fixup(struct tx_pipeline* tp, uint8_t* data, size_t size,
                           int is_key) {
    if (!is_key) return size;
    uint8_t* inj = slab_alloc(tp->slab);
    if (!inj) return size;
    int ir = sps_vui_inject(data, size, inj, slab_blk_size(tp->slab));
    if (ir > 0) {
        memcpy(data, inj, (size_t)ir);
        size = (size_t)ir;
    }
    slab_free(tp->slab, inj);
    return size;
}

/* ---- V4L2 capture callback: runs in capture thread ---- */
static void capture_on_frame(void* u, const struct v4l2_frame* vf) {
    struct tx_pipeline* tp = (struct tx_pipeline*)u;

    /* time_now must be non-blocking (lock-free PTP read); kernel ts is the
     * fallback so the capture thread never stalls on the clock source. */
    uint64_t cap_ts = tp->cfg.time_now ? tp->cfg.time_now(tp->cfg.time_priv) : 0;
    if (!cap_ts)
        cap_ts = vf->timestamp_ns;

    struct dmabuf_frame f = {
        .fd        = vf->dma_fd,
        .fd_uv     = vf->dma_fd_uv,
        .fourcc    = v4l2_capturer_fourcc(tp->capture),
        .width     = tp->cfg.width,
        .height    = tp->cfg.height,
        .num_planes = v4l2_capturer_num_planes(tp->capture),
        .stride    = vf->stride,
        .timestamp = cap_ts,
        .frame_seq = vf->frame_seq,
        .capture   = tp->capture,
        .buf_index = vf->buf_index,
        .generation = vf->generation,
    };

    /* deep copy the struct onto the ring (not the stack addr!) */
    struct dmabuf_frame* item = malloc(sizeof(*item));
    if (!item) {
        v4l2_capturer_release(tp->capture, vf->buf_index, vf->generation);
        return;
    }
    memcpy(item, &f, sizeof(f));

    if (ring_push(tp->ring_cap2enc, item) == 0) {
        tp->stat_captured++;
    } else {
        tp->stat_ring_drops++;
        v4l2_capturer_release(tp->capture, vf->buf_index, vf->generation);
        free(item); /* ring full — drop frame */
    }
}

/* ---- Encode thread: pops CAP2ENC, RGA+encode+publish ---- */
static void* encode_thread(void* arg) {
    struct tx_pipeline* tp = (struct tx_pipeline*)arg;
    while (atomic_load(&tp->running)) {
        struct dmabuf_frame* f = NULL;
        if (ring_pop_wait(tp->ring_cap2enc, (void**)&f, &tp->running) != 0)
            continue;
        if (!f) continue;

        uint32_t min_stride = (uint32_t)tp->cfg.width;
        if (f->fourcc == V4L2_PIX_FMT_BGR24) min_stride = (uint32_t)tp->cfg.width * 3;
        else if (f->fourcc == V4L2_PIX_FMT_UYVY || f->fourcc == V4L2_PIX_FMT_YUYV)
            min_stride = (uint32_t)tp->cfg.width * 2;
        if (f->fd < 0 || f->stride < min_stride) {
            tp->stat_sink_drops++;
            tp->stat_invalid_frames++;
            if (f->capture)
                v4l2_capturer_release(f->capture, f->buf_index, f->generation);
            free(f);
            continue;
        }

        /* NV12 single-plane capture feeds MPP directly; anything else goes
         * through RGA into the shared NV12 pool. */
        int enc_fd = f->fd;
        uint32_t enc_stride = f->stride;
        int nv12_idx = -1;
        bool use_rga = !(f->fourcc == V4L2_PIX_FMT_NV12 && f->num_planes == 1);
        if (use_rga) {
            int nv12_fd; uint32_t nv12_stride;
            if (rga_convert(tp->rga, f->fd, f->stride, f->fourcc,
                            &nv12_fd, &nv12_stride, &nv12_idx) != 0) {
                tp->stat_sink_drops++;
                if (f->capture)
                    v4l2_capturer_release(f->capture, f->buf_index, f->generation);
                free(f);
                continue;
            }
            enc_fd = nv12_fd;
            enc_stride = nv12_stride;
        }

        if (tp->encoder->send(tp->encoder->priv, enc_fd, enc_stride) != 0) {
            if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
            tp->stat_sink_drops++;
            tp->stat_send_fails++;
            if (f->capture)
                v4l2_capturer_release(f->capture, f->buf_index, f->generation);
            free(f);
            continue;
        }

        /* Capture timestamp is the single source of truth (set once in the
         * capture callback); the encode thread must not re-sample a second
         * clock domain. */
        uint64_t cap_ts = f->timestamp;
        size_t   payload_cap = (size_t)tp->cfg.coded_blk_size - SEI_TS_NAL_SIZE;

        if (tp->cfg.mtl_sink) {
            /* Use proven enc_recv (blocking) + memcpy to MTL fb.
             * recv_into direct-write retry loop is fragile: MPP async
             * timing causes excessive waits → frame drops → mosaic.
             * The ~200KB memcpy overhead (<0.1ms) is negligible vs
             * 16.7ms frame period. */
            uint8_t* data; size_t size; int key;
            /* recv now pops the encoder's bounded output ring (collector
             * thread assembles frames); ring_pop waits up to 1ms internally,
             * so only a couple of retries are needed */
            int rr, retry = 3;
            while ((rr = tp->encoder->recv(tp->encoder->priv, &data, &size, &key)) == -EAGAIN
                   && retry-- > 0)
                usleep(500);
            if (rr != 1) {
                if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
                tp->stat_sink_drops++;
                tp->stat_recv_timeouts++;
                if (f->capture)
                    v4l2_capturer_release(f->capture, f->buf_index, f->generation);
                free(f);
                continue;
            }
            size = tx_vui_fixup(tp, data, size, key);
            uint16_t fb_idx;
            void*    fb_addr;
            if (tp->cfg.mtl_sink->acquire_fb(tp->cfg.mtl_sink->priv, &fb_idx,
                                             &fb_addr) == 0) {
                uint8_t* fb = (uint8_t*)fb_addr;
                sei_timestamp_build(fb, (uint32_t)f->frame_seq, cap_ts);
                if (size <= payload_cap) {
                    mtl_memcpy(fb + SEI_TS_NAL_SIZE, data, size);
                    size_t total = size + SEI_TS_NAL_SIZE;
                    if (tp->cfg.mtl_sink->publish_fb(tp->cfg.mtl_sink->priv,
                                                     fb_idx, total) == 0) {
                        tp->stat_encoded++;
                        tp->stat_encoded_bytes += total;
                    } else {
                        tp->cfg.mtl_sink->abort_fb(tp->cfg.mtl_sink->priv, fb_idx);
                        tp->stat_sink_drops++;
                        tp->stat_mtl_publish_fails++;
                    }
                } else {
                    tp->cfg.mtl_sink->abort_fb(tp->cfg.mtl_sink->priv, fb_idx);
                    tp->stat_sink_drops++;
                    tp->stat_payload_oversize++;
                }
            } else {
                tp->stat_sink_drops++;
                tp->stat_mtl_acquire_fails++;
            }
            slab_free(tp->slab, data);
        } else {
            uint8_t* base = slab_alloc(tp->slab);
            if (!base) {
                if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
                tp->stat_sink_drops++;
                tp->stat_mtl_acquire_fails++;
                if (f->capture)
                    v4l2_capturer_release(f->capture, f->buf_index, f->generation);
                free(f);
                continue;
            }
            sei_timestamp_build(base, (uint32_t)f->frame_seq, cap_ts);
            uint8_t* data; size_t sz; int key;
            int rr, retry = 3;
            while ((rr = tp->encoder->recv(tp->encoder->priv, &data, &sz, &key)) == -EAGAIN
                   && retry-- > 0)
                usleep(500);
            if (rr != 1) {
                if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
                tp->stat_sink_drops++;
                tp->stat_recv_timeouts++;
                if (f->capture)
                    v4l2_capturer_release(f->capture, f->buf_index, f->generation);
                slab_free(tp->slab, base);
                free(f);
                continue;
            }
            sz = tx_vui_fixup(tp, data, sz, key);
            size_t total = sz + SEI_TS_NAL_SIZE;
            if (sz <= payload_cap) {
                memcpy(base + SEI_TS_NAL_SIZE, data, sz);
            } else {
                total = sz + SEI_TS_NAL_SIZE;
                tp->stat_sink_drops++;
                tp->stat_payload_oversize++;
                slab_free(tp->slab, data);
                slab_free(tp->slab, base);
                if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
                if (f->capture)
                    v4l2_capturer_release(f->capture, f->buf_index, f->generation);
                free(f);
                continue;
            }
            slab_free(tp->slab, data);
            struct codec_packet* pkt = malloc(sizeof(*pkt));
            if (pkt) {
                pkt->data          = base;
                pkt->size          = total;
                pkt->cap_timestamp = cap_ts;
                pkt->frame_seq     = f->frame_seq;
                if (ring_push(tp->ring_enc2dec, pkt) == 0) {
                    tp->stat_encoded++;
                    tp->stat_encoded_bytes += total;
                } else {
                    tp->stat_sink_drops++;
                    tp->stat_enc2dec_full++;
                    slab_free(tp->slab, base);
                    free(pkt);
                }
            } else {
                tp->stat_sink_drops++;
                tp->stat_mtl_acquire_fails++;
                slab_free(tp->slab, base);
            }
        }

        if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
        if (f->capture)
            v4l2_capturer_release(f->capture, f->buf_index, f->generation);
        free(f);
    }
    return NULL;
}

/* ---- public API ---- */

struct tx_pipeline* tx_pipeline_create(struct tx_pipeline_params* p) {
    struct tx_pipeline* tp = calloc(1, sizeof(*tp));
    if (!tp) return NULL;
    tp->cfg = *p;

    /* ring buffers */
    tp->ring_cap2enc = ring_create(p->ring_size);
    tp->ring_enc2dec = ring_create(p->ring_size);
    if (!tp->ring_cap2enc || !tp->ring_enc2dec) goto fail;

    /* RGA converter */
    tp->rga = rga_converter_create(p->width, p->height, p->rga_pool_size);
    if (!tp->rga) { fprintf(stderr, "tx_pipe: RGA create failed\n"); goto fail; }

    /* slab pool */
    tp->slab = slab_pool_create(p->ring_size * 3, (size_t)p->coded_blk_size);
    if (!tp->slab) { fprintf(stderr, "tx_pipe: slab create failed\n"); goto fail; }

    /* encoder recv uses slab as short-lived staging (one copy to MTL fb in e2e mode) */
    struct codec_config codec_cfg = {
        .format       = CODEC_FMT_H264,
        .width        = p->width, .height = p->height,
        .fps          = p->fps,
        .bitrate_bps  = p->bitrate_bps,
        .gop_size     = p->gop_size,
        .b_frames     = 0,
        .low_latency  = true,
        .num_slices   = p->num_slices,
        .intra_refresh_mode = p->intra_refresh_mode,
        .intra_refresh_arg  = p->intra_refresh_arg,
    };
    /* create a temporary slab for the encoder (it requires one for malloc fallback) */
    tp->encoder = mpp_encoder_create(&codec_cfg, tp->slab);
    if (!tp->encoder) { fprintf(stderr, "tx_pipe: encoder create failed\n"); goto fail; }

    /* V4L2 capture: ENODATA means no signal, caller retries periodically */
    tp->capture = v4l2_capturer_create(p->v4l2_device, p->width, p->height,
                                       p->fps, p->v4l2_buf_count,
                                       capture_on_frame, tp);
    if (!tp->capture) {
        int saved = errno;
        fprintf(stderr, "tx_pipe: capture create failed (%s)\n",
                saved == ENODATA ? "no signal" : strerror(saved));
        errno = saved;
        goto fail_errno;
    }
    v4l2_capturer_set_error_cb(tp->capture, capture_error_cb, tp);
    v4l2_capturer_set_signal_change_cb(tp->capture, signal_change_cb, tp);
    atomic_init(&tp->flag_capture_error, false);
    atomic_init(&tp->flag_signal_change, false);
    atomic_init(&tp->last_captured_seen, 0);
    tp->last_capture_progress_ns = now_ns();
    tp->rebuild_done_ns = now_ns(); /* startup grace window */

    printf("tx_pipe: %dx%d@%.0ffps %dbps slices=%d ready\n",
           p->width, p->height, (double)p->fps, p->bitrate_bps, p->num_slices);
    return tp;

fail:
    tx_pipeline_free(tp);
    return NULL;

fail_errno: {
    int saved = errno;
    tx_pipeline_free(tp);
    errno = saved;
    return NULL;
}
}

void tx_pipeline_free(struct tx_pipeline* tp) {
    if (!tp) return;
    tx_pipeline_stop(tp);
    if (tp->capture)  v4l2_capturer_free(tp->capture);
    if (tp->encoder)  { tp->encoder->free(tp->encoder->priv); free(tp->encoder); }
    if (tp->rga)      rga_converter_free(tp->rga);
    /* ring items: dmabuf_frame* in CAP2ENC, codec_packet* in ENC2DEC.
     * Free only the wrapper structs; coded data is slab-allocated and
     * will be reclaimed by slab_pool_free below. */
    if (tp->ring_cap2enc) {
        struct dmabuf_frame* f;
        while (ring_pop(tp->ring_cap2enc, (void**)&f) == 0) {
            if (!f) continue;
            if (f->capture)
                v4l2_capturer_release(f->capture, f->buf_index, f->generation);
            free(f);
        }
        ring_free(tp->ring_cap2enc);
    }
    if (tp->ring_enc2dec) {
        struct codec_packet* pkt;
        while (ring_pop(tp->ring_enc2dec, (void**)&pkt) == 0) {
            if (pkt) {
                if (pkt->data && tp->slab) slab_free(tp->slab, pkt->data);
                free(pkt);
            }
        }
        ring_free(tp->ring_enc2dec);
    }
    if (tp->slab)     slab_pool_free(tp->slab);
    free(tp);
}

int tx_pipeline_start(struct tx_pipeline* tp) {
    if (!tp) return -1;
    atomic_store(&tp->running, true);
    /* start MPP encoder collector thread (async Feeder/Collector model) */
    if (tp->encoder->start(tp->encoder->priv) != 0) {
        atomic_store(&tp->running, false);
        return -1;
    }
    if (pthread_create(&tp->thd_encode, NULL, encode_thread, tp) != 0) {
        atomic_store(&tp->running, false);
        tp->encoder->stop(tp->encoder->priv);
        return -1;
    }
    /* pin encode thread */
    cpu_set_t cset;
    CPU_ZERO(&cset);
    CPU_SET(tp->cfg.cpu_encode, &cset);
    pthread_setaffinity_np(tp->thd_encode, sizeof(cset), &cset);

    if (v4l2_capturer_start(tp->capture) != 0) {
        atomic_store(&tp->running, false);
        pthread_join(tp->thd_encode, NULL);
        tp->encoder->stop(tp->encoder->priv);
        return -1;
    }
    /* supervisor: rebuild scheduling + encoder hang guard */
    if (pthread_create(&tp->thd_supervise, NULL, supervise_thread, tp) != 0) {
        tp->thd_supervise = 0; /* supervision is optional — continue without */
    }
    return 0;
}

static void tx_pipeline_drain_cap2enc(struct tx_pipeline* tp) {
    if (!tp || !tp->ring_cap2enc) return;
    struct dmabuf_frame* f;
    while (ring_pop(tp->ring_cap2enc, (void**)&f) == 0) {
        if (!f) continue;
        if (f->capture)
            v4l2_capturer_release(f->capture, f->buf_index, f->generation);
        free(f);
    }
}

void tx_pipeline_stop(struct tx_pipeline* tp) {
    if (!tp) return;
    if (!atomic_exchange(&tp->running, false)) return; /* already stopped */
    if (tp->ring_cap2enc) ring_wake_all(tp->ring_cap2enc);
    /* supervisor first: it must not trigger a rebuild during shutdown */
    if (tp->thd_supervise) {
        pthread_join(tp->thd_supervise, NULL);
        tp->thd_supervise = 0;
    }
    if (tp->capture) v4l2_capturer_stop(tp->capture);
    if (tp->thd_encode) { pthread_join(tp->thd_encode, NULL); tp->thd_encode = 0; }
    if (tp->encoder) tp->encoder->stop(tp->encoder->priv);
    tx_pipeline_drain_cap2enc(tp);
    printf("tx_pipe: %llu captured, %llu encoded, ring_drop=%u sink_drop=%u "
           "(send=%llu recv=%llu oversize=%llu invalid=%llu acq=%llu pub=%llu e2e=%llu) "
           "rb=%llu/%llu/%llu/%llu\n",
           (unsigned long long)tp->stat_captured,
           (unsigned long long)tp->stat_encoded,
           tp->stat_ring_drops, tp->stat_sink_drops,
           (unsigned long long)tp->stat_send_fails,
           (unsigned long long)tp->stat_recv_timeouts,
           (unsigned long long)tp->stat_payload_oversize,
           (unsigned long long)tp->stat_invalid_frames,
           (unsigned long long)tp->stat_mtl_acquire_fails,
           (unsigned long long)tp->stat_mtl_publish_fails,
           (unsigned long long)tp->stat_enc2dec_full,
           (unsigned long long)tp->rebuild_errors,
           (unsigned long long)tp->rebuild_signals,
           (unsigned long long)tp->rebuild_cap_hang,
           (unsigned long long)tp->rebuild_enc_hang);
}

struct ring_buffer* tx_pipeline_get_output_ring(struct tx_pipeline* tp) {
    return tp ? tp->ring_enc2dec : NULL;
}

struct slab_pool* tx_pipeline_get_slab_pool(struct tx_pipeline* tp) {
    return tp ? tp->slab : NULL;
}

int tx_pipeline_set_param(struct tx_pipeline* tp, enum codec_param k, int v) {
    if (!tp || !tp->encoder || !tp->encoder->set_param) return -EINVAL;
    return tp->encoder->set_param(tp->encoder->priv, k, v);
}

void tx_pipeline_stats(struct tx_pipeline* tp, struct tx_pipeline_stats* out) {
    if (!out) return;
    if (!tp) { memset(out, 0, sizeof(*out)); return; }
    out->captured         = tp->stat_captured;
    out->encoded          = tp->stat_encoded;
    out->encoded_bytes    = tp->stat_encoded_bytes;
    out->ring_drops       = tp->stat_ring_drops;
    out->sink_drops       = tp->stat_sink_drops;
    out->send_fails       = tp->stat_send_fails;
    out->recv_timeouts    = tp->stat_recv_timeouts;
    out->payload_oversize = tp->stat_payload_oversize;
    out->invalid_frames   = tp->stat_invalid_frames;
    out->mtl_acquire_fails = tp->stat_mtl_acquire_fails;
    out->mtl_publish_fails = tp->stat_mtl_publish_fails;
    out->enc2dec_full     = tp->stat_enc2dec_full;
    if (tp->capture) {
        struct v4l2_capturer_stats cs;
        v4l2_capturer_stats(tp->capture, &cs);
        out->cap_outstanding   = cs.outstanding;
        out->cap_peak          = cs.peak_outstanding;
        out->cap_buf_count     = cs.buf_count;
        out->cap_effective_fps = cs.effective_fps;
        out->cap_fps           = cs.video_fps;
        out->cap_full_events   = cs.full_events;
        out->cap_interval_us   = cs.cap_interval_us;
        out->cap_ts_clamp      = cs.ts_clamp_count;
        out->cap_ts_fallback   = cs.ts_fallback_count;
    }
    if (tp->rga) {
        struct rga_stats rs;
        rga_converter_stats(tp->rga, &rs);
        out->rga_calls          = rs.calls;
        out->rga_fails          = rs.fails;
        out->rga_pool_exhausted = rs.pool_exhausted;
        out->rga_cpu_fallbacks  = rs.cpu_fallbacks;
        out->rga_cpu_fails      = rs.cpu_fails;
        out->rga_ema_us         = rs.time_ema_us;
        out->rga_in_use         = rs.pool_in_use;
        out->rga_peak           = rs.pool_peak;
        out->rga_size           = rs.pool_size;
        out->rga_degraded       = rs.degraded;
    }
    if (tp->encoder && tp->encoder->get_encode_stats) {
        struct codec_encode_stats es;
        memset(&es, 0, sizeof(es));
        if (tp->encoder->get_encode_stats(tp->encoder->priv, &es) == 0) {
            out->enc_fps          = es.encode_fps;
            out->enc_cost_us      = es.encode_cost_us;
            out->enc_p50          = es.latency_p50_us;
            out->enc_p90          = es.latency_p90_us;
            out->enc_p99          = es.latency_p99_us;
            out->enc_avg_qp       = es.avg_qp;
            out->enc_start_qp     = es.start_qp;
            out->enc_bps_rt       = es.bps_rt;
            out->enc_sse          = es.sse;
            out->enc_pskip        = es.pskip;
            out->enc_mpp_now      = es.mpp_buf_now;
            out->enc_mpp_peak     = es.mpp_buf_peak;
            out->enc_packets_total = es.packets_total;
            out->enc_oversize     = es.oversize_drops;
            out->enc_meta_missing = es.meta_missing;
        }
        if (tp->encoder->get_full_drops) {
            uint64_t n = 0;
            if (tp->encoder->get_full_drops(tp->encoder->priv, &n) == 0)
                out->enc_ring_full = n;
        }
    }
    out->rebuild_streak   = tp->consecutive_rebuilds;
    out->rebuild_errors   = tp->rebuild_errors;
    out->rebuild_signals  = tp->rebuild_signals;
    out->rebuild_cap_hang = tp->rebuild_cap_hang;
    out->rebuild_enc_hang = tp->rebuild_enc_hang;
    out->disabled         = tp->disabled ? 1 : 0;
}

int tx_pipeline_format_metrics(struct tx_pipeline* tp, char* buf, size_t len) {
    struct tx_pipeline_stats s;
    tx_pipeline_stats(tp, &s);
    return snprintf(buf, len,
        "cap=%llu enc=%llu ring=%u sink=%u(send=%llu recv=%llu ovs=%llu inv=%llu "
        "acq=%llu pub=%llu e2e=%llu) cap_fps=%u cap_int=%lluus eff=%d out=%d/%d "
        "enc_fps=%u cost=%uus lat=%u/%u/%u qp=%d/%d bps=%d mpp=%u/%u rga=%llu/%llu/%llu/%llu use=%u/%u deg=%d rb=%llu/%llu/%llu/%llu%s",
        (unsigned long long)s.captured, (unsigned long long)s.encoded,
        s.ring_drops, s.sink_drops,
        (unsigned long long)s.send_fails, (unsigned long long)s.recv_timeouts,
        (unsigned long long)s.payload_oversize, (unsigned long long)s.invalid_frames,
        (unsigned long long)s.mtl_acquire_fails, (unsigned long long)s.mtl_publish_fails,
        (unsigned long long)s.enc2dec_full,
        s.cap_fps, (unsigned long long)s.cap_interval_us, s.cap_effective_fps,
        s.cap_outstanding, s.cap_buf_count,
        s.enc_fps, s.enc_cost_us, s.enc_p50, s.enc_p90, s.enc_p99,
        (int)s.enc_avg_qp, (int)s.enc_start_qp, (int)s.enc_bps_rt,
        s.enc_mpp_now, s.enc_mpp_peak,
        (unsigned long long)s.rga_calls, (unsigned long long)s.rga_fails,
        (unsigned long long)s.rga_pool_exhausted, (unsigned long long)s.rga_cpu_fallbacks,
        s.rga_in_use, s.rga_size, (int)s.rga_degraded,
        (unsigned long long)s.rebuild_errors, (unsigned long long)s.rebuild_signals,
        (unsigned long long)s.rebuild_cap_hang, (unsigned long long)s.rebuild_enc_hang,
        s.disabled ? " disabled" : "");
}
