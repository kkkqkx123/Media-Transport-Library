/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * Shared data types for TX and RX pipelines.
 * dmabuf_frame: passes raw video frames between threads (dma-buf based).
 * codec_packet: passes coded H.264 data between threads (slab-allocated).
 */

#ifndef _E2E_PIPELINE_COMMON_H_
#define _E2E_PIPELINE_COMMON_H_

#include <stddef.h>
#include <stdint.h>

/* Raw video frame descriptor. dma-buf fd is the zero-copy transport.
 * Used between Capture Thread and Encode Thread. */
struct dmabuf_frame {
    int      fd;            /* dma-buf fd (BGR3 from V4L2, NV12 from RGA) */
    uint32_t fourcc;        /* DRM pixel format */
    int      width, height;
    uint32_t stride;        /* row stride in bytes */
    uint64_t timestamp;     /* capture timestamp in ns (CLOCK_MONOTONIC) */
    uint64_t frame_seq;     /* global frame sequence number */
};

/* Coded H.264 packet descriptor.
 * Phase 4: data is slab-allocated (tx_pipeline → rx_pipeline).
 * Phase 5: data may point into MTL RX hugepage; mtl_owner tracks pool slot. */
struct codec_packet {
    uint8_t* data;          /* coded stream payload */
    size_t   size;          /* payload size in bytes */
    uint64_t cap_timestamp; /* capture timestamp (from SEI, for E2E latency) */
    uint64_t frame_seq;     /* frame sequence number */
    void*    mtl_owner;     /* non-NULL: struct rx_pkt* from MTL RX pool */
};

#endif /* _E2E_PIPELINE_COMMON_H_ */
