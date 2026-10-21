#define _GNU_SOURCE
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "rx_pipeline.h"
#include "ring_buffer.h"
#include "slice_reassembler.h"
#include "pipeline_common.h"
#include "../hal/sei_timestamp.h"
#include "../hal/slab_pool.h"
#include "../codec/mpp_decoder.h"
#include "../io/drm_renderer.h"
#include "../mtl_rx.h"
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

struct rx_pipeline {
    struct rx_pipeline_params cfg;

    struct codec_plugin*       decoder;
    struct drm_renderer*       renderer;
    struct slice_reassembler*  reasm;

    pthread_t                  thd;
    atomic_bool                running;

    uint64_t                   stat_received;
    uint64_t                   stat_decoded;
    uint64_t                   stat_rendered;
    uint32_t                   stat_drops;
    uint32_t                   stat_cap_ts_zero;  /* frames where SEI cap_ts missing */
    uint64_t                   lat_min_ns, lat_max_ns, lat_sum_ns, lat_count;
};

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void update_latency(struct rx_pipeline* rp, uint64_t ns) {
    if (ns < rp->lat_min_ns || rp->lat_count == 0) rp->lat_min_ns = ns;
    if (ns > rp->lat_max_ns) rp->lat_max_ns = ns;
    rp->lat_sum_ns += ns;
    rp->lat_count++;
}

static int rx_render_frame(struct rx_pipeline* rp, int out_fd, uint32_t out_h_stride,
                           uint32_t out_v_stride, uint64_t cap_ts, int use_cap_ts) {
    rp->stat_decoded++;
    if (drm_renderer_display(rp->renderer, out_fd, out_h_stride, out_v_stride) != 0)
        return -1;
    rp->stat_rendered++;
    uint64_t render_ts = rp->cfg.time_now ? rp->cfg.time_now(rp->cfg.time_priv)
                                          : now_ns();
    if (use_cap_ts && cap_ts)
        update_latency(rp, render_ts - cap_ts);
    else if (use_cap_ts && !cap_ts)
        rp->stat_cap_ts_zero++;
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

static void* decode_thread(void* arg) {
    struct rx_pipeline* rp = (struct rx_pipeline*)arg;

    while (atomic_load(&rp->running)) {
        struct codec_packet* pkt = NULL;
        if (ring_pop_wait(rp->cfg.input_ring, (void**)&pkt, &rp->running) != 0)
            continue;
        if (!pkt) continue;
        rp->stat_received++;

        uint64_t cap_ts = 0;
        size_t   dec_size = pkt->size;
        sei_timestamp_extract(pkt->data, &dec_size, NULL, &cap_ts);
        if (!cap_ts && rp->stat_received <= 3)
            printf("rx_pipe: cap_ts=0 on frame %llu\n",
                   (unsigned long long)rp->stat_received);

        if (rp->cfg.num_slices > 1 && rp->reasm) {
            uint8_t* frame = NULL;
            size_t frame_sz = 0;
            int rr = slice_reasm_feed(rp->reasm, pkt->data, dec_size, &frame, &frame_sz);
            if (rr == 1 && frame) {
                rx_decode_and_render(rp, frame, frame_sz, cap_ts);
                free(frame);
            } else if (rr < 0) {
                rp->stat_drops++;
            }
        } else {
            rx_decode_and_render(rp, pkt->data, dec_size, cap_ts);
        }

        if (pkt->mtl_owner) {
            rx_release_mtl_owner(rp, pkt);
        } else if (rp->cfg.coded_pool && pkt->data) {
            slab_free(rp->cfg.coded_pool, pkt->data);
        }
        free(pkt);
    }

    rx_drain_decoder(rp);
    return NULL;
}

struct rx_pipeline* rx_pipeline_create(struct rx_pipeline_params* p) {
    struct rx_pipeline* rp = calloc(1, sizeof(*rp));
    if (!rp) return NULL;
    rp->cfg = *p;

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
    }

    rp->renderer = drm_renderer_create(p->drm_device, p->drm_connector,
                                       p->drm_mode, p->width, p->height,
                                       DRM_FORMAT_NV12, p->drm_buf_count);
    if (!rp->renderer) { fprintf(stderr, "rx_pipe: renderer create failed\n"); goto fail; }

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
    if (pthread_create(&rp->thd, NULL, decode_thread, rp) != 0) {
        atomic_store(&rp->running, false);
        return -1;
    }
    cpu_set_t cset; CPU_ZERO(&cset); CPU_SET(rp->cfg.cpu_decode, &cset);
    pthread_setaffinity_np(rp->thd, sizeof(cset), &cset);
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
}

void rx_pipeline_stop(struct rx_pipeline* rp) {
    if (!rp) return;
    if (!atomic_exchange(&rp->running, false)) return;
    if (rp->cfg.input_ring) ring_wake_all(rp->cfg.input_ring);
    if (rp->thd) { pthread_join(rp->thd, NULL); rp->thd = 0; }
    rx_pipeline_drain_input(rp);
    uint64_t avg = rp->lat_count ? rp->lat_sum_ns / rp->lat_count : 0;
    printf("rx_pipe: %llu rx, %llu dec, %llu render, %u drops %u cap_ts_zero | "
           "e2e min=%llu avg=%llu max=%llu us\n",
           (unsigned long long)rp->stat_received,
           (unsigned long long)rp->stat_decoded,
           (unsigned long long)rp->stat_rendered,
           rp->stat_drops,
           rp->stat_cap_ts_zero,
           (unsigned long long)(rp->lat_min_ns / 1000),
           (unsigned long long)(avg / 1000),
           (unsigned long long)(rp->lat_max_ns / 1000));
}

void rx_pipeline_get_latency(struct rx_pipeline* rp,
                             uint64_t* min_ns, uint64_t* avg_ns, uint64_t* max_ns) {
    if (!rp) return;
    if (min_ns) *min_ns = rp->lat_min_ns;
    if (max_ns) *max_ns = rp->lat_max_ns;
    if (avg_ns) *avg_ns = rp->lat_count ? rp->lat_sum_ns / rp->lat_count : 0;
}
