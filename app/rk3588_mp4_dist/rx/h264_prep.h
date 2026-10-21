/* SPDX-License-Identifier: BSD-3-Clause
 *
 * H.264 Annex B prep for MPP: validate, cache SPS/PPS, inject before IDR.
 */

#ifndef _H264_PREP_H_
#define _H264_PREP_H_

#include <stddef.h>
#include <stdint.h>

typedef struct h264_prep h264_prep;

h264_prep* h264_prep_create(void);
void h264_prep_free(h264_prep* hp);

/**
 * Prepare bitstream for MPP. May prepend cached SPS/PPS before IDR frames.
 * @return 0 on success; out_data/out_size point at buffer (caller frees if out_owned)
 */
int h264_prep_for_mpp(h264_prep* hp, const uint8_t* data, size_t size,
                      const uint8_t** out_data, size_t* out_size, int* out_owned);

typedef struct h264_prep_stats {
    uint64_t pkt_in;
    uint64_t invalid;
    uint64_t idr_seen;
    uint64_t sps_injected;
    uint64_t sps_extracted;
} h264_prep_stats;

void h264_prep_get_stats(const h264_prep* hp, h264_prep_stats* out);
void h264_prep_print_stats(const h264_prep* hp);

#endif
