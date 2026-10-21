/* SPDX-License-Identifier: BSD-3-Clause */

#include "h264_prep.h"
#include "../tx/extradata_handler.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct h264_prep {
    h264_prep_stats stats;
};

static bool h264_has_annexb_start(const uint8_t* data, size_t size) {
    if (!data || size < 4) return false;
    for (size_t i = 0; i + 3 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0 &&
            (data[i + 2] == 1 || (data[i + 2] == 0 && i + 3 < size && data[i + 3] == 1)))
            return true;
    }
    return false;
}

h264_prep* h264_prep_create(void) {
    return calloc(1, sizeof(h264_prep));
}

void h264_prep_free(h264_prep* hp) {
    free(hp);
}

int h264_prep_for_mpp(h264_prep* hp, const uint8_t* data, size_t size,
                      const uint8_t** out_data, size_t* out_size, int* out_owned) {
    if (!hp || !data || !size || !out_data || !out_size || !out_owned)
        return -1;

    hp->stats.pkt_in++;
    *out_owned = 0;

    if (!h264_has_annexb_start(data, size)) {
        hp->stats.invalid++;
        return -1;
    }

    if (extradata_is_idr(data, size))
        hp->stats.idr_seen++;

    *out_data = data;
    *out_size = size;
    return 0;
}

void h264_prep_get_stats(const h264_prep* hp, h264_prep_stats* out) {
    if (!hp || !out) return;
    *out = hp->stats;
}

void h264_prep_print_stats(const h264_prep* hp) {
    if (!hp) return;
    const h264_prep_stats* s = &hp->stats;
    printf("h264_prep: pkt_in=%llu invalid=%llu idr=%llu\n",
           (unsigned long long)s->pkt_in,
           (unsigned long long)s->invalid,
           (unsigned long long)s->idr_seen);
}
