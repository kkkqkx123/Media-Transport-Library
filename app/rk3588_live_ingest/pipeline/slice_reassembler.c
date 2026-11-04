#include "slice_reassembler.h"
#include "slab_pool.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct slice_reassembler {
    uint8_t* buf;       /* accumulated complete frame */
    size_t   buf_size;  /* allocated size */
    size_t   buf_len;   /* valid data length */
    struct slab_pool* pool; /* optional: slab-backed accumulation, set by owner */
    bool     buf_is_slab;   /* true when buf came from the slab pool */
    /* observability (decode thread writes, supervisor reads) */
    uint64_t stat_complete;
    uint64_t stat_waiting;
    uint64_t stat_errors;
    uint64_t stat_oversize;
    uint64_t stat_alloc_fails;
    uint64_t stat_flushes;
    uint64_t stat_slab_frames;
    uint64_t stat_malloc_frames;
    size_t   stat_pending_peak;
};

struct slice_reassembler* slice_reasm_create(void) {
    struct slice_reassembler* r = calloc(1, sizeof(*r));
    return r;
}

void slice_reasm_free(struct slice_reassembler* r) {
    if (!r) return;
    if (r->buf) {
        if (r->buf_is_slab && r->pool)
            slab_free(r->pool, r->buf);
        else
            free(r->buf);
    }
    free(r);
}

void slice_reasm_reset(struct slice_reassembler* r) {
    if (!r) return;
    if (r->buf) {
        if (r->buf_is_slab && r->pool)
            slab_free(r->pool, r->buf);
        else
            free(r->buf);
        r->buf = NULL;
    }
    r->buf_size = 0;
    r->buf_len = 0;
    r->buf_is_slab = false;
}

/* Attach an optional slab pool. With a pool, the accumulation buffer is a
 * slab block handed to the caller zero-copy on emit; without a pool the
 * previous malloc behaviour is kept. Must be called before the first feed. */
void slice_reasm_set_pool(struct slice_reassembler* r, struct slab_pool* pool) {
    if (r) r->pool = pool;
}

/* Return buffered bytes without freeing the reassembler (for flush paths). */
size_t slice_reasm_pending(struct slice_reassembler* r) {
    return r ? r->buf_len : 0;
}

/* Release a frame previously emitted by feed/flush. */
void slice_reasm_free_frame(struct slice_reassembler* r, uint8_t* frame) {
    if (!frame) return;
    if (r && r->pool && slab_pool_owns(r->pool, frame))
        slab_free(r->pool, frame);
    else
        free(frame);
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
    if (need <= r->buf_size) return 0;
    if (r->pool) {
        /* Slab-backed: one fixed block per picture, handed out zero-copy.
         * A picture larger than a block cannot be reassembled this way. */
        size_t blk = slab_blk_size(r->pool);
        if (need > blk || r->buf) return -1;
        r->buf = slab_alloc(r->pool);
        if (!r->buf) return -1;
        r->buf_size = blk;
        r->buf_is_slab = true;
        return 0;
    }
    size_t newsz = r->buf_size ? r->buf_size * 2 : 256 * 1024;
    if (newsz < need) newsz = need;
    uint8_t* nb = realloc(r->buf, newsz);
    if (!nb) return -1;
    r->buf = nb;
    r->buf_size = newsz;
    return 0;
}

/* Hand the accumulated buffer to the caller (zero-copy either way). */
static void emit_buffered(struct slice_reassembler* r,
                          uint8_t** frame_out, size_t* frame_size_out) {
    bool is_slab = r->buf_is_slab;
    *frame_out      = r->buf;
    *frame_size_out = r->buf_len;
    r->buf          = NULL;
    r->buf_size     = 0;
    r->buf_len      = 0;
    r->buf_is_slab  = false;
    if (is_slab)
        r->stat_slab_frames++;
    else
        r->stat_malloc_frames++;
    /* Note: caller must release via slice_reasm_free_frame after use */
}

int slice_reasm_feed(struct slice_reassembler* r,
                     uint8_t* data, size_t size,
                     uint8_t** frame_out, size_t* frame_size_out) {
    if (frame_out) *frame_out = NULL;
    if (!r || !data || !size) {
        if (r) r->stat_errors++;
        return -1;
    }

    /* parse first_mb_in_slice */
    int first_mb = annex_b_first_mb_in_slice(data, size);

    /* first_mb == 0 means a new picture starts.
     * If we already have buffered data, that's the previous complete frame. */
    if (first_mb == 0 && r->buf_len > 0) {
        emit_buffered(r, frame_out, frame_size_out);
        r->stat_complete++;
    }

    /* append current slice to buffer */
    if (buf_ensure(r, size) < 0) {
        if (r->pool && r->buf_len + size > slab_blk_size(r->pool))
            r->stat_oversize++;
        else
            r->stat_alloc_fails++;
        r->stat_errors++;
        return -1;
    }
    memcpy(r->buf + r->buf_len, data, size);
    r->buf_len += size;
    if (r->buf_len > r->stat_pending_peak)
        r->stat_pending_peak = r->buf_len;
    r->stat_waiting++;

    /* frame ready if we just started a new picture (and had a previous one) */
    return (first_mb == 0 && *frame_out != NULL) ? 1 : 0;
}

/* Flush whatever is buffered (even an incomplete picture) to the caller.
 * Used on reassembly timeout and on pipeline stop so the tail frame is not
 * stuck one frame behind / lost. Returns 1 with a frame, 0 when empty. */
int slice_reasm_flush(struct slice_reassembler* r,
                      uint8_t** frame_out, size_t* frame_size_out) {
    if (frame_out) *frame_out = NULL;
    if (!r || r->buf_len == 0) return 0;
    emit_buffered(r, frame_out, frame_size_out);
    r->stat_flushes++;
    return 1;
}

void slice_reasm_stats(struct slice_reassembler* r,
                       struct slice_reasm_stats* out) {
    if (!out) return;
    if (!r) { memset(out, 0, sizeof(*out)); return; }
    out->feed_complete = r->stat_complete;
    out->feed_waiting = r->stat_waiting;
    out->feed_errors = r->stat_errors;
    out->oversize_drops = r->stat_oversize;
    out->alloc_fails = r->stat_alloc_fails;
    out->flushes = r->stat_flushes;
    out->slab_frames = r->stat_slab_frames;
    out->malloc_frames = r->stat_malloc_frames;
    out->pending_peak = r->stat_pending_peak;
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
        printf("%s\n", pass ? "--- picture flush ok ---" : "FAIL: size mismatch");
        slice_reasm_free_frame(r, frm);
        if (!pass) return 1;
    } else {
        printf("FAIL: expected complete frame\n");
        return 1;
    }

    /* timeout/stop path: the slice of pic2 is still buffered (no following
     * picture arrived). flush must emit it as the tail frame. */
    if (slice_reasm_pending(r) != s2sz) {
      printf("FAIL: tail slice must stay buffered\n");
      return 1;
    }
    ret = slice_reasm_flush(r, &frm, &fsz);
    printf("flush: ret=%d size=%zu (expected 1,%zu)\n", ret, fsz, s2sz);
    if (ret != 1 || !frm || fsz != s2sz) {
        printf("FAIL: flush must emit the tail frame\n");
        return 1;
    }
    slice_reasm_free_frame(r, frm);
    if (slice_reasm_flush(r, &frm, &fsz) != 0) {
        printf("FAIL: flush on empty must return 0\n");
        return 1;
    }
    printf("=== slice_reasm PASS ===\n");

    slice_reasm_free(r);
    return 0;
}
#endif /* REASM_TEST */
