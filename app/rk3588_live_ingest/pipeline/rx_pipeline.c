#define _GNU_SOURCE
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "rx_pipeline.h"
#include "ring_buffer.h"
#include "rx_track.h"
#include "slice_reassembler.h"
#include "pipeline_common.h"
#include "../hal/sei_timestamp.h"
#include "slab_pool.h"
#include "../codec/mpp_decoder.h"
#include "../io/drm_renderer.h"
#include "../mtl_rx.h"
#include "../mtl_rx_bridge.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Supervision discipline mirrors tx_pipeline: hang threshold, startup grace
 * after (re)build, and an anti-storm trip that sticks the pipeline degraded
 * instead of rebuild-looping forever. */
#define RX_SUPERVISE_TICK_S  1
#define RX_HANG_THRESHOLD_NS (3000000000ULL)  /* 3s of input without output */
#define RX_REBUILD_GRACE_NS  (5000000000ULL)  /* startup window after rebuild */
#define RX_MAX_CONSECUTIVE_REBUILDS 5
#define RX_MAX_CONSEC_DECODE_ERRS   120 /* ~2s of hard put failures @60fps */
#define RX_MAX_CONSEC_RENDER_FAILS  30

/* Reassembly without a following picture must not stall a whole frame. */
#define RX_REASM_FLUSH_TIMEOUT_NS (100000000ULL) /* 100ms */
#define RX_LAT_WIN 256 /* E2E latency window for p50/p90/p99 */

/* Input-buffer retention for the MPP lifetime question. mpp_packet_set_data
 * hands MPP a caller-owned pointer and decode is asynchronous; if MPP only
 * references the buffer, freeing the slab right after put-drain would be a
 * use-after-free. Holding the last few inputs (freed FIFO) is harmless under
 * copy semantics and closes the UAF window under reference semantics. */
#define RX_HOLD_N 4

struct rx_pipeline {
    struct rx_pipeline_params cfg;
    int cur_width, cur_height; /* track info-change; renderer matches these */

    struct codec_plugin*       decoder;
    struct drm_renderer*       renderer;
    struct slice_reassembler*  reasm;

    pthread_t                  thd;
    pthread_t                  thd_supervise;
    atomic_bool                running;

    /* supervision state (supervise thread owns rebuild decisions) */
    uint64_t                   rebuild_done_ns;
    uint32_t                   consecutive_rebuilds;
    bool                       disabled;
    atomic_uint                consec_decode_errs;
    atomic_uint                consec_render_fails;

    /* retained input slabs (decode thread only, plus joined-thread drains) */
    uint8_t*                   hold_buf[RX_HOLD_N];
    int                        hold_head, hold_count;
    int                        hold_peak;

    struct rx_seq_tracker      seq;
    uint64_t                   last_reasm_feed_ns;
    uint64_t                   last_reasm_emit_ns;

    uint64_t                   stat_received;
    uint64_t                   stat_decoded;
    uint64_t                   stat_rendered;
    uint64_t                   stat_decode_errs;
    uint64_t                   stat_reasm_drops;
    uint64_t                   stat_reasm_flushes; /* timeout flushes */
    uint64_t                   stat_reasm_stop_flushes; /* stop-path flushes */
    uint64_t                   stat_render_fails;
    uint64_t                   stat_cap_ts_zero;
    uint64_t                   stat_lat_invalid; /* compat: back + over */
    uint64_t                   stat_lat_back;
    uint64_t                   stat_lat_over;
    uint64_t                   stat_lat_valid;
    uint64_t                   stat_sei_missing;
    uint64_t                   stat_empty_payload;
    uint64_t                   lat_win_min_ns, lat_win_max_ns;
    uint64_t                   lat_win_sum_ns, lat_win_count;
    uint64_t                   lat_cum_min_ns, lat_cum_max_ns;
    uint64_t                   lat_cum_sum_ns, lat_cum_count;
    uint64_t                   lat_win[RX_LAT_WIN];
    int                        lat_win_fill;
    atomic_ullong              last_render_ns; /* hang-guard heartbeat */
    /* supervision extras (supervise thread writes, snapshot reads) */
    uint32_t                   rx_fps, dec_fps, render_fps;
    uint64_t                   rebuild_decode_hang, rebuild_decode_errors;
    uint64_t                   rebuild_render_fails;
    uint64_t                   silence_events;
    uint64_t                   last_rebuild_us;
    uint64_t                   last_input_ns;
    bool                       silence_active;
    int                        disabled_reason; /* 0 = none, 1 = storm */
};

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void update_latency(struct rx_pipeline* rp, uint64_t ns) {
    if (ns < rp->lat_win_min_ns || rp->lat_win_count == 0)
        rp->lat_win_min_ns = ns;
    if (ns > rp->lat_win_max_ns) rp->lat_win_max_ns = ns;
    rp->lat_win_sum_ns += ns;
    rp->lat_win_count++;
    if (rp->lat_win_fill < RX_LAT_WIN)
        rp->lat_win[rp->lat_win_fill++] = ns;
    else {
        memmove(&rp->lat_win[0], &rp->lat_win[1],
                (size_t)(RX_LAT_WIN - 1) * sizeof(uint64_t));
        rp->lat_win[RX_LAT_WIN - 1] = ns;
    }
    rp->stat_lat_valid++;
    if (rp->lat_cum_min_ns == 0 || ns < rp->lat_cum_min_ns)
        rp->lat_cum_min_ns = ns;
    if (ns > rp->lat_cum_max_ns) rp->lat_cum_max_ns = ns;
    rp->lat_cum_sum_ns += ns;
    rp->lat_cum_count++;
}

static void lat_win_percentiles(struct rx_pipeline* rp, uint64_t* p50,
                                uint64_t* p90, uint64_t* p99) {
    uint64_t tmp[RX_LAT_WIN];
    int n = rp->lat_win_fill;
    if (n <= 0) { *p50 = *p90 = *p99 = 0; return; }
    memcpy(tmp, rp->lat_win, (size_t)n * sizeof(uint64_t));
    for (int i = 1; i < n; i++) {
        uint64_t v = tmp[i];
        int j = i;
        while (j > 0 && tmp[j - 1] > v) { tmp[j] = tmp[j - 1]; j--; }
        tmp[j] = v;
    }
    *p50 = tmp[(n * 50) / 100 >= n ? n - 1 : (n * 50) / 100];
    *p90 = tmp[(n * 90) / 100 >= n ? n - 1 : (n * 90) / 100];
    *p99 = tmp[(n * 99) / 100 >= n ? n - 1 : (n * 99) / 100];
}

/* Returns 0 on display, -1 on failure (caller counts + triggers). */
static int rx_render_frame(struct rx_pipeline* rp, int out_fd, uint32_t out_h_stride,
                           uint32_t out_v_stride, uint64_t cap_ts, int use_cap_ts) {
    rp->stat_decoded++;
    if (drm_renderer_display(rp->renderer, out_fd, out_h_stride, out_v_stride) != 0) {
        rp->stat_render_fails++;
        atomic_fetch_add(&rp->consec_render_fails, 1);
        return -1;
    }
    atomic_store(&rp->consec_render_fails, 0);
    rp->stat_rendered++;
    atomic_store(&rp->last_render_ns, now_ns());
    if (!use_cap_ts) return 0;
    uint64_t render_ts = rp->cfg.time_now ? rp->cfg.time_now(rp->cfg.time_priv)
                                          : now_ns();
    uint64_t delta = 0;
    switch (rx_lat_classify(cap_ts, render_ts, &delta)) {
    case RX_LAT_OK:
        update_latency(rp, delta);
        break;
    case RX_LAT_ZERO:
        rp->stat_cap_ts_zero++;
        break;
    case RX_LAT_BACK:
        rp->stat_lat_back++;
        rp->stat_lat_invalid++;
        break;
    case RX_LAT_OVER:
    default:
        rp->stat_lat_over++;
        rp->stat_lat_invalid++;
        break;
    }
    return 0;
}

#define RX_FEED_CHUNK  8192

static int rx_decode_and_render(struct rx_pipeline* rp, uint8_t* data, size_t size,
                                uint64_t cap_ts) {
    int out_fd;
    uint32_t out_h_stride, out_v_stride;
    int rendered = 0;
    int first = 1;
    size_t offset = 0;

    /* Feed in chunks and drain after each — MPP may output mid-stream. */
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > RX_FEED_CHUNK) chunk = RX_FEED_CHUNK;
        if (mpp_dec_put(rp->decoder->priv, data + offset, chunk) < 0)
            return rendered > 0 ? 1 : -1;
        offset += chunk;

        while (mpp_dec_pull(rp->decoder->priv, &out_fd, &out_h_stride, &out_v_stride) == 1) {
            rx_render_frame(rp, out_fd, out_h_stride, out_v_stride, cap_ts, first);
            rendered++;
            first = 0;
        }
    }
    return rendered > 0 ? 1 : 0;
}

static void rx_drain_decoder(struct rx_pipeline* rp) {
    int out_fd;
    uint32_t out_h_stride, out_v_stride;
    while (mpp_dec_pull(rp->decoder->priv, &out_fd, &out_h_stride, &out_v_stride) == 1)
        rx_render_frame(rp, out_fd, out_h_stride, out_v_stride, 0, 0);
}

/* Push a consumed slab block into the retention FIFO; the evicted oldest
 * block (if any) is what actually returns to the pool. */
static void rx_hold_input(struct rx_pipeline* rp, uint8_t* blk) {
    if (!blk) return;
    if (rp->hold_count < RX_HOLD_N) {
        rp->hold_buf[(rp->hold_head + rp->hold_count) % RX_HOLD_N] = blk;
        rp->hold_count++;
        if (rp->hold_count > rp->hold_peak)
            rp->hold_peak = rp->hold_count;
        return;
    }
    uint8_t* oldest = rp->hold_buf[rp->hold_head];
    rp->hold_buf[rp->hold_head] = blk;
    rp->hold_head = (rp->hold_head + 1) % RX_HOLD_N;
    if (rp->cfg.coded_pool && oldest) slab_free(rp->cfg.coded_pool, oldest);
}

static void rx_pipeline_drain_input(struct rx_pipeline* rp);

static void rx_hold_flush(struct rx_pipeline* rp) {
    for (int i = 0; i < rp->hold_count; i++) {
        uint8_t* blk = rp->hold_buf[(rp->hold_head + i) % RX_HOLD_N];
        if (blk && rp->cfg.coded_pool) slab_free(rp->cfg.coded_pool, blk);
        rp->hold_buf[(rp->hold_head + i) % RX_HOLD_N] = NULL;
    }
    rp->hold_head = 0;
    rp->hold_count = 0;
}

static void rx_release_mtl_owner(struct rx_pipeline* rp, struct codec_packet* pkt) {
    if (!pkt || !pkt->mtl_owner || !rp->cfg.rx_handle) return;
    struct rx_pkt* mp = (struct rx_pkt*)pkt->mtl_owner;
    if (mp->frame) {
        st22_rx_put_framebuff(rp->cfg.rx_handle, mp->frame);
        mp->frame = NULL;
    }
    atomic_store(&mp->used, 0);
    pkt->mtl_owner = NULL;
}

/* Renderer-only rebuild for resolution changes and display-failure streaks.
 * Runs either on the decode thread (renderer is its private resource then)
 * or on the supervise thread while the decode thread is joined. */
static int rx_rebuild_renderer(struct rx_pipeline* rp, int width, int height) {
    struct drm_renderer* nr = drm_renderer_create(
        rp->cfg.drm_device, rp->cfg.drm_connector, rp->cfg.drm_mode,
        width, height, DRM_FORMAT_NV12, rp->cfg.drm_buf_count);
    if (!nr) {
        fprintf(stderr, "rx_pipe: renderer rebuild %dx%d failed\n", width, height);
        return -1;
    }
    drm_renderer_free(rp->renderer);
    rp->renderer = nr;
    rp->cur_width = width;
    rp->cur_height = height;
    printf("rx_pipe: renderer rebuilt for %dx%d\n", width, height);
    return 0;
}

static void rx_decode_packet(struct rx_pipeline* rp, struct codec_packet* pkt) {
    uint32_t frame_seq = 0;
    uint64_t cap_ts = 0;
    int has_sei = sei_timestamp_peek(pkt->data, pkt->size, &frame_seq, &cap_ts);
    if (has_sei)
        rx_seq_observe(&rp->seq, frame_seq);
    else
        rp->stat_sei_missing++;

    /* SEI stays in the slab; the decoder starts at the payload offset. */
    size_t off = rx_sei_payload_offset(has_sei, SEI_TS_NAL_SIZE);
    uint8_t* payload = pkt->data + off;
    size_t payload_sz = (pkt->size > off) ? pkt->size - off : 0;
    if (payload_sz == 0) {
        rp->stat_empty_payload++;
        rp->stat_decode_errs++;
        atomic_fetch_add(&rp->consec_decode_errs, 1);
        return;
    }

    if (rp->cfg.num_slices > 1 && rp->reasm) {
        uint8_t* frame = NULL;
        size_t frame_sz = 0;
        int rr = slice_reasm_feed(rp->reasm, payload, payload_sz, &frame, &frame_sz);
        rp->last_reasm_feed_ns = now_ns();
        if (rr == 1 && frame) {
            int dr = rx_decode_and_render(rp, frame, frame_sz, cap_ts);
            slice_reasm_free_frame(rp->reasm, frame);
            rp->last_reasm_emit_ns = now_ns();
            if (dr < 0) {
                rp->stat_decode_errs++;
                atomic_fetch_add(&rp->consec_decode_errs, 1);
            } else {
                atomic_store(&rp->consec_decode_errs, 0);
            }
        } else if (rr < 0) {
            rp->stat_reasm_drops++;
        } else if (slice_reasm_pending(rp->reasm) > 0 &&
                   rp->last_reasm_feed_ns - rp->last_reasm_emit_ns >=
                       RX_REASM_FLUSH_TIMEOUT_NS) {
            /* No following picture arrived in time: emit what is buffered
             * instead of holding a fixed extra frame of latency. */
            if (slice_reasm_flush(rp->reasm, &frame, &frame_sz) == 1 && frame) {
                int dr = rx_decode_and_render(rp, frame, frame_sz, cap_ts);
                slice_reasm_free_frame(rp->reasm, frame);
                rp->last_reasm_emit_ns = now_ns();
                rp->stat_reasm_flushes++;
                if (dr < 0) {
                    rp->stat_decode_errs++;
                    atomic_fetch_add(&rp->consec_decode_errs, 1);
                } else {
                    atomic_store(&rp->consec_decode_errs, 0);
                }
            }
        }
    } else {
        int dr = rx_decode_and_render(rp, payload, payload_sz, cap_ts);
        if (dr < 0) {
            rp->stat_decode_errs++;
            atomic_fetch_add(&rp->consec_decode_errs, 1);
        } else {
            atomic_store(&rp->consec_decode_errs, 0);
        }
    }

    /* Info-change resolution: rebuild the renderer inline; the decode thread
     * owns the renderer here so no locking is needed. */
    {
        int nw = 0, nh = 0;
        if (mpp_dec_take_resolution(rp->decoder->priv, &nw, &nh)) {
            printf("rx_pipe: resolution change %dx%d → %dx%d\n",
                   rp->cur_width, rp->cur_height, nw, nh);
            if (rx_rebuild_renderer(rp, nw, nh) != 0)
                atomic_fetch_add(&rp->consec_render_fails, 1);
        }
    }
}

static void* decode_thread(void* arg) {
    struct rx_pipeline* rp = (struct rx_pipeline*)arg;

    while (atomic_load(&rp->running)) {
        struct codec_packet* pkt = NULL;
        if (ring_pop_wait(rp->cfg.input_ring, (void**)&pkt, &rp->running) != 0)
            continue;
        if (!pkt) continue;
        rp->stat_received++;

        rx_decode_packet(rp, pkt);

        if (pkt->mtl_owner) {
            rx_release_mtl_owner(rp, pkt);
        } else if (rp->cfg.coded_pool && pkt->data) {
            rx_hold_input(rp, pkt->data);
            pkt->data = NULL;
        }
        free(pkt);
    }

    /* Stop path: flush a tail frame stuck in the reassembler, then drain. */
    if (rp->reasm && slice_reasm_pending(rp->reasm) > 0) {
        uint8_t* frame = NULL;
        size_t frame_sz = 0;
        if (slice_reasm_flush(rp->reasm, &frame, &frame_sz) == 1 && frame) {
            rx_decode_and_render(rp, frame, frame_sz, 0);
            slice_reasm_free_frame(rp->reasm, frame);
            rp->stat_reasm_stop_flushes++;
        }
    }
    rx_drain_decoder(rp);
    return NULL;
}

/* Full rebuild (supervise thread only): join the data path, drop stale
 * inputs, recreate decoder + renderer, restart. Mirrors tx_pipeline_rebuild. */
static int rx_pipeline_rebuild(struct rx_pipeline* rp, const char* reason) {
    uint64_t t0 = now_ns();
    printf("rx_pipe: rebuilding decoder+renderer (%s, streak=%u)\n",
           reason, rp->consecutive_rebuilds + 1);

    if (rp->cfg.input_ring) ring_wake_all(rp->cfg.input_ring);
    if (rp->thd) { pthread_join(rp->thd, NULL); rp->thd = 0; }

    struct codec_config codec_cfg = {
        .format      = CODEC_FMT_H264,
        .width       = rp->cur_width, .height = rp->cur_height,
        .fps         = 60.0f,
        .low_latency = true,
        .num_slices  = rp->cfg.num_slices,
    };
    if (rp->decoder) { rp->decoder->free(rp->decoder->priv); free(rp->decoder); }
    rp->decoder = mpp_decoder_create(&codec_cfg);
    if (!rp->decoder) {
        fprintf(stderr, "rx_pipe: rebuild decoder create failed\n");
        return -1;
    }
    if (rx_rebuild_renderer(rp, rp->cur_width, rp->cur_height) != 0) return -1;
    if (rp->reasm) slice_reasm_reset(rp->reasm);
    /* Drop stale inputs queued during the hang; retention entries die with
     * the old decoder (no new MPP references them after the join). */
    rx_pipeline_drain_input(rp);
    atomic_store(&rp->consec_decode_errs, 0);
    atomic_store(&rp->consec_render_fails, 0);

    if (!atomic_load(&rp->running)) return -1;
    if (pthread_create(&rp->thd, NULL, decode_thread, rp) != 0) {
        rp->thd = 0;
        return -1;
    }
    cpu_set_t cset; CPU_ZERO(&cset); CPU_SET(rp->cfg.cpu_decode, &cset);
    pthread_setaffinity_np(rp->thd, sizeof(cset), &cset);

    rp->rebuild_done_ns = now_ns();
    rp->last_reasm_feed_ns = rp->rebuild_done_ns;
    rp->last_reasm_emit_ns = rp->rebuild_done_ns;
    rp->last_input_ns = rp->rebuild_done_ns;
    rp->silence_active = false;
    rp->last_rebuild_us = (now_ns() - t0) / 1000;
    printf("rx_pipe: rebuild done\n");
    return 0;
}

static void* supervise_thread(void* arg) {
    struct rx_pipeline* rp = (struct rx_pipeline*)arg;

    uint64_t prev_received = 0, prev_decoded = 0, prev_rendered = 0;
    uint64_t prev_bridge = 0, prev_bridge_bytes = 0, prev_mtl_rx = 0;

    while (atomic_load(&rp->running) && !rp->disabled) {
        sleep(RX_SUPERVISE_TICK_S);
        if (!atomic_load(&rp->running) || rp->disabled) break;

        uint64_t now = now_ns();
        uint64_t received = rp->stat_received;
        bool input_moving = received > prev_received;
        if (input_moving) {
            rp->last_input_ns = now;
            rp->silence_active = false;
        } else if (!rp->silence_active &&
                   now - rp->last_input_ns > RX_HANG_THRESHOLD_NS &&
                   now - rp->rebuild_done_ns >= RX_REBUILD_GRACE_NS) {
            rp->silence_events++;
            rp->silence_active = true;
        }

        /* Hang: input flows but nothing has been rendered for 3s. Silence on
         * the input side is net loss / signal loss, not a decoder fault. */
        bool trigger = false;
        const char* reason = "";
        bool is_hang = false, is_dec_err = false, is_render = false;
        uint64_t last_out = atomic_load(&rp->last_render_ns);
        bool in_grace = (now - rp->rebuild_done_ns < RX_REBUILD_GRACE_NS) ||
                        (rp->stat_rendered < 5);
        if (input_moving && !in_grace &&
            now - last_out > RX_HANG_THRESHOLD_NS) {
            fprintf(stderr, "rx_pipe: decode hang suspected "
                    "(input moving, no render for >%lldms), rebuilding\n",
                    (long long)(RX_HANG_THRESHOLD_NS / 1000000));
            trigger = true;
            reason = "decode-hang";
            is_hang = true;
        }
        if (!trigger &&
            atomic_load(&rp->consec_decode_errs) >= RX_MAX_CONSEC_DECODE_ERRS) {
            fprintf(stderr, "rx_pipe: %u consecutive decode errors, rebuilding\n",
                    atomic_load(&rp->consec_decode_errs));
            trigger = true;
            reason = "decode-errors";
            is_dec_err = true;
        }
        if (!trigger &&
            atomic_load(&rp->consec_render_fails) >= RX_MAX_CONSEC_RENDER_FAILS) {
            trigger = true;
            reason = "render-fails";
            is_render = true;
        }

        /* 1s aggregation, same cadence as tx_pipe[1s]. */
        uint64_t win_avg = rp->lat_win_count
            ? rp->lat_win_sum_ns / rp->lat_win_count : 0;
        uint64_t lat_p50 = 0, lat_p90 = 0, lat_p99 = 0;
        lat_win_percentiles(rp, &lat_p50, &lat_p90, &lat_p99);
        struct mpp_decode_stats dstat;
        memset(&dstat, 0, sizeof(dstat));
        if (rp->decoder)
            mpp_dec_get_decode_stats(rp->decoder->priv, &dstat);
        struct slice_reasm_stats rstat;
        memset(&rstat, 0, sizeof(rstat));
        if (rp->reasm) slice_reasm_stats(rp->reasm, &rstat);
        struct drm_renderer_stats gstat;
        memset(&gstat, 0, sizeof(gstat));
        if (rp->renderer) drm_renderer_stats(rp->renderer, &gstat);
        struct mtl_rx_stats mrx = {0};
        struct mtl_rx_bridge_stats bst = {0};
        if (rp->cfg.mtl_src) mtl_rx_stats(rp->cfg.mtl_src, &mrx);
        if (rp->cfg.bridge_src) mtl_rx_bridge_stats(rp->cfg.bridge_src, &bst);
        rp->rx_fps = (uint32_t)(received - prev_received);
        rp->dec_fps = (uint32_t)(rp->stat_decoded - prev_decoded);
        rp->render_fps = (uint32_t)(rp->stat_rendered - prev_rendered);
        printf("rx_pipe[1s]: rx=%llu/s dec=%llu/s render=%llu/s %llukbps "
               "drops={reasm=%llu dec_full=%llu put_fail=%llu render_fail=%llu sei_miss=%llu empty=%llu} "
               "seq={gap_ev=%llu lost=%llu dup=%llu} ts={zero=%llu back=%llu over=%llu} "
               "lat_win min/avg/max=%llu/%llu/%llu us p50/p90/p99=%llu/%llu/%llu us lat_cum min/max=%llu/%llu us "
               "dec_fps=%u take=%uus dec_lat=%u/%u/%uus chunks=%llu pull_empty=%llu corrupt=%llu discard=%llu "
               "info=%llu res=%llu mpp=%u/%uB reasm={c=%llu w=%llu ovs=%llu alloc=%llu flush=%llu slab=%llu malloc=%llu peak=%zu} "
               "hold=%d/%d disp_fps=%u disp=%lluus flipw=%lluus flipto=%llu addfb2=%llu flipfail=%llu mode=%dfps | "
               "mtl rx=%llu/s ring=%llu stop=%llu inflight=%d peak=%d poolfull=%llu max=%zu | "
               "bridge=%llu/s bad=%llu slab=%llu malloc=%llu bring=%llu copy=%lluus "
               "streak=%u rb=%llu/%llu/%llu silence=%llu%s\n",
               (unsigned long long)(received - prev_received),
               (unsigned long long)(rp->stat_decoded - prev_decoded),
               (unsigned long long)(rp->stat_rendered - prev_rendered),
               (unsigned long long)((bst.bytes - prev_bridge_bytes) * 8 / 1000),
               (unsigned long long)rp->stat_reasm_drops,
               (unsigned long long)dstat.input_full_drops,
               (unsigned long long)dstat.put_fail_drops,
               (unsigned long long)rp->stat_render_fails,
               (unsigned long long)rp->stat_sei_missing,
               (unsigned long long)rp->stat_empty_payload,
               (unsigned long long)rp->seq.gap_events,
               (unsigned long long)rp->seq.lost,
               (unsigned long long)rp->seq.dups,
               (unsigned long long)rp->stat_cap_ts_zero,
               (unsigned long long)rp->stat_lat_back,
               (unsigned long long)rp->stat_lat_over,
               (unsigned long long)(rp->lat_win_count
                   ? rp->lat_win_min_ns / 1000 : 0),
               (unsigned long long)(win_avg / 1000),
               (unsigned long long)(rp->lat_win_count
                   ? rp->lat_win_max_ns / 1000 : 0),
               (unsigned long long)(lat_p50 / 1000),
               (unsigned long long)(lat_p90 / 1000),
               (unsigned long long)(lat_p99 / 1000),
               (unsigned long long)(rp->lat_cum_min_ns / 1000),
               (unsigned long long)(rp->lat_cum_max_ns / 1000),
               (unsigned)dstat.decode_fps,
               (unsigned)dstat.take_ema_us,
               (unsigned)dstat.lat_p50_us, (unsigned)dstat.lat_p90_us,
               (unsigned)dstat.lat_p99_us,
               (unsigned long long)dstat.feed_chunks,
               (unsigned long long)dstat.pull_empty,
               (unsigned long long)dstat.corrupt_frames,
               (unsigned long long)dstat.discarded_frames,
               (unsigned long long)dstat.info_changes,
               (unsigned long long)dstat.resolution_changes,
               (unsigned)dstat.mpp_buf_now, (unsigned)dstat.mpp_buf_peak,
               (unsigned long long)rstat.feed_complete,
               (unsigned long long)rstat.feed_waiting,
               (unsigned long long)rstat.oversize_drops,
               (unsigned long long)rstat.alloc_fails,
               (unsigned long long)rstat.flushes,
               (unsigned long long)rstat.slab_frames,
               (unsigned long long)rstat.malloc_frames,
               rstat.pending_peak,
               rp->hold_count, RX_HOLD_N,
               (unsigned)gstat.display_fps,
               (unsigned long long)gstat.display_ema_us,
               (unsigned long long)gstat.flip_wait_ema_us,
               (unsigned long long)gstat.flip_timeouts,
               (unsigned long long)gstat.addfb2_fails,
               (unsigned long long)gstat.flip_fails,
               (int)gstat.mode_fps,
               (unsigned long long)(mrx.received - prev_mtl_rx),
               (unsigned long long)mrx.ring_drops,
               (unsigned long long)mrx.stopping_drops,
               mrx.inflight, mrx.peak,
               (unsigned long long)mrx.pool_full,
               mrx.frame_max_size,
               (unsigned long long)(bst.bridged - prev_bridge),
               (unsigned long long)bst.bad_drops,
               (unsigned long long)bst.slab_drops,
               (unsigned long long)bst.malloc_drops,
               (unsigned long long)bst.ring_drops,
               (unsigned long long)bst.copy_ema_us,
               rp->consecutive_rebuilds,
               (unsigned long long)rp->rebuild_decode_hang,
               (unsigned long long)rp->rebuild_decode_errors,
               (unsigned long long)rp->rebuild_render_fails,
               (unsigned long long)rp->silence_events,
               rp->disabled ? " DISABLED" : "");
        prev_received = received;
        prev_decoded = rp->stat_decoded;
        prev_rendered = rp->stat_rendered;
        prev_bridge = bst.bridged;
        prev_bridge_bytes = bst.bytes;
        prev_mtl_rx = mrx.received;
        rp->lat_win_min_ns = rp->lat_win_max_ns = 0;
        rp->lat_win_sum_ns = rp->lat_win_count = 0;
        rp->lat_win_fill = 0;

        if (!trigger) {
            rp->consecutive_rebuilds = 0;
            continue;
        }

        if (is_hang) rp->rebuild_decode_hang++;
        if (is_dec_err) rp->rebuild_decode_errors++;
        if (is_render) rp->rebuild_render_fails++;
        rp->consecutive_rebuilds++;
        if (rp->consecutive_rebuilds >= RX_MAX_CONSECUTIVE_REBUILDS) {
            rp->disabled = true;
            rp->disabled_reason = 1;
            fprintf(stderr, "rx_pipe: %u consecutive rebuild triggers — "
                    "supervision disabled, data path keeps running degraded\n",
                    rp->consecutive_rebuilds);
            break;
        }
        if (rx_pipeline_rebuild(rp, reason) != 0) {
            fprintf(stderr, "rx_pipe: rebuild failed (streak=%u)\n",
                    rp->consecutive_rebuilds);
        }
    }
    return NULL;
}

struct rx_pipeline* rx_pipeline_create(struct rx_pipeline_params* p) {
    struct rx_pipeline* rp = calloc(1, sizeof(*rp));
    if (!rp) return NULL;
    rp->cfg = *p;
    rp->cur_width = p->width;
    rp->cur_height = p->height;
    rx_seq_reset(&rp->seq);
    atomic_init(&rp->consec_decode_errs, 0);
    atomic_init(&rp->consec_render_fails, 0);
    atomic_init(&rp->last_render_ns, 0);

    struct codec_config codec_cfg = {
        .format      = CODEC_FMT_H264,
        .width       = p->width, .height = p->height,
        .fps         = 60.0f,
        .low_latency = true,
        .num_slices  = p->num_slices,
    };
    rp->decoder = mpp_decoder_create(&codec_cfg);
    if (!rp->decoder) { fprintf(stderr, "rx_pipe: decoder create failed\n"); goto fail; }

    if (p->num_slices > 1) {
        rp->reasm = slice_reasm_create();
        if (!rp->reasm) { fprintf(stderr, "rx_pipe: reasm create failed\n"); goto fail; }
        /* Slab-backed handoff when the pool is shared; malloc fallback
         * when the pipeline runs without one. */
        slice_reasm_set_pool(rp->reasm, p->coded_pool);
    }

    rp->renderer = drm_renderer_create(p->drm_device, p->drm_connector,
                                       p->drm_mode, p->width, p->height,
                                       DRM_FORMAT_NV12, p->drm_buf_count);
    if (!rp->renderer) { fprintf(stderr, "rx_pipe: renderer create failed\n"); goto fail; }

    rp->rebuild_done_ns = now_ns(); /* startup grace window */
    rp->last_input_ns = rp->rebuild_done_ns;
    rp->silence_active = false;
    rp->disabled_reason = 0;
    printf("rx_pipe: %dx%d slices=%d ready\n", p->width, p->height, p->num_slices);
    return rp;

fail:
    rx_pipeline_free(rp);
    return NULL;
}

void rx_pipeline_free(struct rx_pipeline* rp) {
    if (!rp) return;
    rx_pipeline_stop(rp);
    if (rp->reasm) slice_reasm_free(rp->reasm);
    if (rp->decoder)  { rp->decoder->free(rp->decoder->priv); free(rp->decoder); }
    if (rp->renderer) drm_renderer_free(rp->renderer);
    free(rp);
}

int rx_pipeline_start(struct rx_pipeline* rp) {
    if (!rp || !rp->cfg.input_ring) return -1;
    atomic_store(&rp->running, true);
    atomic_store(&rp->last_render_ns, now_ns());
    rp->last_input_ns = now_ns();
    rp->silence_active = false;
    rp->last_reasm_feed_ns = now_ns();
    rp->last_reasm_emit_ns = rp->last_reasm_feed_ns;
    if (pthread_create(&rp->thd, NULL, decode_thread, rp) != 0) {
        atomic_store(&rp->running, false);
        return -1;
    }
    cpu_set_t cset; CPU_ZERO(&cset); CPU_SET(rp->cfg.cpu_decode, &cset);
    pthread_setaffinity_np(rp->thd, sizeof(cset), &cset);
    if (pthread_create(&rp->thd_supervise, NULL, supervise_thread, rp) != 0) {
        rp->thd_supervise = 0; /* supervision is optional — continue without */
    }
    return 0;
}

static void rx_pipeline_drain_input(struct rx_pipeline* rp) {
    if (!rp || !rp->cfg.input_ring) return;
    struct codec_packet* pkt;
    while (ring_pop(rp->cfg.input_ring, (void**)&pkt) == 0) {
        if (!pkt) continue;
        if (pkt->mtl_owner) {
            rx_release_mtl_owner(rp, pkt);
        } else if (rp->cfg.coded_pool && pkt->data) {
            slab_free(rp->cfg.coded_pool, pkt->data);
        }
        free(pkt);
    }
    rx_hold_flush(rp);
}

void rx_pipeline_stop(struct rx_pipeline* rp) {
    if (!rp) return;
    if (!atomic_exchange(&rp->running, false)) return;
    if (rp->cfg.input_ring) ring_wake_all(rp->cfg.input_ring);
    /* Supervisor first: it must not trigger a rebuild during shutdown. */
    if (rp->thd_supervise) {
        pthread_join(rp->thd_supervise, NULL);
        rp->thd_supervise = 0;
    }
    if (rp->thd) { pthread_join(rp->thd, NULL); rp->thd = 0; }
    rx_pipeline_drain_input(rp);
    uint64_t dec_full = 0, dec_ema = 0;
    uint64_t put_fails = 0;
    if (rp->decoder) {
        struct mpp_decode_stats ds;
        memset(&ds, 0, sizeof(ds));
        mpp_dec_get_decode_stats(rp->decoder->priv, &ds);
        dec_full = ds.input_full_drops;
        dec_ema = ds.take_ema_us;
        put_fails = ds.put_fail_drops;
    }
    printf("rx_pipe: %llu rx, %llu dec, %llu render, "
           "drops{reasm=%llu (tflush=%llu sflush=%llu) dec_full=%llu put_fail=%llu render_fail=%llu sei_miss=%llu empty=%llu} "
           "seq{gap_ev=%llu lost=%llu dup=%llu} ts{zero=%llu back=%llu over=%llu} | "
           "e2e cum min/max=%llu/%llu us valid=%llu rb=%llu/%llu/%llu silence=%llu rebuild_us=%llu%s\n",
           (unsigned long long)rp->stat_received,
           (unsigned long long)rp->stat_decoded,
           (unsigned long long)rp->stat_rendered,
           (unsigned long long)rp->stat_reasm_drops,
           (unsigned long long)rp->stat_reasm_flushes,
           (unsigned long long)rp->stat_reasm_stop_flushes,
           (unsigned long long)dec_full,
           (unsigned long long)put_fails,
           (unsigned long long)rp->stat_render_fails,
           (unsigned long long)rp->stat_sei_missing,
           (unsigned long long)rp->stat_empty_payload,
           (unsigned long long)rp->seq.gap_events,
           (unsigned long long)rp->seq.lost,
           (unsigned long long)rp->seq.dups,
           (unsigned long long)rp->stat_cap_ts_zero,
           (unsigned long long)rp->stat_lat_back,
           (unsigned long long)rp->stat_lat_over,
           (unsigned long long)(rp->lat_cum_min_ns / 1000),
           (unsigned long long)(rp->lat_cum_max_ns / 1000),
           (unsigned long long)rp->stat_lat_valid,
           (unsigned long long)rp->rebuild_decode_hang,
           (unsigned long long)rp->rebuild_decode_errors,
           (unsigned long long)rp->rebuild_render_fails,
           (unsigned long long)rp->silence_events,
           (unsigned long long)rp->last_rebuild_us,
           rp->disabled ? " SUPERVISION-DISABLED" : "");
}

void rx_pipeline_get_latency(struct rx_pipeline* rp,
                             uint64_t* min_ns, uint64_t* avg_ns, uint64_t* max_ns) {
    if (!rp) return;
    if (min_ns) *min_ns = rp->lat_cum_min_ns;
    if (max_ns) *max_ns = rp->lat_cum_max_ns;
    if (avg_ns)
        *avg_ns = rp->lat_cum_count
            ? rp->lat_cum_sum_ns / rp->lat_cum_count : 0;
}

void rx_pipeline_stats(struct rx_pipeline* rp, struct rx_pipeline_stats* out) {
    if (!out) return;
    if (!rp) { memset(out, 0, sizeof(*out)); return; }
    out->received = rp->stat_received;
    out->decoded = rp->stat_decoded;
    out->rendered = rp->stat_rendered;
    out->rx_fps = rp->rx_fps;
    out->dec_fps = rp->dec_fps;
    out->render_fps = rp->render_fps;
    out->decode_errs = rp->stat_decode_errs;
    out->reasm_drops = rp->stat_reasm_drops;
    out->reasm_timeout_flushes = rp->stat_reasm_flushes;
    out->reasm_stop_flushes = rp->stat_reasm_stop_flushes;
    out->render_fails = rp->stat_render_fails;
    out->lat_valid = rp->stat_lat_valid;
    out->lat_zero = rp->stat_cap_ts_zero;
    out->lat_back = rp->stat_lat_back;
    out->lat_over = rp->stat_lat_over;
    out->lat_invalid = rp->stat_lat_invalid;
    {
        uint64_t p50 = 0, p90 = 0, p99 = 0;
        lat_win_percentiles(rp, &p50, &p90, &p99);
        out->lat_p50_us = (uint32_t)(p50 / 1000);
        out->lat_p90_us = (uint32_t)(p90 / 1000);
        out->lat_p99_us = (uint32_t)(p99 / 1000);
    }
    out->lat_win_avg_us = rp->lat_win_count
        ? rp->lat_win_sum_ns / rp->lat_win_count / 1000 : 0;
    out->lat_cum_min_us = rp->lat_cum_min_ns / 1000;
    out->lat_cum_max_us = rp->lat_cum_max_ns / 1000;
    out->sei_missing = rp->stat_sei_missing;
    out->empty_payload = rp->stat_empty_payload;
    out->seq_gap_events = rp->seq.gap_events;
    out->seq_lost = rp->seq.lost;
    out->seq_dups = rp->seq.dups;
    out->hold_in_use = rp->hold_count;
    out->hold_peak = rp->hold_peak;
    if (rp->decoder) {
        struct mpp_decode_stats ds;
        memset(&ds, 0, sizeof(ds));
        mpp_dec_get_decode_stats(rp->decoder->priv, &ds);
        out->dec_frames_out = ds.frames_out;
        out->dec_out_fps = ds.decode_fps;
        out->dec_take_ema_us = ds.take_ema_us;
        out->dec_p50_us = ds.lat_p50_us;
        out->dec_p90_us = ds.lat_p90_us;
        out->dec_p99_us = ds.lat_p99_us;
        out->dec_feed_chunks = ds.feed_chunks;
        out->dec_pull_empty = ds.pull_empty;
        out->dec_corrupt = ds.corrupt_frames;
        out->dec_discarded = ds.discarded_frames;
        out->dec_input_full = ds.input_full_drops;
        out->dec_put_fails = ds.put_fail_drops;
        out->dec_info_changes = ds.info_changes;
        out->dec_res_changes = ds.resolution_changes;
        out->dec_mpp_now = ds.mpp_buf_now;
        out->dec_mpp_peak = ds.mpp_buf_peak;
    }
    if (rp->reasm) {
        struct slice_reasm_stats rs;
        memset(&rs, 0, sizeof(rs));
        slice_reasm_stats(rp->reasm, &rs);
        out->reasm_complete = rs.feed_complete;
        out->reasm_waiting = rs.feed_waiting;
        out->reasm_errors = rs.feed_errors;
        out->reasm_oversize = rs.oversize_drops;
        out->reasm_alloc_fails = rs.alloc_fails;
        out->reasm_flushes = rs.flushes;
        out->reasm_slab_frames = rs.slab_frames;
        out->reasm_malloc_frames = rs.malloc_frames;
        out->reasm_pending_peak = rs.pending_peak;
    }
    if (rp->renderer) {
        struct drm_renderer_stats gs;
        memset(&gs, 0, sizeof(gs));
        drm_renderer_stats(rp->renderer, &gs);
        out->disp_displays = gs.displays;
        out->disp_fps = gs.display_fps;
        out->disp_ema_us = gs.display_ema_us;
        out->flip_wait_ema_us = gs.flip_wait_ema_us;
        out->flip_timeouts = gs.flip_timeouts;
        out->flip_pending_peak = gs.flip_pending_peak;
        out->addfb2_fails = gs.addfb2_fails;
        out->flip_fails = gs.flip_fails;
        out->disp_bad_args = gs.bad_args;
        out->disp_mode_fps = gs.mode_fps;
    }
    if (rp->cfg.mtl_src) {
        struct mtl_rx_stats ms;
        memset(&ms, 0, sizeof(ms));
        mtl_rx_stats(rp->cfg.mtl_src, &ms);
        out->mtl_received = ms.received;
        out->mtl_bytes = ms.bytes;
        out->mtl_pool_full = ms.pool_full;
        out->mtl_drops = ms.drops;
        out->mtl_ring_drops = ms.ring_drops;
        out->mtl_stopping_drops = ms.stopping_drops;
        out->mtl_fps = ms.fps;
        out->mtl_frame_max = ms.frame_max_size;
        out->mtl_inflight = ms.inflight;
        out->mtl_peak = ms.peak;
    }
    if (rp->cfg.bridge_src) {
        struct mtl_rx_bridge_stats bs;
        memset(&bs, 0, sizeof(bs));
        mtl_rx_bridge_stats(rp->cfg.bridge_src, &bs);
        out->bridge_bridged = bs.bridged;
        out->bridge_bytes = bs.bytes;
        out->bridge_drops = bs.drops;
        out->bridge_bad = bs.bad_drops;
        out->bridge_slab = bs.slab_drops;
        out->bridge_malloc = bs.malloc_drops;
        out->bridge_ring = bs.ring_drops;
        out->bridge_fps = bs.fps;
        out->bridge_copy_ema_us = bs.copy_ema_us;
    }
    out->rebuild_streak = rp->consecutive_rebuilds;
    out->rebuild_decode_hang = rp->rebuild_decode_hang;
    out->rebuild_decode_errors = rp->rebuild_decode_errors;
    out->rebuild_render_fails = rp->rebuild_render_fails;
    out->silence_events = rp->silence_events;
    out->last_rebuild_us = rp->last_rebuild_us;
    out->disabled = rp->disabled ? 1 : 0;
    out->disabled_reason = rp->disabled_reason;
}

int rx_pipeline_format_metrics(struct rx_pipeline* rp, char* buf, size_t len) {
    struct rx_pipeline_stats s;
    rx_pipeline_stats(rp, &s);
    return snprintf(buf, len,
        "rx=%llu dec=%llu render=%llu fps=%u/%u/%u "
        "drops={reasm=%llu dec_full=%llu put=%llu render=%llu sei=%llu empty=%llu} "
        "seq={gap=%llu lost=%llu dup=%llu} ts={zero=%llu back=%llu over=%llu} "
        "lat=%u/%u/%uus cum=%llu/%lluus dec_fps=%u take=%uus mpp=%u/%u hold=%d/%d "
        "disp=%u mode=%d rb=%llu/%llu/%llu silence=%llu%s",
        (unsigned long long)s.received, (unsigned long long)s.decoded,
        (unsigned long long)s.rendered, s.rx_fps, s.dec_fps, s.render_fps,
        (unsigned long long)s.reasm_drops, (unsigned long long)s.dec_input_full,
        (unsigned long long)s.dec_put_fails, (unsigned long long)s.render_fails,
        (unsigned long long)s.sei_missing, (unsigned long long)s.empty_payload,
        (unsigned long long)s.seq_gap_events, (unsigned long long)s.seq_lost,
        (unsigned long long)s.seq_dups,
        (unsigned long long)s.lat_zero, (unsigned long long)s.lat_back,
        (unsigned long long)s.lat_over,
        s.lat_p50_us, s.lat_p90_us, s.lat_p99_us,
        (unsigned long long)s.lat_cum_min_us,
        (unsigned long long)s.lat_cum_max_us,
        s.dec_out_fps, s.dec_take_ema_us, s.dec_mpp_now, s.dec_mpp_peak,
        s.hold_in_use, RX_HOLD_N,
        s.disp_fps, (int)s.disp_mode_fps,
        (unsigned long long)s.rebuild_decode_hang,
        (unsigned long long)s.rebuild_decode_errors,
        (unsigned long long)s.rebuild_render_fails,
        (unsigned long long)s.silence_events,
        s.disabled ? " disabled" : "");
}
