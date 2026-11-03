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

    struct rx_seq_tracker      seq;
    uint64_t                   last_reasm_feed_ns;
    uint64_t                   last_reasm_emit_ns;

    uint64_t                   stat_received;
    uint64_t                   stat_decoded;
    uint64_t                   stat_rendered;
    uint64_t                   stat_decode_errs;
    uint64_t                   stat_reasm_drops;
    uint64_t                   stat_reasm_flushes;
    uint64_t                   stat_render_fails;
    uint64_t                   stat_cap_ts_zero;
    uint64_t                   stat_lat_invalid;
    uint64_t                   lat_win_min_ns, lat_win_max_ns;
    uint64_t                   lat_win_sum_ns, lat_win_count;
    uint64_t                   lat_cum_min_ns, lat_cum_max_ns;
    uint64_t                   lat_cum_sum_ns, lat_cum_count;
    atomic_ullong              last_render_ns; /* hang-guard heartbeat */
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
    if (rp->lat_cum_min_ns == 0 || ns < rp->lat_cum_min_ns)
        rp->lat_cum_min_ns = ns;
    if (ns > rp->lat_cum_max_ns) rp->lat_cum_max_ns = ns;
    rp->lat_cum_sum_ns += ns;
    rp->lat_cum_count++;
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
    if (!cap_ts) {
        rp->stat_cap_ts_zero++;
        return 0;
    }
    uint64_t render_ts = rp->cfg.time_now ? rp->cfg.time_now(rp->cfg.time_priv)
                                          : now_ns();
    uint64_t delta;
    if (!rx_lat_valid(cap_ts, render_ts, &delta)) {
        rp->stat_lat_invalid++;
        return 0;
    }
    update_latency(rp, delta);
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
    if (has_sei) rx_seq_observe(&rp->seq, frame_seq);

    /* SEI stays in the slab; the decoder starts at the payload offset. */
    size_t off = rx_sei_payload_offset(has_sei, SEI_TS_NAL_SIZE);
    uint8_t* payload = pkt->data + off;
    size_t payload_sz = (pkt->size > off) ? pkt->size - off : 0;
    if (payload_sz == 0) {
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
        }
    }
    rx_drain_decoder(rp);
    return NULL;
}

/* Full rebuild (supervise thread only): join the data path, drop stale
 * inputs, recreate decoder + renderer, restart. Mirrors tx_pipeline_rebuild. */
static int rx_pipeline_rebuild(struct rx_pipeline* rp, const char* reason) {
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

        uint64_t received = rp->stat_received;
        bool input_moving = received > prev_received;

        /* Hang: input flows but nothing has been rendered for 3s. Silence on
         * the input side is net loss / signal loss, not a decoder fault. */
        bool trigger = false;
        const char* reason = "";
        uint64_t last_out = atomic_load(&rp->last_render_ns);
        bool in_grace = (now_ns() - rp->rebuild_done_ns < RX_REBUILD_GRACE_NS) ||
                        (rp->stat_rendered < 5);
        if (input_moving && !in_grace &&
            now_ns() - last_out > RX_HANG_THRESHOLD_NS) {
            fprintf(stderr, "rx_pipe: decode hang suspected "
                    "(input moving, no render for >%lldms), rebuilding\n",
                    (long long)(RX_HANG_THRESHOLD_NS / 1000000));
            trigger = true;
            reason = "decode-hang";
        }
        if (!trigger &&
            atomic_load(&rp->consec_decode_errs) >= RX_MAX_CONSEC_DECODE_ERRS) {
            fprintf(stderr, "rx_pipe: %u consecutive decode errors, rebuilding\n",
                    atomic_load(&rp->consec_decode_errs));
            trigger = true;
            reason = "decode-errors";
        }
        if (!trigger &&
            atomic_load(&rp->consec_render_fails) >= RX_MAX_CONSEC_RENDER_FAILS) {
            trigger = true;
            reason = "render-fails";
        }

        /* 1s aggregation, same cadence as tx_pipe[1s]. */
        uint64_t win_avg = rp->lat_win_count
            ? rp->lat_win_sum_ns / rp->lat_win_count : 0;
        uint64_t dec_full = 0, dec_ema = 0;
        if (rp->decoder)
            mpp_dec_get_stats(rp->decoder->priv, &dec_full, &dec_ema, NULL);
        struct mtl_rx_stats mrx = {0};
        struct mtl_rx_bridge_stats bst = {0};
        if (rp->cfg.mtl_src) mtl_rx_stats(rp->cfg.mtl_src, &mrx);
        if (rp->cfg.bridge_src) mtl_rx_bridge_stats(rp->cfg.bridge_src, &bst);
        printf("rx_pipe[1s]: rx=%llu/s dec=%llu/s render=%llu/s %llukbps "
               "drops={reasm=%llu dec_full=%llu render_fail=%llu} "
               "seq={gap_ev=%llu lost=%llu dup=%llu} ts={zero=%llu invalid=%llu} "
               "lat_win min/avg/max=%llu/%llu/%llu us lat_cum min/max=%llu/%llu us "
               "dec_ema=%lluus | mtl rx=%llu/s inflight=%d peak=%d poolfull=%llu | "
               "bridge=%llu/s streak=%u%s\n",
               (unsigned long long)(received - prev_received),
               (unsigned long long)(rp->stat_decoded - prev_decoded),
               (unsigned long long)(rp->stat_rendered - prev_rendered),
               (unsigned long long)((bst.bytes - prev_bridge_bytes) * 8 / 1000),
               (unsigned long long)rp->stat_reasm_drops,
               (unsigned long long)dec_full,
               (unsigned long long)rp->stat_render_fails,
               (unsigned long long)rp->seq.gap_events,
               (unsigned long long)rp->seq.lost,
               (unsigned long long)rp->seq.dups,
               (unsigned long long)rp->stat_cap_ts_zero,
               (unsigned long long)rp->stat_lat_invalid,
               (unsigned long long)(rp->lat_win_count
                   ? rp->lat_win_min_ns / 1000 : 0),
               (unsigned long long)(win_avg / 1000),
               (unsigned long long)(rp->lat_win_count
                   ? rp->lat_win_max_ns / 1000 : 0),
               (unsigned long long)(rp->lat_cum_min_ns / 1000),
               (unsigned long long)(rp->lat_cum_max_ns / 1000),
               (unsigned long long)dec_ema,
               (unsigned long long)(mrx.received - prev_mtl_rx),
               mrx.inflight, mrx.peak,
               (unsigned long long)mrx.pool_full,
               (unsigned long long)(bst.bridged - prev_bridge),
               rp->consecutive_rebuilds,
               rp->disabled ? " DISABLED" : "");
        prev_received = received;
        prev_decoded = rp->stat_decoded;
        prev_rendered = rp->stat_rendered;
        prev_bridge = bst.bridged;
        prev_bridge_bytes = bst.bytes;
        prev_mtl_rx = mrx.received;
        rp->lat_win_min_ns = rp->lat_win_max_ns = 0;
        rp->lat_win_sum_ns = rp->lat_win_count = 0;

        if (!trigger) {
            rp->consecutive_rebuilds = 0;
            continue;
        }

        rp->consecutive_rebuilds++;
        if (rp->consecutive_rebuilds >= RX_MAX_CONSECUTIVE_REBUILDS) {
            rp->disabled = true;
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
    if (rp->decoder)
        mpp_dec_get_stats(rp->decoder->priv, &dec_full, &dec_ema, NULL);
    printf("rx_pipe: %llu rx, %llu dec, %llu render, "
           "drops{reasm=%llu dec_full=%llu render_fail=%llu} "
           "seq{gap_ev=%llu lost=%llu dup=%llu} ts{zero=%llu invalid=%llu} | "
           "e2e cum min/max=%llu/%llu us%s\n",
           (unsigned long long)rp->stat_received,
           (unsigned long long)rp->stat_decoded,
           (unsigned long long)rp->stat_rendered,
           (unsigned long long)rp->stat_reasm_drops,
           (unsigned long long)dec_full,
           (unsigned long long)rp->stat_render_fails,
           (unsigned long long)rp->seq.gap_events,
           (unsigned long long)rp->seq.lost,
           (unsigned long long)rp->seq.dups,
           (unsigned long long)rp->stat_cap_ts_zero,
           (unsigned long long)rp->stat_lat_invalid,
           (unsigned long long)(rp->lat_cum_min_ns / 1000),
           (unsigned long long)(rp->lat_cum_max_ns / 1000),
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
