/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 *
 * H.264 Slice reassembler — see slice_reassembler.h for design.
 *
 * UE Golomb decoding of first_mb_in_slice is based on:
 *   ITU-T H.264 (V12) §9.1 Parsing process for Exp-Golomb codes.
 */

#include "slice_reassembler.h"
#include <stdlib.h>
#include <string.h>

struct slice_reassembler {
    uint8_t* buf;       /* accumulated complete frame */
    size_t   buf_size;  /* allocated size */
    size_t   buf_len;   /* valid data length */
};

struct slice_reassembler* slice_reasm_create(void) {
    struct slice_reassembler* r = calloc(1, sizeof(*r));
    return r;
}

void slice_reasm_free(struct slice_reassembler* r) {
    if (!r) return;
    free(r->buf);
    free(r);
}

void slice_reasm_reset(struct slice_reassembler* r) {
    if (r) r->buf_len = 0;
}

/*
 * UE Golomb decode a value from a bit buffer.
 * Returns the decoded value, or -1 on error.
 */
static int ue_golomb_decode(const uint8_t* data, size_t size_bytes,
                            size_t* bit_offset) {
    size_t byte_off = *bit_offset / 8;
    int    bit_off  = (int)(*bit_offset % 8);

    if (byte_off >= size_bytes) return -1;

    /* count leading zeros */
    int leading_zero = 0;
    while (1) {
        if (byte_off >= size_bytes) return -1;
        int bit = (data[byte_off] >> (7 - bit_off)) & 1;
        if (bit == 1) break;
        leading_zero++;
        bit_off++;
        if (bit_off == 8) { bit_off = 0; byte_off++; }
    }

    /* skip the 1 bit */
    bit_off++;
    if (bit_off == 8) { bit_off = 0; byte_off++; }

    /* read leading_zero bits as suffix value */
    if (leading_zero == 0) {
        *bit_offset += (size_t)(leading_zero * 2 + 1);
        return 0; /* code_num = 0 */
    }

    int suffix = 0;
    for (int i = 0; i < leading_zero; i++) {
        if (byte_off >= size_bytes) return -1;
        int bit = (data[byte_off] >> (7 - bit_off)) & 1;
        suffix = (suffix << 1) | bit;
        bit_off++;
        if (bit_off == 8) { bit_off = 0; byte_off++; }
    }

    *bit_offset += (size_t)(leading_zero * 2 + 1);
    return (1 << leading_zero) - 1 + suffix;
}

/*
 * Parse first_mb_in_slice from an Annex B slice NAL unit.
 * first_mb_in_slice is the first UE(v) field after the NAL header.
 * Returns the value, or -1 if parsing fails.
 */
static int annex_b_first_mb_in_slice(const uint8_t* data, size_t size) {
    if (!data || size < 6) return -1;

    /* find start code length */
    size_t sc_len = 0;
    if (size >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
        data[2] == 0x00 && data[3] == 0x01)
        sc_len = 4;
    else if (size >= 3 && data[0] == 0x00 && data[1] == 0x00 &&
             data[2] == 0x01)
        sc_len = 3;
    else
        return -1; /* not a valid Annex B NAL */

    /* skip start code + 1-byte NAL header */
    /* slice data starts at offset sc_len + 1 */
    size_t bit_offset = (sc_len + 1) * 8;
    return ue_golomb_decode(data, size, &bit_offset);
}

/* ensure buffer has room for `extra` more bytes */
static int buf_ensure(struct slice_reassembler* r, size_t extra) {
    size_t need = r->buf_len + extra;
    if (need > r->buf_size) {
        size_t newsz = r->buf_size ? r->buf_size * 2 : 256 * 1024;
        if (newsz < need) newsz = need;
        uint8_t* nb = realloc(r->buf, newsz);
        if (!nb) return -1;
        r->buf = nb;
        r->buf_size = newsz;
    }
    return 0;
}

int slice_reasm_feed(struct slice_reassembler* r,
                     uint8_t* data, size_t size,
                     uint8_t** frame_out, size_t* frame_size_out) {
    if (frame_out) *frame_out = NULL;
    if (!r || !data || !size) return -1;

    /* parse first_mb_in_slice */
    int first_mb = annex_b_first_mb_in_slice(data, size);

    /* first_mb == 0 means a new picture starts.
     * If we already have buffered data, that's the previous complete frame. */
    if (first_mb == 0 && r->buf_len > 0) {
        /* flush previous complete frame */
        *frame_out      = r->buf;
        *frame_size_out = r->buf_len;
        r->buf          = NULL;
        r->buf_size     = 0;
        r->buf_len      = 0;
        /* Note: caller must free(*frame_out) after use */
    }

    /* append current slice to buffer */
    if (buf_ensure(r, size) < 0) return -1;
    memcpy(r->buf + r->buf_len, data, size);
    r->buf_len += size;

    /* frame ready if we just started a new picture (and had a previous one) */
    return (first_mb == 0 && *frame_out != NULL) ? 1 : 0;
}

/* ---- self-test (compile with -DREASM_TEST) ---- */
#ifdef REASM_TEST
#include <stdio.h>

/* synthetic Annex B slice: start_code + NAL_header + UE(first_mb) + dummy */
static uint8_t* make_slice(int first_mb, size_t* out_size) {
    /* allocate enough for worst case */
    static uint8_t buf[64];
    int pos = 0;
    buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x01;
    buf[pos++] = 0x41; /* NAL type=1 (non-IDR slice), nri=2 */
    /* UE encode first_mb */
    if (first_mb == 0) {
        buf[pos++] = 0x80; /* 1 bit → code_num=0 */
    } else if (first_mb <= 15) {
        /* small values: 00..01 + N bits */
        int n = 0, v = first_mb + 1;
        while (v > 1) { n++; v >>= 1; }
        /* leading zeros */
        for (int i = 0; i < n; i++) { buf[pos] &= 0x7F; pos++; }
        buf[pos++] = 0x80; /* the 1 bit + start of suffix (MSB=1) */
        /* suffix bits */
        int suffix = (first_mb + 1) & ((1 << n) - 1);
        if (n > 0) {
            buf[pos - 1] |= (uint8_t)((suffix >> (n - 1)) & 1) << (7 - (n > 0 ? 1 : 0));
            /* simplified — just ensure non-zero for test */
            buf[pos++] = (uint8_t)(suffix & 0xFF);
        }
    }
    buf[pos++] = 0x00; /* dummy payload */
    *out_size = (size_t)pos;
    return buf;
}

int main(void) {
    struct slice_reassembler* r = slice_reasm_create();
    uint8_t *frm; size_t fsz;
    int ret;

    /* feed slice 0 (first_mb=0) — no previous frame */
    size_t s0sz; uint8_t* s0 = make_slice(0, &s0sz);
    ret = slice_reasm_feed(r, s0, s0sz, &frm, &fsz);
    printf("feed slice0 (first_mb=0): ret=%d frame=%p (expected 0,nil)\n", ret, (void*)frm);

    /* feed slice 1 (first_mb=1) */
    size_t s1sz; uint8_t* s1 = make_slice(1, &s1sz);
    ret = slice_reasm_feed(r, s1, s1sz, &frm, &fsz);
    printf("feed slice1 (first_mb=1): ret=%d frame=%p (expected 0,nil)\n", ret, (void*)frm);

    /* feed slice 0 of next picture — triggers flush of previous picture */
    size_t s2sz; uint8_t* s2 = make_slice(0, &s2sz);
    ret = slice_reasm_feed(r, s2, s2sz, &frm, &fsz);
    printf("feed slice0 of pic2: ret=%d frame=%p size=%zu (expected 1,non-nil,>s0sz+s1sz)\n",
           ret, (void*)frm, fsz);
    if (ret == 1 && frm) {
        int pass = (fsz >= s0sz + s1sz);
        printf("%s\n", pass ? "=== slice_reasm PASS ===" : "FAIL: size mismatch");
        free(frm);
        if (!pass) return 1;
    } else {
        printf("FAIL: expected complete frame\n");
        return 1;
    }

    slice_reasm_free(r);
    return 0;
}
#endif /* REASM_TEST */
