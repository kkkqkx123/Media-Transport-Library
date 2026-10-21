/* SPDX-License-Identifier: BSD-3-Clause */

#include "../shared/e2e_stamp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t buf[64];
    memset(buf, 0xAA, sizeof(buf));

    uint64_t tai = 1700000000123456789ULL;
    e2e_stamp_write(buf, tai);

    uint64_t out_tai = 0;
    size_t off = 0, psz = 0;
    uint8_t payload[] = {0, 0, 0, 1, 0x67};
    memcpy(buf + e2e_stamp_wire_size(), payload, sizeof(payload));
    size_t total = e2e_stamp_wire_size() + sizeof(payload);

    assert(e2e_stamp_try_parse(buf, total, &out_tai, &off, &psz));
    assert(out_tai == tai);
    assert(off == e2e_stamp_wire_size());
    assert(psz == sizeof(payload));
    assert(memcmp(buf + off, payload, psz) == 0);

    uint64_t no_stamp = 0;
    assert(!e2e_stamp_try_parse(payload, sizeof(payload), &no_stamp, NULL, NULL));
    assert(no_stamp == 0);

    printf("test_e2e_stamp: OK\n");
    return 0;
}
