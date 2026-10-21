/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * MTL ST22 TX wrapper: framebuff pool + lcore callbacks.
 *
 * Encode thread writes codestream directly into MTL hugepage framebuffs
 * (acquire → fill → publish). No fill thread or intermediate slab copy.
 *
 * State machine (lock-free atomics, lcore-safe):
 *   FREE →(encode thread) FILLING → READY →(get_next_frame) SENDING →(notify_done) FREE
 */

#ifndef _E2E_MTL_TX_H_
#define _E2E_MTL_TX_H_

#include <stddef.h>
#include <stdint.h>
#include <mtl_api.h>
#include <st20_api.h>

struct mtl_tx;

struct mtl_tx_params {
    mtl_handle   st;
    int          framebuff_cnt;
    size_t       framebuff_max;
    int          width, height;
    float        fps;
    uint16_t     udp_port;
    const char*  dip_addr;
    const char*  port_name;
};

struct mtl_tx* mtl_tx_create(struct mtl_tx_params* p);
void mtl_tx_free(struct mtl_tx* tx);

/** Mark session active (encode thread may acquire framebuffs). */
int  mtl_tx_start(struct mtl_tx* tx);
void mtl_tx_stop(struct mtl_tx* tx);

/**
 * Acquire a FREE framebuff for encoding (blocks briefly until available).
 * Marks buffer FILLING; caller writes codestream then calls mtl_tx_publish_fb.
 */
int mtl_tx_acquire_fb(struct mtl_tx* tx, uint16_t* idx, void** addr);

/** Publish filled buffer to MTL lcore (FILLING → READY). */
int mtl_tx_publish_fb(struct mtl_tx* tx, uint16_t idx, size_t size);

/** Return buffer to pool on encode error (FILLING → FREE). */
void mtl_tx_abort_fb(struct mtl_tx* tx, uint16_t idx);

st22_tx_handle mtl_tx_handle(struct mtl_tx* tx);

#endif
