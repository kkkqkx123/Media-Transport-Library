/* SPDX-License-Identifier: BSD-3-Clause */

#include "e2e_stamp.h"
#include <string.h>

#define E2E_STAMP_VERSION 1U

typedef struct __attribute__((packed)) e2e_stamp_wire {
    uint32_t magic;
    uint32_t version;
    uint64_t capture_tai_ns;
} e2e_stamp_wire;

size_t e2e_stamp_wire_size(void) {
    return sizeof(e2e_stamp_wire);
}

void e2e_stamp_write(void* buf, uint64_t capture_tai_ns) {
    e2e_stamp_wire* w = (e2e_stamp_wire*)buf;
    w->magic          = E2E_STAMP_MAGIC;
    w->version        = E2E_STAMP_VERSION;
    w->capture_tai_ns = capture_tai_ns;
}

bool e2e_stamp_try_parse(const uint8_t* data, size_t size,
                         uint64_t* capture_tai_out, size_t* payload_off_out,
                         size_t* payload_size_out) {
    size_t hdr = e2e_stamp_wire_size();

    if (payload_off_out)
        *payload_off_out = 0;
    if (payload_size_out)
        *payload_size_out = size;
    if (!data || size < hdr)
        return false;

    const e2e_stamp_wire* w = (const e2e_stamp_wire*)data;
    if (w->magic != E2E_STAMP_MAGIC || w->version != E2E_STAMP_VERSION)
        return false;

    if (capture_tai_out)
        *capture_tai_out = w->capture_tai_ns;
    if (payload_off_out)
        *payload_off_out = hdr;
    if (payload_size_out)
        *payload_size_out = size - hdr;
    return true;
}
