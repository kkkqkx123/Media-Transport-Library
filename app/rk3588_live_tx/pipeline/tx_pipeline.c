#define _GNU_SOURCE

#include "tx_pipeline_priv.h"

#include "ring_buffer.h"
#include "slab_pool.h"
#include "pipeline_common.h"
#include "codec_plugin.h"
#include "../codec/rga_converter.h"
#include "../codec/mpp_encoder.h"
#include "../io/v4l2_capturer.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tx_pipeline_drain_cap2enc(struct tx_pipeline* tp) {
    if (!tp || !tp->ring_cap2enc) return;
    struct dmabuf_frame* f;
    while (ring_pop(tp->ring_cap2enc, (void**)&f) == 0) {
        if (!f) continue;
        if (f->capture)
            v4l2_capturer_release(f->capture, f->buf_index, f->generation);
        free(f);
    }
}

struct tx_pipeline* tx_pipeline_create(struct tx_pipeline_params* p) {
    struct tx_pipeline* tp = calloc(1, sizeof(*tp));
    if (!tp) return NULL;
    tp->cfg = *p;

    tp->ring_cap2enc = ring_create(p->ring_size);
    tp->ring_enc2dec = ring_create(p->ring_size);
    if (!tp->ring_cap2enc || !tp->ring_enc2dec) goto fail;

    /* Capture must be created before the encoder: the negotiated V4L2
     * format decides whether the encoder takes BGR888 directly (no RGA).
     * Streaming only starts later in tx_pipeline_start(). */
    tp->capture = v4l2_capturer_create(p->v4l2_device, p->width, p->height,
                                       p->fps, p->v4l2_buf_count,
                                       tx_encode_on_frame, tp);
    if (!tp->capture) {
        int saved = errno;
        fprintf(stderr, "tx_pipe: capture create failed (%s)\n",
                saved == ENODATA ? "no signal" : strerror(saved));
        errno = saved;
        goto fail_errno;
    }
    v4l2_capturer_set_error_cb(tp->capture, tx_supervise_error_cb, tp);
    v4l2_capturer_set_signal_change_cb(tp->capture, tx_supervise_signal_cb,
                                       tp);
    tp->input_fourcc = v4l2_capturer_fourcc(tp->capture);

    tp->rga = rga_converter_create(p->width, p->height, p->rga_pool_size);
    if (!tp->rga) {
        fprintf(stderr, "tx_pipe: RGA create failed\n");
        goto fail;
    }

    tp->slab = slab_pool_create(p->ring_size * 3, (size_t)p->coded_blk_size);
    if (!tp->slab) {
        fprintf(stderr, "tx_pipe: slab create failed\n");
        goto fail;
    }

    struct codec_config codec_cfg = {
        .format = p->codec,
        .width = p->width,
        .height = p->height,
        .fps = p->fps,
        .bitrate_bps = p->bitrate_bps,
        .gop_size = p->gop_size,
        .b_frames = 0,
        .low_latency = true,
        .num_slices = p->num_slices,
        .intra_refresh_mode = p->intra_refresh_mode,
        .intra_refresh_arg = p->intra_refresh_arg,
        .input_fourcc = tp->input_fourcc,
    };
    tp->encoder = mpp_encoder_create(&codec_cfg, tp->slab);
    if (!tp->encoder) {
        fprintf(stderr, "tx_pipe: encoder create failed\n");
        goto fail;
    }

    atomic_init(&tp->flag_capture_error, false);
    atomic_init(&tp->flag_signal_change, false);
    atomic_init(&tp->last_captured_seen, 0);
    tp->last_capture_progress_ns = tx_pipeline_now_ns();
    tp->rebuild_done_ns = tx_pipeline_now_ns();

    printf("tx_pipe: %dx%d@%.0ffps %dbps slices=%d ready\n", p->width,
           p->height, (double)p->fps, p->bitrate_bps, p->num_slices);
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
    if (tp->capture) v4l2_capturer_free(tp->capture);
    if (tp->encoder) {
        tp->encoder->free(tp->encoder->priv);
        free(tp->encoder);
    }
    if (tp->rga) rga_converter_free(tp->rga);
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
    if (tp->slab) slab_pool_free(tp->slab);
    free(tp);
}

int tx_pipeline_start(struct tx_pipeline* tp) {
    if (!tp) return -1;
    atomic_store(&tp->running, true);
    if (tp->encoder->start(tp->encoder->priv) != 0) {
        atomic_store(&tp->running, false);
        return -1;
    }
    if (pthread_create(&tp->thd_encode, NULL, tx_encode_thread, tp) != 0) {
        atomic_store(&tp->running, false);
        tp->encoder->stop(tp->encoder->priv);
        return -1;
    }
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
    if (pthread_create(&tp->thd_supervise, NULL, tx_supervise_thread, tp) !=
        0) {
        tp->thd_supervise = 0;
    }
    return 0;
}

void tx_pipeline_stop(struct tx_pipeline* tp) {
    if (!tp) return;
    if (!atomic_exchange(&tp->running, false)) return;
    if (tp->ring_cap2enc) ring_wake_all(tp->ring_cap2enc);
    if (tp->thd_supervise) {
        pthread_join(tp->thd_supervise, NULL);
        tp->thd_supervise = 0;
    }
    if (tp->capture) v4l2_capturer_stop(tp->capture);
    if (tp->thd_encode) {
        pthread_join(tp->thd_encode, NULL);
        tp->thd_encode = 0;
    }
    if (tp->encoder) tp->encoder->stop(tp->encoder->priv);
    tx_pipeline_drain_cap2enc(tp);
    printf("tx_pipe: %llu captured, %llu encoded, ring_drop=%u sink_drop=%u "
           "(send=%llu recv=%llu oversize=%llu invalid=%llu acq=%llu pub=%llu e2e=%llu) "
           "rb=%llu/%llu/%llu/%llu\n",
           (unsigned long long)tp->stat_captured,
           (unsigned long long)tp->stat_encoded, tp->stat_ring_drops,
           tp->stat_sink_drops, (unsigned long long)tp->stat_send_fails,
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
