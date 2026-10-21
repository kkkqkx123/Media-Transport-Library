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
#include "../hal/slab_pool.h"
#include "../hal/sei_timestamp.h"
#include "../codec/rga_converter.h"
#include "../codec/mpp_encoder.h"
#include "../io/v4l2_capturer.h"
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
    atomic_bool            running;

    /* stats */
    uint64_t               stat_captured;
    uint64_t               stat_encoded;
    uint32_t               stat_ring_drops;
};

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ---- V4L2 capture callback: runs in capture thread ---- */
static void capture_on_frame(void* u, int fd, int fd_uv,
                             uint32_t stride, uint32_t sz,
                             uint64_t ts, uint64_t seq) {
    struct tx_pipeline* tp = (struct tx_pipeline*)u;
    (void)fd_uv;

    /* Capture timestamp: PTP preferred; fall back if not yet synchronized. */
    uint64_t cap_ts = tp->cfg.time_now ? tp->cfg.time_now(tp->cfg.time_priv) : 0;
    if (!cap_ts)
        cap_ts = now_ns();

    struct dmabuf_frame f = {
        .fd        = fd,
        .fourcc    = 0x33524742, /* BGR3 from rk_hdmirx */
        .width     = tp->cfg.width,
        .height    = tp->cfg.height,
        .stride    = stride,
        .timestamp = cap_ts,   /* ★ E2E start: PTP or MONOTONIC */
        .frame_seq = seq,
    };

    /* deep copy the struct onto the ring (not the stack addr!) */
    struct dmabuf_frame* item = malloc(sizeof(*item));
    if (!item) return;
    memcpy(item, &f, sizeof(f));

    if (ring_push(tp->ring_cap2enc, item) == 0) {
        tp->stat_captured++;
    } else {
        tp->stat_ring_drops++;
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

        /* RGA: BGR3 → NV12 (zero-copy via RGA hardware DMA) */
        int nv12_fd; uint32_t nv12_stride;
        if (rga_convert(tp->rga, f->fd, f->stride, &nv12_fd, &nv12_stride) != 0) {
            free(f);
            continue;
        }

        if (tp->encoder->send(tp->encoder->priv, nv12_fd, nv12_stride) != 0) {
            free(f);
            continue;
        }

        uint64_t cap_ts = f->timestamp;
        if (!cap_ts) {
            cap_ts = tp->cfg.time_now ? tp->cfg.time_now(tp->cfg.time_priv) : 0;
            if (!cap_ts)
                cap_ts = now_ns();
        }
        size_t   payload_cap = (size_t)tp->cfg.coded_blk_size - SEI_TS_NAL_SIZE;

        if (tp->cfg.mtl_sink) {
            /* Use proven enc_recv (blocking) + memcpy to MTL fb.
             * recv_into direct-write retry loop is fragile: MPP async
             * timing causes excessive waits → frame drops → mosaic.
             * The ~200KB memcpy overhead (<0.1ms) is negligible vs
             * 16.7ms frame period. */
            uint8_t* data; size_t size; int key;
            int rr, retry = 100; /* 100 × 100µs = 10ms, MPP P-frame <2ms */
            while ((rr = tp->encoder->recv(tp->encoder->priv, &data, &size, &key)) == -EAGAIN
                   && retry-- > 0)
                usleep(100);
            if (rr != 1) { free(f); continue; }
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
                                                     fb_idx, total) == 0)
                        tp->stat_encoded++;
                    else {
                        tp->cfg.mtl_sink->abort_fb(tp->cfg.mtl_sink->priv, fb_idx);
                        tp->stat_ring_drops++;
                    }
                } else {
                    tp->cfg.mtl_sink->abort_fb(tp->cfg.mtl_sink->priv, fb_idx);
                    tp->stat_ring_drops++;
                }
            }
            slab_free(tp->slab, data);
        } else {
            uint8_t* base = slab_alloc(tp->slab);
            if (!base) { free(f); continue; }
            sei_timestamp_build(base, (uint32_t)f->frame_seq, cap_ts);
            uint8_t* data; size_t sz; int key;
            int rr, retry = 100;
            while ((rr = tp->encoder->recv(tp->encoder->priv, &data, &sz, &key)) == -EAGAIN
                   && retry-- > 0)
                usleep(100);
            if (rr != 1) { slab_free(tp->slab, base); free(f); continue; }
            size_t total = sz + SEI_TS_NAL_SIZE;
            if (sz <= payload_cap) {
                memcpy(base + SEI_TS_NAL_SIZE, data, sz);
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
                } else {
                    tp->stat_ring_drops++;
                    slab_free(tp->slab, base);
                    free(pkt);
                }
            } else {
                slab_free(tp->slab, base);
            }
        }

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

    /* V4L2 capture */
    tp->capture = v4l2_capturer_create(p->v4l2_device, p->width, p->height,
                                       p->fps, p->v4l2_buf_count,
                                       capture_on_frame, tp);
    if (!tp->capture) { fprintf(stderr, "tx_pipe: capture create failed\n"); goto fail; }

    printf("tx_pipe: %dx%d@%.0ffps %dbps slices=%d ready\n",
           p->width, p->height, (double)p->fps, p->bitrate_bps, p->num_slices);
    return tp;

fail:
    tx_pipeline_free(tp);
    return NULL;
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
        void* item;
        while (ring_pop(tp->ring_cap2enc, &item) == 0) free(item);
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
    if (pthread_create(&tp->thd_encode, NULL, encode_thread, tp) != 0) {
        atomic_store(&tp->running, false);
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
        return -1;
    }
    return 0;
}

static void tx_pipeline_drain_cap2enc(struct tx_pipeline* tp) {
    if (!tp || !tp->ring_cap2enc) return;
    struct dmabuf_frame* f;
    while (ring_pop(tp->ring_cap2enc, (void**)&f) == 0)
        free(f);
}

void tx_pipeline_stop(struct tx_pipeline* tp) {
    if (!tp) return;
    if (!atomic_exchange(&tp->running, false)) return; /* already stopped */
    if (tp->ring_cap2enc) ring_wake_all(tp->ring_cap2enc);
    if (tp->capture) v4l2_capturer_stop(tp->capture);
    if (tp->thd_encode) { pthread_join(tp->thd_encode, NULL); tp->thd_encode = 0; }
    tx_pipeline_drain_cap2enc(tp);
    printf("tx_pipe: %llu captured, %llu encoded, %u drops\n",
           (unsigned long long)tp->stat_captured,
           (unsigned long long)tp->stat_encoded,
           tp->stat_ring_drops);
}

struct ring_buffer* tx_pipeline_get_output_ring(struct tx_pipeline* tp) {
    return tp ? tp->ring_enc2dec : NULL;
}

struct slab_pool* tx_pipeline_get_slab_pool(struct tx_pipeline* tp) {
    return tp ? tp->slab : NULL;
}
