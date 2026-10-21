/* SPDX-License-Identifier: BSD-3-Clause
 *
 * MPP H.264 hardware decoder (RK3588 VPU).
 * Input: H.264 Annex B coded packets
 * Output: NV12 dma-buf frames
 */

#ifndef _E2E_MPP_DECODER_H_
#define _E2E_MPP_DECODER_H_

#include "codec_plugin.h"

struct codec_plugin* mpp_decoder_create(struct codec_config* cfg);

/* Feed H.264 data (Annex B without start-code prefix), get decoded NV12 dma-buf fd.
 * Returns: 1 = frame ready, 0 = consumed but no output yet, <0 = error */
int mpp_dec_feed(void* dec_priv, uint8_t* data, size_t size, int* fd_out,
                 uint32_t* hor_stride_out, uint32_t* ver_stride_out);

/** Feed entire access unit in one call (lower latency, for MTL zero-copy path). */
int mpp_dec_feed_direct(void* dec_priv, uint8_t* data, size_t size, int* fd_out,
                        uint32_t* hor_stride_out, uint32_t* ver_stride_out);

/** Stream coded data into decoder (chunked for split_parse). Returns 0 on success. */
int mpp_dec_put(void* dec_priv, uint8_t* data, size_t size);

/** Pull one decoded NV12 dma-buf frame. Returns 1=frame, 0=none, <0=error. */
int mpp_dec_pull(void* dec_priv, int* fd_out, uint32_t* hor_stride_out,
                 uint32_t* ver_stride_out);

#endif
