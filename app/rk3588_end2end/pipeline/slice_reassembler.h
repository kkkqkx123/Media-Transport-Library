/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * H.264 Slice-level reassembler. Concatenates multiple slices of a single
 * picture into a complete frame buffer.
 *
 * Only activated when codec.num_slices > 1. For frame-level encoding
 * (slices=1) this module is a transparent no-op.
 *
 * Usage (in Decode Thread):
 *   if (num_slices > 1) {
 *       slice_reasm_feed(reasm, pkt, &frame, &frame_size);
 *       if (frame) mpp_dec_feed(dec, frame, frame_size);
 *   } else {
 *       mpp_dec_feed(dec, pkt.data, pkt.size);
 *   }
 */

#ifndef _E2E_SLICE_REASSEMBLER_H_
#define _E2E_SLICE_REASSEMBLER_H_

#include <stddef.h>
#include <stdint.h>

struct slice_reassembler;

/** Create a reassembler for one picture. */
struct slice_reassembler* slice_reasm_create(void);
void slice_reasm_free(struct slice_reassembler* r);

/**
 * Feed a coded slice (Annex B format with start-code prefix).
 * When all slices for a picture have been received, returns the complete
 * frame via frame_out/frame_size_out. The caller owns the returned buffer
 * (allocated with malloc).
 *
 * @param data   Annex B coded slice data (with 00 00 00 01 prefix)
 * @param size   size of data in bytes
 * @param frame_out [out] complete frame (NULL if picture not yet complete)
 * @param frame_size_out [out] size of complete frame
 * @return 1 if a complete frame is ready, 0 if waiting for more slices,
 *         -1 on error.
 */
int slice_reasm_feed(struct slice_reassembler* r,
                     uint8_t* data, size_t size,
                     uint8_t** frame_out, size_t* frame_size_out);

/**
 * Reset internal state for a new picture. Call after frame_out is consumed.
 */
void slice_reasm_reset(struct slice_reassembler* r);

#endif /* _E2E_SLICE_REASSEMBLER_H_ */
