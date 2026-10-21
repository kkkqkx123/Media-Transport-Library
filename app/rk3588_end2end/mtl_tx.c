#define _GNU_SOURCE
/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "mtl_tx.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <st_api.h>

enum fb_state {
    FB_FREE = 0,
    FB_FILLING,
    FB_READY,
    FB_SENDING,
};

struct mtl_tx {
    struct mtl_tx_params cfg;
    st22_tx_handle       handle;

    int                  fb_cnt;
    void**               fb_addr;
    size_t*              fb_sizes;
    atomic_int*          fb_states;

    atomic_bool          running;

    pthread_mutex_t      fb_mutex;
    pthread_cond_t       fb_free;
};

static int tx_get_next_frame(void* priv, uint16_t* next_frame_idx,
                             struct st22_tx_frame_meta* meta) {
    struct mtl_tx* tx = (struct mtl_tx*)priv;

    for (int i = 0; i < tx->fb_cnt; i++) {
        int expected = FB_READY;
        if (atomic_compare_exchange_strong(&tx->fb_states[i], &expected, FB_SENDING)) {
            *next_frame_idx = (uint16_t)i;
            meta->codestream_size = tx->fb_sizes[i];
            return 0;
        }
    }
    return -EAGAIN;
}

static int tx_notify_frame_done(void* priv, uint16_t frame_idx,
                                struct st22_tx_frame_meta* meta) {
    struct mtl_tx* tx = (struct mtl_tx*)priv;
    (void)meta;
    atomic_store(&tx->fb_states[frame_idx], FB_FREE);
    pthread_mutex_lock(&tx->fb_mutex);
    pthread_cond_signal(&tx->fb_free);
    pthread_mutex_unlock(&tx->fb_mutex);
    return 0;
}

static void tx_signal_fb_free(struct mtl_tx* tx) {
    pthread_mutex_lock(&tx->fb_mutex);
    pthread_cond_signal(&tx->fb_free);
    pthread_mutex_unlock(&tx->fb_mutex);
}

struct mtl_tx* mtl_tx_create(struct mtl_tx_params* p) {
    struct mtl_tx* tx = calloc(1, sizeof(*tx));
    if (!tx) return NULL;
    tx->cfg = *p;
    tx->fb_cnt = p->framebuff_cnt;

    tx->fb_addr   = calloc((size_t)tx->fb_cnt, sizeof(void*));
    tx->fb_sizes  = calloc((size_t)tx->fb_cnt, sizeof(size_t));
    tx->fb_states = calloc((size_t)tx->fb_cnt, sizeof(atomic_int));
    if (!tx->fb_addr || !tx->fb_sizes || !tx->fb_states) goto fail;

    for (int i = 0; i < tx->fb_cnt; i++)
        atomic_init(&tx->fb_states[i], FB_FREE);
    atomic_init(&tx->running, false);
    pthread_mutex_init(&tx->fb_mutex, NULL);
    pthread_cond_init(&tx->fb_free, NULL);

    struct st22_tx_ops ops = {0};
    ops.name               = "e2e_tx";
    ops.priv               = tx;
    ops.type               = ST22_TYPE_FRAME_LEVEL;
    ops.fps                = st_frame_rate_to_st_fps((double)p->fps);
    ops.width              = (uint32_t)(p->width > 0 ? p->width : 1920);
    ops.height             = (uint32_t)(p->height > 0 ? p->height : 1080);
    ops.fmt                = ST20_FMT_YUV_420_8BIT;
    ops.pack_type          = ST22_PACK_CODESTREAM;
    ops.num_port           = 1;
    ops.flags |= ST22_TX_FLAG_DISABLE_BOXES;
    ops.framebuff_cnt      = (uint16_t)tx->fb_cnt;
    ops.framebuff_max_size = p->framebuff_max;
    ops.get_next_frame     = tx_get_next_frame;
    ops.notify_frame_done  = tx_notify_frame_done;
    ops.payload_type       = 112;
    {
        unsigned int a, b, c, d;
        if (sscanf(p->dip_addr, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
            ops.dip_addr[0][0] = (uint8_t)a;
            ops.dip_addr[0][1] = (uint8_t)b;
            ops.dip_addr[0][2] = (uint8_t)c;
            ops.dip_addr[0][3] = (uint8_t)d;
        }
    }
    ops.udp_port[0] = p->udp_port;
    strncpy(ops.port[0], p->port_name, MTL_PORT_MAX_LEN - 1);

    tx->handle = st22_tx_create(p->st, &ops);
    if (!tx->handle) { fprintf(stderr, "mtl_tx: st22_tx_create failed\n"); goto fail; }

    for (int i = 0; i < tx->fb_cnt; i++)
        tx->fb_addr[i] = st22_tx_get_fb_addr(tx->handle, (uint16_t)i);

    printf("mtl_tx: %d fbs x %zu bytes, %.0ffps ready (direct-write)\n",
           tx->fb_cnt, p->framebuff_max, (double)p->fps);
    return tx;

fail:
    mtl_tx_free(tx);
    return NULL;
}

void mtl_tx_free(struct mtl_tx* tx) {
    if (!tx) return;
    mtl_tx_stop(tx);
    pthread_mutex_destroy(&tx->fb_mutex);
    pthread_cond_destroy(&tx->fb_free);
    if (tx->handle) st22_tx_free(tx->handle);
    free(tx->fb_addr);
    free(tx->fb_sizes);
    free(tx->fb_states);
    free(tx);
}

int mtl_tx_start(struct mtl_tx* tx) {
    if (!tx) return -1;
    atomic_store(&tx->running, true);
    return 0;
}

void mtl_tx_stop(struct mtl_tx* tx) {
    if (!tx) return;
    atomic_store(&tx->running, false);
    pthread_mutex_lock(&tx->fb_mutex);
    pthread_cond_broadcast(&tx->fb_free);
    pthread_mutex_unlock(&tx->fb_mutex);
}

int mtl_tx_acquire_fb(struct mtl_tx* tx, uint16_t* idx, void** addr) {
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

int mtl_tx_publish_fb(struct mtl_tx* tx, uint16_t idx, size_t size) {
    if (!tx || idx >= (uint16_t)tx->fb_cnt) return -EINVAL;
    if (!size || size > tx->cfg.framebuff_max) return -EINVAL;
    if (atomic_load(&tx->fb_states[idx]) != FB_FILLING) return -EINVAL;

    tx->fb_sizes[idx] = size;
    atomic_store(&tx->fb_states[idx], FB_READY);
    return 0;
}

void mtl_tx_abort_fb(struct mtl_tx* tx, uint16_t idx) {
    if (!tx || idx >= (uint16_t)tx->fb_cnt) return;
    atomic_store(&tx->fb_states[idx], FB_FREE);
    tx_signal_fb_free(tx);
}

st22_tx_handle mtl_tx_handle(struct mtl_tx* tx) {
    return tx ? tx->handle : NULL;
}
