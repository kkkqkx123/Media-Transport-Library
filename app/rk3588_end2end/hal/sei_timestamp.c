/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

#include "sei_timestamp.h"
#include <string.h>

/* SEI NAL payload: UUID prefix "MTLT" */
static const uint8_t k_sei_uuid[4] = {'M', 'T', 'L', 'T'};

void sei_timestamp_build(uint8_t* dst, uint32_t frame_seq, uint64_t cap_ts_ns) {
  /* Annex B start code */
  dst[0] = 0x00; dst[1] = 0x00; dst[2] = 0x00; dst[3] = 0x01;
  /* NAL header: type=6 (SEI), nri=0 */
  dst[4] = 0x06;
  /* payloadType = 5 (user_data_unregistered) */
  dst[5] = 0x05;
  /* payloadSize = 16 */
  dst[6] = 0x10;
  /* UUID prefix */
  memcpy(dst + 7, k_sei_uuid, 4);
  /* frame_seq (4 bytes LE) */
  dst[11] = (uint8_t)(frame_seq);
  dst[12] = (uint8_t)(frame_seq >> 8);
  dst[13] = (uint8_t)(frame_seq >> 16);
  dst[14] = (uint8_t)(frame_seq >> 24);
  /* cap_ts_ns (8 bytes LE) */
  dst[15] = (uint8_t)(cap_ts_ns);
  dst[16] = (uint8_t)(cap_ts_ns >> 8);
  dst[17] = (uint8_t)(cap_ts_ns >> 16);
  dst[18] = (uint8_t)(cap_ts_ns >> 24);
  dst[19] = (uint8_t)(cap_ts_ns >> 32);
  dst[20] = (uint8_t)(cap_ts_ns >> 40);
  dst[21] = (uint8_t)(cap_ts_ns >> 48);
  dst[22] = (uint8_t)(cap_ts_ns >> 56);
  /* RBSP trailing bits */
  dst[23] = 0x80;
  dst[24] = 0x00; /* padding to 25 bytes (keeps alignment) */
}

static int sei_parse_header(const uint8_t* data, size_t size,
                            uint32_t* frame_seq, uint64_t* cap_ts_ns) {
  if (!data || size < SEI_TS_NAL_SIZE) return 0;
  if (data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x00 ||
      data[3] != 0x01 || data[4] != 0x06 || data[5] != 0x05)
    return 0;
  if (memcmp(data + 7, k_sei_uuid, 4) != 0) return 0;

  uint32_t seq = (uint32_t)data[11] | ((uint32_t)data[12] << 8) |
                 ((uint32_t)data[13] << 16) | ((uint32_t)data[14] << 24);
  uint64_t ts = (uint64_t)data[15] | ((uint64_t)data[16] << 8) |
                ((uint64_t)data[17] << 16) | ((uint64_t)data[18] << 24) |
                ((uint64_t)data[19] << 32) | ((uint64_t)data[20] << 40) |
                ((uint64_t)data[21] << 48) | ((uint64_t)data[22] << 56);

  if (frame_seq) *frame_seq = seq;
  if (cap_ts_ns) *cap_ts_ns = ts;
  return 1;
}

int sei_timestamp_peek(const uint8_t* data, size_t size,
                       uint32_t* frame_seq, uint64_t* cap_ts_ns) {
  return sei_parse_header(data, size, frame_seq, cap_ts_ns);
}

int sei_timestamp_extract(uint8_t* data, size_t* size,
                          uint32_t* frame_seq, uint64_t* cap_ts_ns) {
  if (!data || !size || !sei_parse_header(data, *size, frame_seq, cap_ts_ns))
    return 0;

  size_t remaining = *size - SEI_TS_NAL_SIZE;
  if (remaining > 0)
    memmove(data, data + SEI_TS_NAL_SIZE, remaining);
  *size = remaining;
  return 1;
}
