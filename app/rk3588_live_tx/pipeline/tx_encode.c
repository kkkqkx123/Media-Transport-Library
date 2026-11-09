#define _GNU_SOURCE

#include "tx_pipeline_priv.h"

#include "ring_buffer.h"
#include "slab_pool.h"
#include "sei_timestamp.h"
#include "sps_vui.h"
#include "pipeline_common.h"
#include "codec_plugin.h"
#include "../codec/rga_converter.h"
#include "../codec/mpp_encoder.h"
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
#include <unistd.h>

size_t tx_encode_vui_fixup(struct tx_pipeline* tp, uint8_t* data, size_t size,
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

int tx_encode_recv(struct tx_pipeline* tp, uint8_t** data, size_t* size,
                   int* key) {
    int rr, retry = 3;
    while ((rr = tp->encoder->recv(tp->encoder->priv, data, size, key)) ==
               -EAGAIN &&
           retry-- > 0)
        usleep(500);
    return rr;
}

void tx_encode_on_frame(void* u, const struct v4l2_frame* vf) {
    struct tx_pipeline* tp = (struct tx_pipeline*)u;

    uint64_t cap_ts = tp->cfg.time_now ? tp->cfg.time_now(tp->cfg.time_priv) : 0;
    if (!cap_ts) cap_ts = vf->timestamp_ns;

    struct dmabuf_frame f = {
        .fd = vf->dma_fd,
        .fd_uv = vf->dma_fd_uv,
        .fourcc = v4l2_capturer_fourcc(tp->capture),
        .width = tp->cfg.width,
        .height = tp->cfg.height,
        .num_planes = v4l2_capturer_num_planes(tp->capture),
        .stride = vf->stride,
        .timestamp = cap_ts,
        .frame_seq = vf->frame_seq,
        .capture = tp->capture,
        .buf_index = vf->buf_index,
        .generation = vf->generation,
    };

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
        free(item);
    }
}

static int tx_encode_publish_mtl(struct tx_pipeline* tp,
                                 struct dmabuf_frame* f, uint64_t cap_ts,
                                 size_t payload_cap, int nv12_idx) {
    uint8_t* data;
    size_t size;
    int key;
    int rr = tx_encode_recv(tp, &data, &size, &key);
    if (rr != 1) {
        if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
        tp->stat_sink_drops++;
        tp->stat_recv_timeouts++;
        if (f->capture)
            v4l2_capturer_release(f->capture, f->buf_index, f->generation);
        free(f);
        return -1;
    }
    size = tx_encode_vui_fixup(tp, data, size, key);
    uint16_t fb_idx;
    void* fb_addr;
    if (tp->cfg.mtl_sink->acquire_fb(tp->cfg.mtl_sink->priv, &fb_idx,
                                     &fb_addr) == 0) {
        uint8_t* fb = (uint8_t*)fb_addr;
        sei_timestamp_build(fb, (uint32_t)f->frame_seq, cap_ts);
        if (size <= payload_cap) {
            mtl_memcpy(fb + SEI_TS_NAL_SIZE, data, size);
            size_t total = size + SEI_TS_NAL_SIZE;
            if (tp->cfg.mtl_sink->publish_fb(tp->cfg.mtl_sink->priv, fb_idx,
                                             total) == 0) {
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
    if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
    if (f->capture)
        v4l2_capturer_release(f->capture, f->buf_index, f->generation);
    free(f);
    return 0;
}

static int tx_encode_publish_ring(struct tx_pipeline* tp,
                                  struct dmabuf_frame* f, uint64_t cap_ts,
                                  size_t payload_cap, int nv12_idx) {
    uint8_t* base = slab_alloc(tp->slab);
    if (!base) {
        if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
        tp->stat_sink_drops++;
        tp->stat_mtl_acquire_fails++;
        if (f->capture)
            v4l2_capturer_release(f->capture, f->buf_index, f->generation);
        free(f);
        return -1;
    }
    sei_timestamp_build(base, (uint32_t)f->frame_seq, cap_ts);
    uint8_t* data;
    size_t sz;
    int key;
    int rr = tx_encode_recv(tp, &data, &sz, &key);
    if (rr != 1) {
        if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
        tp->stat_sink_drops++;
        tp->stat_recv_timeouts++;
        if (f->capture)
            v4l2_capturer_release(f->capture, f->buf_index, f->generation);
        slab_free(tp->slab, base);
        free(f);
        return -1;
    }
    sz = tx_encode_vui_fixup(tp, data, sz, key);
    size_t total = sz + SEI_TS_NAL_SIZE;
    if (sz <= payload_cap) {
        memcpy(base + SEI_TS_NAL_SIZE, data, sz);
    } else {
        tp->stat_sink_drops++;
        tp->stat_payload_oversize++;
        slab_free(tp->slab, data);
        slab_free(tp->slab, base);
        if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
        if (f->capture)
            v4l2_capturer_release(f->capture, f->buf_index, f->generation);
        free(f);
        return -1;
    }
    slab_free(tp->slab, data);
    struct codec_packet* pkt = malloc(sizeof(*pkt));
    if (pkt) {
        pkt->data = base;
        pkt->size = total;
        pkt->cap_timestamp = cap_ts;
        pkt->frame_seq = f->frame_seq;
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
    if (nv12_idx >= 0) rga_converter_release(tp->rga, nv12_idx);
    if (f->capture)
        v4l2_capturer_release(f->capture, f->buf_index, f->generation);
    free(f);
    return 0;
}

void* tx_encode_thread(void* arg) {
    struct tx_pipeline* tp = (struct tx_pipeline*)arg;
#if defined(__linux__) && defined(_GNU_SOURCE)
    pthread_setname_np(pthread_self(), "tx_enc");
#endif
    while (atomic_load(&tp->running)) {
        struct dmabuf_frame* f = NULL;
        if (ring_pop_wait(tp->ring_cap2enc, (void**)&f, &tp->running) != 0)
            continue;
        if (!f) continue;

        uint32_t min_stride = (uint32_t)tp->cfg.width;
        if (f->fourcc == V4L2_PIX_FMT_BGR24)
            min_stride = (uint32_t)tp->cfg.width * 3;
        else if (f->fourcc == V4L2_PIX_FMT_UYVY ||
                 f->fourcc == V4L2_PIX_FMT_YUYV)
            min_stride = (uint32_t)tp->cfg.width * 2;
        if (f->fd < 0 || f->stride < min_stride) {
            tp->stat_sink_drops++;
            tp->stat_invalid_frames++;
            if (f->capture)
                v4l2_capturer_release(f->capture, f->buf_index, f->generation);
            free(f);
            continue;
        }

        int enc_fd = f->fd;
        uint32_t enc_stride = f->stride;
        int nv12_idx = -1;
        /* RGA only when the encoder cannot take the format directly:
         * NV12 passes through; BGR888 (V4L2 BGR3) is supported natively
         * by the MPP encoder (round1 §2.2 direct-BGR path). */
        bool use_rga = !(f->fourcc == V4L2_PIX_FMT_NV12 && f->num_planes == 1) &&
                       f->fourcc != V4L2_PIX_FMT_BGR24;
        if (use_rga) {
            int nv12_fd;
            uint32_t nv12_stride;
            if (rga_convert(tp->rga, f->fd, f->stride, f->fourcc, &nv12_fd,
                            &nv12_stride, &nv12_idx) != 0) {
                tp->stat_sink_drops++;
                if (f->capture)
                    v4l2_capturer_release(f->capture, f->buf_index,
                                          f->generation);
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

        uint64_t cap_ts = f->timestamp;
        size_t payload_cap = (size_t)tp->cfg.coded_blk_size - SEI_TS_NAL_SIZE;

        if (tp->cfg.mtl_sink)
            tx_encode_publish_mtl(tp, f, cap_ts, payload_cap, nv12_idx);
        else
            tx_encode_publish_ring(tp, f, cap_ts, payload_cap, nv12_idx);
    }
    return NULL;
}
