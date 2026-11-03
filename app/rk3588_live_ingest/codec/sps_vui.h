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
