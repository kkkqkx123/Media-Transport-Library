#ifndef _RK3588_COMMON_PIPELINE_COMMON_H_
#define _RK3588_COMMON_PIPELINE_COMMON_H_

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Raw video frame descriptor (end2end). */
struct dmabuf_frame {
    int      fd;
    uint32_t fourcc;
    int      width, height;
    uint32_t stride;
    uint64_t timestamp;
    uint64_t frame_seq;
    void*    capture;
    uint32_t buf_index;
    uint64_t generation;
};

/* Coded stream packet (union of both apps' fields). */
struct codec_packet {
    uint8_t* data;
    size_t   size;

    /* end2end fields */
    uint64_t cap_timestamp;
    uint64_t frame_seq;
    void*    mtl_owner;

    /* mp4_dist fields */
    uint64_t pts;
    uint64_t capture_tai_ns;
    void*    owner;
};

/* RX-side MTL framebuffer descriptor (mp4_dist). */
struct mp4_rx_pkt {
    void*      frame;
    size_t     size;
    uint64_t   tai_ns;
    uint32_t   media_clk;
    atomic_int used;
};

enum mp4_dist_mode {
    MP4_DIST_MODE_TX = 0,
    MP4_DIST_MODE_RX,
    MP4_DIST_MODE_BOTH,
};

struct decoded_frame {
    int      dma_fd;
    uint32_t hor_stride;
    uint32_t ver_stride;
    uint64_t pts;
    uint64_t capture_tai_ns;
    int      ready;
};

#endif /* _RK3588_COMMON_PIPELINE_COMMON_H_ */
