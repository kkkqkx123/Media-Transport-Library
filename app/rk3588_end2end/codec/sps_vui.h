/* SPDX-License-Identifier: BSD-3-Clause
 * H.264 SPS VUI injection (Rec.709 full range) — pure bitstream level,
 * no hardware dependency. Port of zlmediakit HdmiInSpsVui to C.
 *
 * MPP 1.5.0 cannot configure h264:vui_* keys; the encoded SPS carries no VUI,
 * so receivers interpret full-range BT.709 data as BT.601 limited → color shift.
 * This module locates the SPS (NAL type 7) inside an Annex-B packet and
 * injects/rewrites the VUI colour signalling. On any parse failure the packet
 * is left untouched (return < 0, out unchanged).
 */

#ifndef RK3588_SPS_VUI_H
#define RK3588_SPS_VUI_H

#include <stddef.h>
#include <stdint.h>

/* Scan an Annex-B packet for the SPS NAL and inject VUI colour info
 * (full_range=1, primaries/transfer/matrix=1 i.e. BT.709).
 *
 * data/in:  input packet (Annex-B, may contain SPS+PPS+IDR).
 * out:      caller buffer, capacity out_cap.
 * Returns:  new total size on success, -1 on failure (out may be modified
 *           only after success is certain; treat out as invalid on failure).
 */
int sps_vui_inject(const uint8_t* data, size_t len, uint8_t* out, size_t out_cap);

#endif /* RK3588_SPS_VUI_H */
