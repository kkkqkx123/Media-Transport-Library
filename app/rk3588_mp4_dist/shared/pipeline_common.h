/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Minimal common types for MP4 distribution pipeline.
 * Stripped-down from E2E pipeline_common.h — only what's needed for
 * coded-stream (non-dmabuf) processing.
 */

#ifndef _MP4_PIPELINE_COMMON_H_
#define _MP4_PIPELINE_COMMON_H_

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/** Coded stream packet flowing through the pipeline.
 *  On TX side: produced by mp4_demux, consumed by mtl_tx_mp4.
 *  On RX side: produced by mtl_rx_bridge, consumed by rx_pipeline decoder.
 */
struct codec_packet {
    uint8_t* data;      /* coded Annex B byte-stream */
    size_t   size;      /* valid byte count */
    uint64_t pts;       /* presentation timestamp in nanoseconds (monotonic or TAI) */
    uint64_t capture_tai_ns; /* TX publish TAI from in-band stamp; 0 if absent */
    void*    owner;     /* opaque owner pointer for lifecycle management */
};

/** RX-side MTL framebuffer descriptor (from notify_frame_ready callback). */
struct rx_pkt {
    void*      frame;     /* MTL framebuffer address (st22_rx_ops level) */
    size_t     size;      /* frame_total_size from st22_rx_frame_meta */
    uint64_t   tai_ns;    /* presentation PTS (ns, monotonic anchor or TAI) */
    uint32_t   media_clk; /* raw RTP timestamp when MEDIA_CLK */
    atomic_int used;      /* pool slot in-use flag (bridge releases) */
};

/** Mode selection for the application. */
enum mp4_dist_mode {
    MP4_DIST_MODE_TX = 0,
    MP4_DIST_MODE_RX,
    MP4_DIST_MODE_BOTH,
};

/** Decoded frame descriptor for the playout buffer. */
struct decoded_frame {
    int      dma_fd;           /* MPP decoder output dma-buf fd */
    uint32_t hor_stride;       /* NV12 horizontal stride */
    uint32_t ver_stride;       /* NV12 vertical stride */
    uint64_t pts;              /* TAI nanosecond presentation timestamp */
    uint64_t capture_tai_ns;   /* TX publish TAI (E2E-C); 0 if absent */
    int      ready;            /* 1 = slot occupied */
};

#endif /* _MP4_PIPELINE_COMMON_H_ */
