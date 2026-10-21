/* SPDX-License-Identifier: BSD-3-Clause
 *
 * In-band E2E capture timestamp prefix on ST22 codestream (TX → RX).
 * Prepended before H.264 Annex B; stripped in mtl_rx_bridge before decode.
 */

#ifndef _E2E_STAMP_H_
#define _E2E_STAMP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define E2E_STAMP_MAGIC 0x3245314DUL /* "M1E2" */

/** Wire header size in bytes (magic + version + capture_tai_ns). */
size_t e2e_stamp_wire_size(void);

/** Write stamp at @buf; @buf must hold at least e2e_stamp_wire_size() bytes. */
void e2e_stamp_write(void* buf, uint64_t capture_tai_ns);

/**
 * Parse stamp at start of @data.
 * On success: *capture_tai_out set, payload starts at *payload_off_out with
 * *payload_size_out bytes. On failure: payload is full @size, capture_tai 0.
 * @return true when magic matches.
 */
bool e2e_stamp_try_parse(const uint8_t* data, size_t size,
                         uint64_t* capture_tai_out, size_t* payload_off_out,
                         size_t* payload_size_out);

#endif /* _E2E_STAMP_H_ */
