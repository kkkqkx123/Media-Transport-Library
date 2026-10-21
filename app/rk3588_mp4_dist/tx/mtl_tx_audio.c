#define _GNU_SOURCE
/* SPDX-License-Identifier: BSD-3-Clause
 *
 * ST30 TX session — E2E-style framebuff state machine for 1ms PCM frames.
 */

#include "mtl_tx_audio.h"
#include "../shared/tx_tai_ts.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum fb_state {
    FB_FREE = 0,
    FB_FILLING,
    FB_READY,
    FB_SENDING,
};

struct mtl_tx_audio {
    struct mtl_tx_audio_params cfg;
    st30_tx_handle             handle;
    tx_tai_ts                  tai;
    size_t                     frame_bytes;

    int           fb_cnt;
    void**        fb_addr;
    atomic_int*   fb_states;

    atomic_bool   running;
    pthread_mutex_t fb_mutex;
    pthread_cond_t  fb_free;
};

static int tx_get_next_frame(void* priv, uint16_t* next_frame_idx,
                             struct st30_tx_frame_meta* meta) {
    struct mtl_tx_audio* tx = priv;

    for (int i = 0; i < tx->fb_cnt; i++) {
        int expected = FB_READY;
        if (atomic_compare_exchange_strong(&tx->fb_states[i], &expected, FB_SENDING)) {
            *next_frame_idx = (uint16_t)i;
            {
                uint64_t mtl_ts = meta->timestamp;
                tx_tai_ts_fill(&tx->tai, &meta->tfmt, &meta->timestamp, meta->epoch,
                               mtl_ts);
            }
            return 0;
        }
    }
    return -EAGAIN;
}

static int tx_notify_frame_done(void* priv, uint16_t frame_idx,
                                struct st30_tx_frame_meta* meta) {
    struct mtl_tx_audio* tx = priv;
    (void)meta;
    atomic_store(&tx->fb_states[frame_idx], FB_FREE);
    pthread_mutex_lock(&tx->fb_mutex);
    pthread_cond_signal(&tx->fb_free);
    pthread_mutex_unlock(&tx->fb_mutex);
    return 0;
}

static void tx_signal_fb_free(struct mtl_tx_audio* tx) {
    pthread_mutex_lock(&tx->fb_mutex);
    pthread_cond_signal(&tx->fb_free);
    pthread_mutex_unlock(&tx->fb_mutex);
}

size_t mtl_tx_audio_frame_bytes(const struct mtl_tx_audio* tx) {
    return tx ? tx->frame_bytes : 0;
}

struct mtl_tx_audio* mtl_tx_audio_create(const struct mtl_tx_audio_params* p) {
    struct mtl_tx_audio* tx = calloc(1, sizeof(*tx));
    if (!tx) return NULL;
    tx->cfg = *p;
    tx->fb_cnt = p->framebuff_cnt > 0 ? p->framebuff_cnt : 32;

    tx_tai_ts_init(&tx->tai, p->st, MP4_ST30_FRAME_TIME_NS, p->use_tai_timestamp);

    tx->fb_addr   = calloc((size_t)tx->fb_cnt, sizeof(void*));
    tx->fb_states = calloc((size_t)tx->fb_cnt, sizeof(atomic_int));
    if (!tx->fb_addr || !tx->fb_states) goto fail;

    for (int i = 0; i < tx->fb_cnt; i++)
        atomic_init(&tx->fb_states[i], FB_FREE);
    atomic_init(&tx->running, false);
    pthread_mutex_init(&tx->fb_mutex, NULL);
    pthread_cond_init(&tx->fb_free, NULL);

    uint32_t fb_size = (uint32_t)st30_calculate_framebuff_size(
        p->fmt, p->ptime, p->sampling, p->channel,
        MP4_ST30_FRAME_TIME_NS, NULL);
    tx->frame_bytes = fb_size;

    struct st30_tx_ops ops = {0};
    ops.name               = "mp4_dist_audio_tx";
    ops.priv               = tx;
    ops.type               = ST30_TYPE_FRAME_LEVEL;
    ops.fmt                = p->fmt;
    ops.channel            = p->channel;
    ops.sampling           = p->sampling;
    ops.ptime              = p->ptime;
    ops.num_port           = 1;
    if (p->use_tai_timestamp)
        ops.flags |= ST30_TX_FLAG_USER_TIMESTAMP | ST30_TX_FLAG_USER_PACING;
    ops.framebuff_cnt      = (uint16_t)tx->fb_cnt;
    ops.framebuff_size     = fb_size;
    ops.get_next_frame     = tx_get_next_frame;
    ops.notify_frame_done  = tx_notify_frame_done;
    ops.payload_type       = p->payload_type ? p->payload_type : 98;
    {
        unsigned int a, b, c, d;
        if (p->dip_addr &&
            sscanf(p->dip_addr, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            ops.dip_addr[0][0] = (uint8_t)a;
            ops.dip_addr[0][1] = (uint8_t)b;
            ops.dip_addr[0][2] = (uint8_t)c;
            ops.dip_addr[0][3] = (uint8_t)d;
        }
    }
    ops.udp_port[0] = p->udp_port;
    if (p->port_name)
        strncpy(ops.port[0], p->port_name, MTL_PORT_MAX_LEN - 1);

    tx->handle = st30_tx_create(p->st, &ops);
    if (!tx->handle) {
        fprintf(stderr, "mtl_tx_audio: st30_tx_create failed\n");
        goto fail;
    }

    for (int i = 0; i < tx->fb_cnt; i++)
        tx->fb_addr[i] = st30_tx_get_framebuffer(tx->handle, (uint16_t)i);

    printf("mtl_tx_audio: %d fbs x %zu B (1ms wire pacing)%s\n",
           tx->fb_cnt, tx->frame_bytes,
           p->use_tai_timestamp ? " tai-ts" : "");
    return tx;

fail:
    mtl_tx_audio_free(tx);
    return NULL;
}

void mtl_tx_audio_free(struct mtl_tx_audio* tx) {
    if (!tx) return;
    mtl_tx_audio_stop(tx);
    pthread_mutex_destroy(&tx->fb_mutex);
    pthread_cond_destroy(&tx->fb_free);
    if (tx->handle) st30_tx_free(tx->handle);
    free(tx->fb_addr);
    free(tx->fb_states);
    free(tx);
}

int mtl_tx_audio_start(struct mtl_tx_audio* tx) {
    if (!tx) return -EINVAL;
    atomic_store(&tx->running, true);
    return 0;
}

void mtl_tx_audio_stop(struct mtl_tx_audio* tx) {
    if (!tx) return;
    atomic_store(&tx->running, false);
    pthread_mutex_lock(&tx->fb_mutex);
    pthread_cond_broadcast(&tx->fb_free);
    pthread_mutex_unlock(&tx->fb_mutex);
}

int mtl_tx_audio_acquire_fb(struct mtl_tx_audio* tx, uint16_t* idx, void** addr) {
    if (!tx || !idx || !addr) return -EINVAL;

    while (atomic_load(&tx->running)) {
        for (int i = 0; i < tx->fb_cnt; i++) {
            int expected = FB_FREE;
            if (atomic_compare_exchange_strong(&tx->fb_states[i], &expected, FB_FILLING)) {
                *idx  = (uint16_t)i;
                *addr = tx->fb_addr[i];
                return 0;
            }
        }
        pthread_mutex_lock(&tx->fb_mutex);
        if (atomic_load(&tx->running))
            pthread_cond_wait(&tx->fb_free, &tx->fb_mutex);
        pthread_mutex_unlock(&tx->fb_mutex);
    }
    return -EAGAIN;
}

int mtl_tx_audio_publish_fb(struct mtl_tx_audio* tx, uint16_t idx, size_t size) {
    if (!tx || idx >= (uint16_t)tx->fb_cnt) return -EINVAL;
    if (size != tx->frame_bytes) return -EINVAL;
    if (atomic_load(&tx->fb_states[idx]) != FB_FILLING) return -EINVAL;

    atomic_store(&tx->fb_states[idx], FB_READY);
    return 0;
}

void mtl_tx_audio_abort_fb(struct mtl_tx_audio* tx, uint16_t idx) {
    if (!tx || idx >= (uint16_t)tx->fb_cnt) return;
    atomic_store(&tx->fb_states[idx], FB_FREE);
    tx_signal_fb_free(tx);
}

st30_tx_handle mtl_tx_audio_handle(struct mtl_tx_audio* tx) {
    return tx ? tx->handle : NULL;
}
