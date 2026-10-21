/* SPDX-License-Identifier: BSD-3-Clause
 * MPP H.264 hardware decoder (RK3588 VPU).
 * H.264 Annex B packets → NV12 dma-buf frames.
 */

#include "mpp_decoder.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_vdec_cfg.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_vdec_cmd.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FB_DELAY          8
#define MPP_BUF_COUNT     32
#define FEED_CHUNK_SIZE   8192
#define PUT_RETRY_MAX          12
#define AU_DUMP_MAX            512 * 1024
#define AGGRESSIVE_IDLE_ROUNDS 4   /* 1 ms sleep between tries after each AU */
#define EOS_FLUSH_IDLE_ROUNDS  40  /* up to ~40 ms after EOS at shutdown */
#define DRAIN_PENDING_MAX      64  /* backpressure frames queued for mpp_dec_pull */

struct drain_pending {
    int      fd;
    uint32_t hor_stride;
    uint32_t ver_stride;
};

struct mpp_dec {
    MppCtx         ctx;
    MppApi*        api;
    int            w, h;
    int            split_parse;
    int            feed_whole_au;
    int            aggressive_pull;
    bool           eos_sent;
    MppBufferGroup frm_grp;
    MppFrame       held[FB_DELAY];
    int            held_idx;
    bool           info_change_done;
    int            frame_count;
    mpp_dec_stats  stats;
    int            dump_fail_left;
    int            dump_err_left;
    uint8_t*       last_au;
    size_t         last_au_size;
    size_t         last_au_cap;
    pthread_mutex_t lock;
    struct drain_pending drain_pending[DRAIN_PENDING_MAX];
    int            drain_pending_cnt;
};

static int drain_pending_push(struct mpp_dec* d, int fd, uint32_t hs, uint32_t vs) {
    if (d->drain_pending_cnt >= DRAIN_PENDING_MAX)
        return -ENOSPC;
    struct drain_pending* p = &d->drain_pending[d->drain_pending_cnt++];
    p->fd         = fd;
    p->hor_stride = hs;
    p->ver_stride = vs;
    return 0;
}

static int drain_pending_pop(struct mpp_dec* d, int* fd_out,
                             uint32_t* hor_stride_out, uint32_t* ver_stride_out) {
    if (!d->drain_pending_cnt)
        return 0;
    struct drain_pending p = d->drain_pending[0];
    for (int i = 1; i < d->drain_pending_cnt; i++)
        d->drain_pending[i - 1] = d->drain_pending[i];
    d->drain_pending_cnt--;

    *fd_out = p.fd;
    if (hor_stride_out) *hor_stride_out = p.hor_stride;
    if (ver_stride_out) *ver_stride_out = p.ver_stride;
    return 1;
}

static inline void msleep(int ms) { usleep((unsigned)(ms) * 1000); }

static inline void busy_wait_us(int us) {
    for (int i = 0; i < us; i++)
        __asm__ volatile("yield" ::: "memory");
}

static void maybe_dump_au(const char* prefix, int idx, const uint8_t* data,
                          size_t size) {
    if (!data || !size) return;

    char path[128];
    snprintf(path, sizeof(path), "/tmp/mp4_dist_mpp_%s_%d.h264", prefix, idx);
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "mpp_dec: cannot dump %s\n", path);
        return;
    }
    fwrite(data, 1, size, fp);
    fclose(fp);
    fprintf(stderr, "mpp_dec: dumped %s AU %zu B -> %s\n", prefix, size, path);
}

static void maybe_dump_fail_au(const struct mpp_dec* d, const uint8_t* data,
                               size_t size) {
    if (!d || d->dump_fail_left <= 0 || !data || !size) return;
    maybe_dump_au("fail", PUT_RETRY_MAX - d->dump_fail_left, data, size);
}

static void maybe_dump_err_au(struct mpp_dec* d) {
    if (!d || d->dump_err_left <= 0 || !d->last_au || !d->last_au_size) return;
    maybe_dump_au("err", 3 - d->dump_err_left, d->last_au, d->last_au_size);
    d->dump_err_left--;
}

static void remember_au(struct mpp_dec* d, const uint8_t* data, size_t size) {
    if (!d || !data || !size) return;
    if (size > AU_DUMP_MAX) size = AU_DUMP_MAX;
    if (size > d->last_au_cap) {
        size_t cap = size + 4096;
        uint8_t* nb = realloc(d->last_au, cap);
        if (!nb) return;
        d->last_au = nb;
        d->last_au_cap = cap;
    }
    memcpy(d->last_au, data, size);
    d->last_au_size = size;
}

static int handle_info_change(struct mpp_dec* d, MppFrame frame);

static int dec_get_frame(struct mpp_dec* d, int* fd_out, uint32_t* hor_stride_out,
                         uint32_t* ver_stride_out, bool count_output);

static void dec_drain_for_backpressure(struct mpp_dec* d) {
    int fd;
    uint32_t hs, vs;

    while (dec_get_frame(d, &fd, &hs, &vs, true)) {
        d->stats.drain_pull++;
        if (drain_pending_push(d, fd, hs, vs) < 0) {
            fprintf(stderr, "mpp_dec: drain pending full, frame held for release\n");
            break;
        }
    }
}

static int dec_put_packet_once(struct mpp_dec* d, uint8_t* data, size_t size) {
    MppPacket pkt = NULL;
    mpp_packet_init(&pkt, NULL, 0);
    mpp_packet_set_data(pkt, data);
    mpp_packet_set_size(pkt, size);
    mpp_packet_set_pos(pkt, data);
    mpp_packet_set_length(pkt, size);

    MPP_RET r = d->api->decode_put_packet(d->ctx, pkt);
    mpp_packet_deinit(&pkt);
    return (r == MPP_OK) ? 0 : -EIO;
}

static int dec_put_packet(struct mpp_dec* d, uint8_t* data, size_t size) {
    if (dec_put_packet_once(d, data, size) == 0) {
        d->stats.put_ok++;
        return 0;
    }

    for (int attempt = 0; attempt < PUT_RETRY_MAX; attempt++) {
        dec_drain_for_backpressure(d);
        msleep(1 + attempt / 2);

        if (dec_put_packet_once(d, data, size) == 0) {
            d->stats.put_ok++;
            return 0;
        }
    }

    d->stats.put_fail++;
    return -EIO;
}

static int dec_get_frame(struct mpp_dec* d, int* fd_out, uint32_t* hor_stride_out,
                         uint32_t* ver_stride_out, bool count_output) {
    int got_frame = 0;

    while (1) {
        MppFrame f = NULL;
        MPP_RET r;
        int retry = 40;

    try_again:
        r = d->api->decode_get_frame(d->ctx, &f);

        if (r == MPP_ERR_TIMEOUT) {
            if (retry-- > 0) {
                busy_wait_us(50);
                goto try_again;
            }
            break;
        }
        if (r != MPP_OK || !f) break;

        if (mpp_frame_get_info_change(f)) {
            handle_info_change(d, f);
            mpp_frame_deinit(&f);
            continue;
        }

        RK_U32 err_info = mpp_frame_get_errinfo(f);
        RK_U32 discard  = mpp_frame_get_discard(f);
        if (err_info) d->stats.err_info++;
        if (discard)  d->stats.discard++;
        if (err_info || discard) {
            maybe_dump_err_au(d);
            mpp_frame_deinit(&f);
            continue;
        }

        MppBuffer buf = mpp_frame_get_buffer(f);
        if (!buf) {
            d->stats.no_buffer++;
            mpp_frame_deinit(&f);
            continue;
        }

        int fd = mpp_buffer_get_fd(buf);
        if (fd < 0) {
            d->stats.no_buffer++;
            mpp_frame_deinit(&f);
            continue;
        }

        if (!count_output) {
            mpp_frame_deinit(&f);
            return 1;
        }

        int release_idx = d->held_idx;
        if (d->held[release_idx]) {
            mpp_frame_deinit(&d->held[release_idx]);
            d->held[release_idx] = NULL;
        }

        *fd_out = fd;
        if (hor_stride_out) *hor_stride_out = mpp_frame_get_hor_stride(f);
        if (ver_stride_out) *ver_stride_out = mpp_frame_get_ver_stride(f);
        if (d->frame_count == 0) {
            printf("mpp_dec: first frame stride %ux%u (display %dx%d)\n",
                   mpp_frame_get_hor_stride(f), mpp_frame_get_ver_stride(f),
                   d->w, d->h);
        }

        d->held[d->held_idx] = f;
        d->held_idx = (d->held_idx + 1) % FB_DELAY;
        d->frame_count++;
        d->stats.frames_out++;
        if (d->eos_sent)
            d->stats.eos_frames++;
        got_frame = 1;
        break;
    }

    return got_frame;
}

static void dec_drain_internal(struct mpp_dec* d) {
    MppFrame f = NULL;
    for (int n = 0; n < 64; n++) {
        if (d->api->decode_get_frame(d->ctx, &f) != MPP_OK || !f)
            break;
        if (mpp_frame_get_info_change(f))
            handle_info_change(d, f);
        mpp_frame_deinit(&f);
        f = NULL;
    }
}

static int handle_info_change(struct mpp_dec* d, MppFrame frame) {
    RK_U32 width    = mpp_frame_get_width(frame);
    RK_U32 height   = mpp_frame_get_height(frame);
    RK_U32 buf_size = mpp_frame_get_buf_size(frame);

    printf("mpp_dec: info-change: %ux%u buf_size=%u\n", width, height, buf_size);

    MppBufferGroup grp = NULL;
    MPP_RET r = mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION);
    if (r) {
        fprintf(stderr, "mpp_dec: buffer_group_get_internal failed %d\n", r);
        return -EIO;
    }

    r = mpp_buffer_group_limit_config(grp, buf_size, MPP_BUF_COUNT);
    if (r) {
        fprintf(stderr, "mpp_dec: limit_config failed %d\n", r);
        mpp_buffer_group_put(grp);
        return -EIO;
    }

    r = d->api->control(d->ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp);
    if (r) {
        fprintf(stderr, "mpp_dec: SET_EXT_BUF_GROUP failed %d\n", r);
        mpp_buffer_group_put(grp);
        return -EIO;
    }

    if (d->frm_grp) mpp_buffer_group_put(d->frm_grp);
    d->frm_grp = grp;

    r = d->api->control(d->ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
    if (r) {
        fprintf(stderr, "mpp_dec: INFO_CHANGE_READY failed %d\n", r);
        return -EIO;
    }

    d->info_change_done = true;
    printf("mpp_dec: info-change handshake complete (pool=%d)\n", MPP_BUF_COUNT);
    return 0;
}

static int mp4_dist_mpp_feed_whole_au(void) {
    return getenv("MP4_DIST_MPP_WHOLE_AU") ? 1 : 0;
}

static int mp4_dist_mpp_aggressive_pull(void) {
    return getenv("MP4_DIST_MPP_AGGRESSIVE_PULL") ? 1 : 0;
}

/* split_parse: MPP "broken frame" mode — only needed when puts are arbitrary
 * chunks. Evidence (2026-06): tx_wire==rx_br 100% => each ST22 pkt is one AU;
 * mpi_dec_test feeds whole stream without split. Whole-AU path uses split=0. */
static int mp4_dist_mpp_split_parse(void) {
    if (getenv("MP4_DIST_MPP_FORCE_SPLIT"))
        return 1;
    if (getenv("MP4_DIST_NO_SPLIT_PARSE"))
        return 0;
    if (mp4_dist_mpp_feed_whole_au())
        return 0;
    return 1;
}

static int dec_init(void** out, struct codec_config* c) {
    struct mpp_dec* d = calloc(1, sizeof(*d));
    if (!d) return -ENOMEM;
    d->w = c->width;
    d->h = c->height;
    d->feed_whole_au      = mp4_dist_mpp_feed_whole_au();
    d->split_parse        = mp4_dist_mpp_split_parse();
    d->aggressive_pull    = mp4_dist_mpp_aggressive_pull();
    d->eos_sent           = false;
    d->dump_fail_left = 3;
    d->dump_err_left  = 3;
    if (getenv("MP4_DIST_NO_DECODE_DUMP")) {
        d->dump_fail_left = 0;
        d->dump_err_left  = 0;
    }
    if (getenv("MP4_DIST_DUMP_ERR"))
        d->dump_err_left = 3;

    MPP_RET r = mpp_create(&d->ctx, &d->api);
    if (r) { free(d); return -EIO; }

    {
        RK_U32 disable = 0;
        d->api->control(d->ctx, MPP_DEC_SET_ENABLE_DEINTERLACE, &disable);
    }

    r = mpp_init(d->ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
    if (r) { mpp_destroy(d->ctx); free(d); return -EIO; }

    {
        RK_U32 disable_dpb = 1;
        d->api->control(d->ctx, MPP_DEC_SET_DISABLE_DPB_CHECK, &disable_dpb);
    }

    MppDecCfg cfg = NULL;
    mpp_dec_cfg_init(&cfg);
    r = d->api->control(d->ctx, MPP_DEC_GET_CFG, cfg);
    if (!r) {
        RK_U32 split = d->split_parse ? 1 : 0;
        mpp_dec_cfg_set_u32(cfg, "base:split_parse", split);
        r = d->api->control(d->ctx, MPP_DEC_SET_CFG, cfg);
        if (r) fprintf(stderr, "mpp_dec: SET_CFG failed %d\n", r);
    }
    mpp_dec_cfg_deinit(cfg);

    pthread_mutex_init(&d->lock, NULL);

    *out = d;
    mpp_dec_print_config(d);
    return 0;
}

static void dec_free(void* ctx) {
    struct mpp_dec* d = (struct mpp_dec*)ctx;
    if (!d) return;

    pthread_mutex_lock(&d->lock);

    if (!d->eos_sent) {
        MppPacket eos = NULL;
        mpp_packet_init(&eos, NULL, 0);
        mpp_packet_set_eos(eos);
        d->api->decode_put_packet(d->ctx, eos);
        mpp_packet_deinit(&eos);
        d->eos_sent = true;
    }

    for (int t = 0; t < 80; t++) {
        MppFrame f = NULL;
        if (d->api->decode_get_frame(d->ctx, &f) != MPP_OK || !f) {
            msleep(1);
            continue;
        }
        mpp_frame_deinit(&f);
    }

    for (int i = 0; i < FB_DELAY; i++) {
        if (d->held[i]) {
            mpp_frame_deinit(&d->held[i]);
            d->held[i] = NULL;
        }
    }
    d->held_idx = 0;

    if (d->frm_grp) {
        mpp_buffer_group_put(d->frm_grp);
        d->frm_grp = NULL;
    }

    /* mpp_destroy() tears down the context; api->reset() after buffer-group
     * release triggers mpp_dec_notify_normal() SIGSEGV on Rockchip MPP 1.3.x. */
    MppCtx mpp = d->ctx;
    d->ctx = NULL;
    d->api = NULL;

    pthread_mutex_unlock(&d->lock);
    pthread_mutex_destroy(&d->lock);

    if (mpp)
        mpp_destroy(mpp);

    free(d->last_au);
    free(d);
}

static int dec_start(void* ctx) { (void)ctx; return 0; }
static void dec_stop(void* ctx) { (void)ctx; }

int mpp_dec_put_au(void* dec_priv, uint8_t* data, size_t size) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (!d || !data || !size) return -EINVAL;

    pthread_mutex_lock(&d->lock);
    d->stats.au_in++;
    remember_au(d, data, size);

    size_t offset = 0;
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > FEED_CHUNK_SIZE) chunk = FEED_CHUNK_SIZE;

        if (dec_put_packet(d, data + offset, chunk) < 0) {
            d->stats.au_put_fail++;
            if (d->dump_fail_left > 0) {
                maybe_dump_fail_au(d, data, size);
                d->dump_fail_left--;
            }
            pthread_mutex_unlock(&d->lock);
            return -EIO;
        }
        offset += chunk;
    }
    pthread_mutex_unlock(&d->lock);
    return 0;
}

void mpp_dec_mark_au(void* dec_priv, const uint8_t* data, size_t size) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    d->stats.au_in++;
    remember_au(d, data, size);
    pthread_mutex_unlock(&d->lock);
}

void mpp_dec_note_no_output(void* dec_priv) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (d) d->stats.au_no_output++;
}

int mpp_dec_send_eos(void* dec_priv) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (!d) return -EINVAL;

    pthread_mutex_lock(&d->lock);
    if (d->eos_sent) {
        pthread_mutex_unlock(&d->lock);
        return 0;
    }

    MppPacket eos = NULL;
    mpp_packet_init(&eos, NULL, 0);
    mpp_packet_set_eos(eos);
    MPP_RET r = d->api->decode_put_packet(d->ctx, eos);
    mpp_packet_deinit(&eos);
    if (r != MPP_OK) {
        pthread_mutex_unlock(&d->lock);
        return -EIO;
    }
    d->eos_sent = true;
    pthread_mutex_unlock(&d->lock);
    return 0;
}

int mpp_dec_reset(void* dec_priv) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (!d) return -EINVAL;

    pthread_mutex_lock(&d->lock);

    for (int i = 0; i < FB_DELAY; i++)
        d->held[i] = NULL;
    d->held_idx = 0;

    dec_drain_internal(d);

    /* Avoid api->reset() — same SIGSEGV as dec_free on MPP 1.3.x after buf release.
     * Drain + drop held refs is enough to resync; next IDR triggers info_change. */
    d->info_change_done = false;
    d->stats.reset_count++;
    printf("mpp_dec: soft reset (#%llu)\n",
           (unsigned long long)d->stats.reset_count);

    pthread_mutex_unlock(&d->lock);
    return 0;
}

int mpp_dec_put_chunk(void* dec_priv, uint8_t* data, size_t size) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (!d || !data || !size) return -EINVAL;

    pthread_mutex_lock(&d->lock);
    int ret = dec_put_packet(d, data, size);
    pthread_mutex_unlock(&d->lock);
    return ret;
}

int mpp_dec_put_whole(void* dec_priv, uint8_t* data, size_t size) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (!d || !data || !size) return -EINVAL;

    pthread_mutex_lock(&d->lock);
    d->stats.au_in++;
    d->stats.pkt_in++;
    remember_au(d, data, size);

    if (dec_put_packet(d, data, size) < 0) {
        d->stats.au_put_fail++;
        if (d->dump_fail_left > 0) {
            maybe_dump_fail_au(d, data, size);
            d->dump_fail_left--;
        }
        pthread_mutex_unlock(&d->lock);
        return -EIO;
    }
    pthread_mutex_unlock(&d->lock);
    return 0;
}

int mpp_dec_put(void* dec_priv, uint8_t* data, size_t size) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (!d || !data || !size) return -EINVAL;

    pthread_mutex_lock(&d->lock);
    d->stats.pkt_in++;

    size_t offset = 0;
    int ret = 0;
    while (offset < size) {
        size_t chunk = size - offset;
        if (chunk > FEED_CHUNK_SIZE) chunk = FEED_CHUNK_SIZE;

        if (dec_put_packet(d, data + offset, chunk) < 0) {
            ret = -EIO;
            break;
        }
        offset += chunk;
    }
    pthread_mutex_unlock(&d->lock);
    return ret;
}

int mpp_dec_pull(void* dec_priv, int* fd_out, uint32_t* hor_stride_out,
                 uint32_t* ver_stride_out) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (!d || !fd_out) return -EINVAL;

    pthread_mutex_lock(&d->lock);
    int ret = 0;
    if (drain_pending_pop(d, fd_out, hor_stride_out, ver_stride_out))
        ret = 1;
    else
        ret = dec_get_frame(d, fd_out, hor_stride_out, ver_stride_out, true) ? 1 : 0;
    pthread_mutex_unlock(&d->lock);
    return ret;
}

int mpp_dec_feed_direct(void* dec_priv, uint8_t* data, size_t size, int* fd_out,
                        uint32_t* hor_stride_out, uint32_t* ver_stride_out) {
    int r = mpp_dec_put_au(dec_priv, data, size);
    if (r < 0) return r;
    return mpp_dec_pull(dec_priv, fd_out, hor_stride_out, ver_stride_out);
}

int mpp_dec_feed(void* dec_priv, uint8_t* data, size_t size, int* fd_out,
                 uint32_t* hor_stride_out, uint32_t* ver_stride_out) {
    return mpp_dec_feed_direct(dec_priv, data, size, fd_out, hor_stride_out, ver_stride_out);
}

void mpp_dec_get_stats(void* dec_priv, mpp_dec_stats* out) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (!d || !out) return;
    *out = d->stats;
}

void mpp_dec_print_stats(void* dec_priv) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (!d) return;

    const mpp_dec_stats* s = &d->stats;
    uint64_t fed = s->pkt_in ? s->pkt_in : s->au_in;

    printf("mpp_dec stats: pkt_in=%llu au_in=%llu au_put_fail=%llu au_no_out=%llu "
           "put_ok=%llu put_fail=%llu "
           "frames_out=%llu discard=%llu err=%llu no_buf=%llu drain=%llu "
           "eos_frames=%llu reset=%llu\n",
           (unsigned long long)s->pkt_in,
           (unsigned long long)s->au_in,
           (unsigned long long)s->au_put_fail,
           (unsigned long long)s->au_no_output,
           (unsigned long long)s->put_ok,
           (unsigned long long)s->put_fail,
           (unsigned long long)s->frames_out,
           (unsigned long long)s->discard,
           (unsigned long long)s->err_info,
           (unsigned long long)s->no_buffer,
           (unsigned long long)s->drain_pull,
           (unsigned long long)s->eos_frames,
           (unsigned long long)s->reset_count);
    (void)fed;
}

void mpp_dec_print_config(void* dec_priv) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    if (!d) return;

    const char* feed = d->feed_whole_au
        ? "whole_au (1 put/ST22 pkt, MP4_DIST_MPP_WHOLE_AU=1)"
        : "legacy_8k (mpp_dec_put 8KB chunks, default)";
    printf("mpp_dec: %dx%d pool=%d split_parse=%d feed=%s%s\n",
           d->w, d->h, MPP_BUF_COUNT, d->split_parse, feed,
           d->aggressive_pull ? " aggressive_pull=on" : "");
    if (d->aggressive_pull)
        printf("mpp_dec: aggressive_pull: idle retry after each AU + EOS flush "
               "(MP4_DIST_MPP_AGGRESSIVE_PULL=1)\n");
}

int mpp_dec_uses_whole_au(void* dec_priv) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    return d && d->feed_whole_au;
}

int mpp_dec_uses_aggressive_pull(void* dec_priv) {
    struct mpp_dec* d = (struct mpp_dec*)dec_priv;
    return d && d->aggressive_pull;
}

static int dw_send(void* ctx, int dma_fd, uint32_t stride)
    { (void)ctx; (void)dma_fd; (void)stride; return -ENOSYS; }

static int dw_recv(void* ctx, uint8_t** data, size_t* size, int* is_key)
    { (void)ctx; (void)data; (void)size; (void)is_key; return -ENOSYS; }

struct codec_plugin* mpp_decoder_create(struct codec_config* cfg) {
    struct codec_plugin* p = calloc(1, sizeof(*p));
    struct mpp_dec* d = NULL;
    if (!p || dec_init((void**)&d, cfg)) { free(p); return NULL; }
    p->priv  = d;
    p->name  = "mpp_decoder";
    p->init  = dec_init;
    p->start = dec_start;
    p->stop  = dec_stop;
    p->free  = dec_free;
    p->send  = dw_send;
    p->recv  = dw_recv;
    p->caps  = (struct codec_caps){ .fmts={CODEC_FMT_H264}, .min_w=64, .max_w=4096,
        .min_h=64, .max_h=2160, .supports_dmabuf=true };
    return p;
}
