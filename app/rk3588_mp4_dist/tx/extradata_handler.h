/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Extradata handler — SPS/PPS extraction and IDR-frame prepending.
 * Ensures every IDR frame carries codec configuration for RX decoder init.
 */

#ifndef _EXTRADATA_HANDLER_H_
#define _EXTRADATA_HANDLER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Opaque handler context. */
typedef struct extradata_handler extradata_handler;

/**
 * Create handler from raw extradata bytes (typically from AVCodecParameters).
 * @param data  extradata bytes (SPS+PPS in Annex B format)
 * @param size  byte count
 * @return handler, or NULL on failure
 */
extradata_handler* extradata_handler_create(const uint8_t* data, size_t size);

/**
 * Check if this packet is an IDR (key) frame that needs extradata prepended.
 * For H.264, checks for IDR NAL type (nal_unit_type == 5).
 *
 * @param data  Annex B coded data
 * @param size  byte count
 * @return true if IDR
 */
bool extradata_is_idr(const uint8_t* data, size_t size);

/**
 * Build a combined buffer: [extradata ... | frame data ...].
 * Caller must free the returned buffer with free().
 *
 * @param h         handler
 * @param frame_data  original frame bytes
 * @param frame_size  original frame byte count
 * @param out_size    [out] combined size
 * @return malloc'd combined buffer, or NULL on failure
 */
uint8_t* extradata_splice_idr(extradata_handler* h,
                              const uint8_t* frame_data, size_t frame_size,
                              size_t* out_size);

/** Get the raw extradata pointer and size. */
const uint8_t* extradata_get_data(const extradata_handler* h);
size_t extradata_get_size(const extradata_handler* h);

/** Free the handler. */
void extradata_handler_free(extradata_handler* h);

#endif /* _EXTRADATA_HANDLER_H_ */
