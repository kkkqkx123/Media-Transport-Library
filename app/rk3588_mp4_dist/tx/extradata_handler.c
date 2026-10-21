/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Extradata handler — parses AVCC-format extradata from FFmpeg,
 * converts to Annex B format (start-code delimited), and splices
 * SPS+PPS before IDR frames when needed.
 *
 * Note: When the h264_mp4toannexb BSF is active, SPS+PPS are already
 * embedded in the first IDR frame by FFmpeg. This handler provides
 * an additional safety net for late joiners and non-BSF paths.
 */

#include "extradata_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct extradata_handler {
    uint8_t* annexb_data;   /* SPS+PPS in Annex B format (start-code prefixed) */
    size_t   annexb_size;
};

extradata_handler* extradata_handler_create(const uint8_t* data, size_t size) {
    if (!data || size < 7) return NULL;

    extradata_handler* h = calloc(1, sizeof(*h));
    if (!h) return NULL;

    /* Parse AVCC extradata format:
     *   byte 0:   version (0x01)
     *   byte 1:   profile
     *   byte 2:   compatibility
     *   byte 3:   level
     *   byte 4:   NAL length size minus 1 (lower 2 bits)
     *   byte 5:   number of SPS NALs (lower 5 bits)
     *   byte 6-7: SPS NAL size (big-endian)
     *   bytes 8..: SPS data
     *   next byte: number of PPS NALs
     *   next 2:    PPS NAL size
     *   follows:    PPS data
     */
    if (data[0] != 0x01) {
        /* Not AVCC header — might already be Annex B, use as-is */
        h->annexb_data = malloc(size);
        if (!h->annexb_data) { free(h); return NULL; }
        memcpy(h->annexb_data, data, size);
        h->annexb_size = size;
        printf("extradata: %zu bytes (raw, assumed Annex B)\n", size);
        return h;
    }

    int num_sps = data[5] & 0x1f;
    if (num_sps == 0) { free(h); return NULL; }

    /* Estimate Annex B output size:
     *   For each SPS: 4-byte start code + SPS data
     *   For each PPS: 4-byte start code + PPS data
     */
    const uint8_t* p = data + 6;
    size_t estimated = 0;
    size_t sps_sizes[32], pps_sizes[32];
    int sps_cnt = 0, pps_cnt = 0;

    for (int i = 0; i < num_sps && (size_t)(p - data) + 2 <= size; i++) {
        size_t nal_size = ((size_t)p[0] << 8) | p[1];
        p += 2;
        if ((size_t)(p - data) + nal_size > size) break;
        sps_sizes[sps_cnt++] = nal_size;
        p += nal_size;
    }

    int num_pps = (p < data + size) ? (int)*p++ : 0;
    for (int i = 0; i < num_pps && (size_t)(p - data) + 2 <= size; i++) {
        size_t nal_size = ((size_t)p[0] << 8) | p[1];
        p += 2;
        if ((size_t)(p - data) + nal_size > size) break;
        pps_sizes[pps_cnt++] = nal_size;
        p += nal_size;
    }

    /* allocate Annex B buffer: 4 bytes start code per NAL + NAL data */
    for (int i = 0; i < sps_cnt; i++) estimated += 4 + sps_sizes[i];
    for (int i = 0; i < pps_cnt; i++) estimated += 4 + pps_sizes[i];

    h->annexb_data = malloc(estimated);
    if (!h->annexb_data) { free(h); return NULL; }

    /* re-parse and write Annex B */
    p = data + 6;
    for (int i = 0; i < sps_cnt; i++) {
        size_t nal_size = ((size_t)p[0] << 8) | p[1];
        p += 2;
        h->annexb_data[h->annexb_size + 0] = 0x00;
        h->annexb_data[h->annexb_size + 1] = 0x00;
        h->annexb_data[h->annexb_size + 2] = 0x00;
        h->annexb_data[h->annexb_size + 3] = 0x01;
        memcpy(h->annexb_data + h->annexb_size + 4, p, nal_size);
        h->annexb_size += 4 + nal_size;
        p += nal_size;
    }
    num_pps = (p < data + size) ? (int)*p++ : 0;
    for (int i = 0; i < num_pps; i++) {
        size_t nal_size = ((size_t)p[0] << 8) | p[1];
        p += 2;
        h->annexb_data[h->annexb_size + 0] = 0x00;
        h->annexb_data[h->annexb_size + 1] = 0x00;
        h->annexb_data[h->annexb_size + 2] = 0x00;
        h->annexb_data[h->annexb_size + 3] = 0x01;
        memcpy(h->annexb_data + h->annexb_size + 4, p, nal_size);
        h->annexb_size += 4 + nal_size;
        p += nal_size;
    }

    printf("extradata: parsed AVCC → Annex B: %d SPS + %d PPS = %zu bytes\n",
           sps_cnt, pps_cnt, h->annexb_size);
    return h;
}

bool extradata_is_idr(const uint8_t* data, size_t size) {
    if (!data || size < 5) return false;

    /* Scan ALL NAL units for IDR slice (type 5) */
    for (size_t i = 0; i + 4 < size; i++) {
        if (data[i] == 0x00 && data[i+1] == 0x00 &&
            (data[i+2] == 0x01 || (data[i+2] == 0x00 && data[i+3] == 0x01))) {
            size_t nal_start = (data[i+2] == 0x01) ? (i + 3) : (i + 4);
            if (nal_start >= size) continue;
            if ((data[nal_start] & 0x1f) == 5) return true;
        }
    }
    return false;
}

uint8_t* extradata_splice_idr(extradata_handler* h,
                              const uint8_t* frame_data, size_t frame_size,
                              size_t* out_size) {
    if (!h || !frame_data) return NULL;

    *out_size = h->annexb_size + frame_size;
    uint8_t* buf = malloc(*out_size);
    if (!buf) return NULL;

    memcpy(buf, h->annexb_data, h->annexb_size);
    memcpy(buf + h->annexb_size, frame_data, frame_size);
    return buf;
}

const uint8_t* extradata_get_data(const extradata_handler* h) {
    return h ? h->annexb_data : NULL;
}

size_t extradata_get_size(const extradata_handler* h) {
    return h ? h->annexb_size : 0;
}

void extradata_handler_free(extradata_handler* h) {
    if (!h) return;
    free(h->annexb_data);
    free(h);
}
