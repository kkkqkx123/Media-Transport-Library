#ifndef _LIVE_INGEST_MPP_DECODER_H_
#define _LIVE_INGEST_MPP_DECODER_H_

#include "codec_plugin.h"
#include <stdint.h>

struct codec_plugin* mpp_decoder_create(struct codec_config* cfg);

/* Feed H.264 data (Annex B without start-code prefix), get decoded NV12 dma-buf fd.
 * Returns: 1 = frame ready, 0 = consumed but no output yet, <0 = error */
int mpp_dec_feed(void* dec_priv, uint8_t* data, size_t size, int* fd_out,
                 uint32_t* hor_stride_out, uint32_t* ver_stride_out);

/** Stream coded data into decoder (chunked for split_parse). Returns 0 on success. */
int mpp_dec_put(void* dec_priv, uint8_t* data, size_t size);

/** Pull one decoded NV12 dma-buf frame. Returns 1=frame, 0=none, <0=error. */
int mpp_dec_pull(void* dec_priv, int* fd_out, uint32_t* hor_stride_out,
                 uint32_t* ver_stride_out);

/** Supervision snapshot: input-full drops, take-frame EMA (us), last output (ns). */
void mpp_dec_get_stats(void* dec_priv, uint64_t* input_full_drops,
                       uint64_t* ema_us, uint64_t* last_output_ns);

/** Take a pending info-change resolution (1 = changed, sizes stored to out). */
int mpp_dec_take_resolution(void* dec_priv, int* w_out, int* h_out);

#endif
