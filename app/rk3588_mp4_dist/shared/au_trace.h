/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Lightweight per-AU trace for TX/RX bitstream comparison.
 *
 * Enable on both sides with the same base path:
 *   export MP4_DIST_AU_TRACE=/tmp/run1
 *
 * Creates (append, one CSV line per AU):
 *   /tmp/run1.tx.csv      — demux → ST22 fb (tx_scheduler publish)
 *   /tmp/run1.tx_wire.csv — MTL get_next_frame (wire send)
 *   /tmp/run1.rx_mtl.csv  — ST22 notify_frame_ready
 *   /tmp/run1.rx_br.csv   — bridge slab copy (decoder input)
 *
 * Columns: seq,stage,size,crc32,nal0,idr,sps,extra
 *   extra = demux_pts (TX) or tai_ns (RX)
 */

#ifndef _MP4_AU_TRACE_H_
#define _MP4_AU_TRACE_H_

#include <stddef.h>
#include <stdint.h>

enum au_trace_stage {
    AU_TRACE_TX_PUBLISH = 0,
    AU_TRACE_TX_WIRE,
    AU_TRACE_RX_MTL,
    AU_TRACE_RX_BRIDGE,
};

void au_trace_record(enum au_trace_stage stage, const uint8_t* data, size_t size,
                     int64_t extra);

void au_trace_close(void);

#endif /* _MP4_AU_TRACE_H_ */
