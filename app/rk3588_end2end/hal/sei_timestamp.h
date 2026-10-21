/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * H.264 SEI NAL (user_data_unregistered) timestamp embedding.
 * Embeds capture timestamp + frame sequence into the codestream
 * for cross-machine end-to-end latency measurement.
 *
 * SEI NAL format (25 bytes total):
 *   [00 00 00 01]  Annex B start code         (4 bytes)
 *   [06]           NAL header: type=6(SEI)    (1 byte)
 *   [05]           payloadType=5              (1 byte)
 *   [10]           payloadSize=16             (1 byte)
 *   [4B UUID]      "MTLT" identifier          (4 bytes)
 *   [4B frame_seq]  global frame sequence     (4 bytes, little-endian)
 *   [8B cap_ts_ns]  capture PTP timestamp     (8 bytes, little-endian)
 *   [80]           RBSP trailing bits         (1 byte)
 */

#ifndef _E2E_SEI_TIMESTAMP_H_
#define _E2E_SEI_TIMESTAMP_H_

#include <stddef.h>
#include <stdint.h>

#define SEI_TS_NAL_SIZE 25

/**
 * Build an E2E timestamp SEI NAL unit.
 * @param dst       output buffer (must be >= SEI_TS_NAL_SIZE bytes)
 * @param frame_seq global frame sequence number
 * @param cap_ts_ns capture timestamp in nanoseconds (PTP time)
 */
/* frame_seq is 32-bit (4 bytes in SEI payload, ~2 years @ 60fps before wrap) */
void sei_timestamp_build(uint8_t* dst, uint32_t frame_seq, uint64_t cap_ts_ns);

/**
 * Extract E2E timestamp from SEI NAL at the start of a codestream.
 * Strips the SEI in-place (moves remaining data to the beginning).
 * @param data       [in/out] codestream data
 * @param size       [in/out] data size, updated after SEI removal
 * @param frame_seq  [out] extracted frame sequence (32-bit)
 * @param cap_ts_ns  [out] extracted capture timestamp (64-bit)
 * @return 1 if SEI found and extracted, 0 if not found
 */
int sei_timestamp_extract(uint8_t* data, size_t* size,
                          uint32_t* frame_seq, uint64_t* cap_ts_ns);

/** Read SEI at stream start without modifying data (for zero-copy RX). */
int sei_timestamp_peek(const uint8_t* data, size_t size,
                       uint32_t* frame_seq, uint64_t* cap_ts_ns);

#endif /* _E2E_SEI_TIMESTAMP_H_ */
