/* SPDX-License-Identifier: BSD-3-Clause
 *
 * MPP H.264 hardware decoder (RK3588 VPU).
 * Input: H.264 Annex B coded packets
 * Output: NV12 dma-buf frames
 */

#ifndef _E2E_MPP_DECODER_H_
#define _E2E_MPP_DECODER_H_

#include "codec_plugin.h"
#include <stddef.h>
#include <stdint.h>

struct codec_plugin* mpp_decoder_create(struct codec_config* cfg);

/** Runtime counters for decode-path diagnostics. */
typedef struct mpp_dec_stats {
    uint64_t pkt_in;       /* decode_put_packet calls (legacy: per 8KB chunk) */
    uint64_t au_in;        /* access units fed (one per ST22 packet in whole mode) */
    uint64_t au_put_fail;  /* whole AU rejected after retries */
    uint64_t au_no_output; /* AUs that produced zero displayable frames */
    uint64_t put_ok;       /* successful decode_put_packet calls */
    uint64_t put_fail;     /* failed decode_put_packet calls */
    uint64_t frames_out;   /* displayable frames returned to caller */
    uint64_t discard;      /* MPP discard flag */
    uint64_t err_info;     /* MPP errinfo flag */
    uint64_t no_buffer;    /* frame without dma buffer */
    uint64_t drain_pull;   /* frames drained during put backpressure (queued for pull) */
    uint64_t eos_frames;   /* frames after EOS flush at shutdown */
    uint64_t reset_count;  /* decoder resets */
} mpp_dec_stats;

int mpp_dec_feed(void* dec_priv, uint8_t* data, size_t size, int* fd_out,
                 uint32_t* hor_stride_out, uint32_t* ver_stride_out);

int mpp_dec_feed_direct(void* dec_priv, uint8_t* data, size_t size, int* fd_out,
                        uint32_t* hor_stride_out, uint32_t* ver_stride_out);

/** One ST22 access unit → single decode_put_packet (no 8KB split). */
int mpp_dec_put_whole(void* dec_priv, uint8_t* data, size_t size);

/** Feed one complete access unit (chunked internally). Returns 0 on success. */
int mpp_dec_put_au(void* dec_priv, uint8_t* data, size_t size);

/** Mark start of AU for stats/dump (use with chunked mpp_dec_put). */
void mpp_dec_mark_au(void* dec_priv, const uint8_t* data, size_t size);

void mpp_dec_note_no_output(void* dec_priv);

int mpp_dec_put(void* dec_priv, uint8_t* data, size_t size);

/** Feed one chunk without AU accounting (pair with mpp_dec_mark_au + mpp_dec_pull). */
int mpp_dec_put_chunk(void* dec_priv, uint8_t* data, size_t size);

int mpp_dec_pull(void* dec_priv, int* fd_out, uint32_t* hor_stride_out,
                 uint32_t* ver_stride_out);

void mpp_dec_get_stats(void* dec_priv, mpp_dec_stats* out);
void mpp_dec_print_stats(void* dec_priv);

/** Human-readable feed/split config (set at init from env). */
void mpp_dec_print_config(void* dec_priv);

/** 1 if MP4_DIST_MPP_WHOLE_AU was set at decoder init. */
int mpp_dec_uses_whole_au(void* dec_priv);

/** 1 if MP4_DIST_MPP_AGGRESSIVE_PULL was set at decoder init. */
int mpp_dec_uses_aggressive_pull(void* dec_priv);

/** Send EOS to MPP (idempotent). Used before final drain. */
int mpp_dec_send_eos(void* dec_priv);

#define MPP_DEC_AGGRESSIVE_IDLE_ROUNDS 4
#define MPP_DEC_EOS_FLUSH_IDLE_ROUNDS 40

/** Flush decoder state after sync loss; wait for next IDR. */
int mpp_dec_reset(void* dec_priv);

#endif
