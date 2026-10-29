/* SPDX-License-Identifier: BSD-3-Clause
 * Port of zlmediakit HdmiInSpsVui to C. See sps_vui.h.
 */

#include "sps_vui.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ---- bit reader over RBSP ---- */

struct bit_reader {
  const uint8_t* d;
  size_t         len; /* bytes */
  size_t         bit; /* absolute bit position */
  bool           err;
};

static bool br_read_bit(struct bit_reader* r, uint32_t* out) {
  size_t byte = r->bit >> 3;
  if (r->err || byte >= r->len) {
    r->err = true;
    return false;
  }
  *out = (r->d[byte] >> (7 - (r->bit & 7))) & 1;
  r->bit++;
  return true;
}

static uint32_t br_read_bits(struct bit_reader* r, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    uint32_t b;
    if (!br_read_bit(r, &b)) return 0;
    v = (v << 1) | b;
  }
  return v;
}

static uint32_t br_read_ue(struct bit_reader* r) {
  int zeros = 0;
  for (;;) {
    uint32_t b;
    if (!br_read_bit(r, &b)) return 0;
    if (b) break;
    if (++zeros > 31) {
      r->err = true;
      return 0;
    }
  }
  uint32_t suffix = zeros ? br_read_bits(r, zeros) : 0;
  return (1u << zeros) - 1 + suffix;
}

static int32_t br_read_se(struct bit_reader* r) {
  uint32_t ue = br_read_ue(r);
  uint32_t k  = ue + 1;
  return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}

static bool br_exhausted(struct bit_reader* r) {
  return r->err || r->bit >= r->len * 8;
}

/* ---- emulation prevention ---- */

static size_t rbsp_from_ebsp(const uint8_t* src, size_t len, uint8_t* dst,
                             size_t dst_cap) {
  size_t o = 0, i = 0;
  while (i < len) {
    if (i + 2 < len && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 3) {
      if (o + 2 > dst_cap) return 0;
      dst[o++] = 0;
      dst[o++] = 0;
      i += 3;
    } else {
      if (o + 1 > dst_cap) return 0;
      dst[o++] = src[i++];
    }
  }
  return o;
}

static size_t ebsp_from_rbsp(const uint8_t* src, size_t len, uint8_t* dst,
                             size_t dst_cap) {
  size_t o = 0, zeros = 0;
  for (size_t i = 0; i < len; i++) {
    if (zeros >= 2 && src[i] <= 3) {
      if (o + 1 > dst_cap) return 0;
      dst[o++] = 3;
      zeros = 0;
    }
    if (o + 1 > dst_cap) return 0;
    dst[o++] = src[i];
    zeros    = (src[i] == 0) ? zeros + 1 : 0;
  }
  return o;
}

/* ---- bit vector (set-bit only writes; buffer must be zeroed first) ---- */

struct bitvec {
  uint8_t* bits;
  size_t   n;  /* number of bits used */
  size_t   cap;
  bool     oom;
};

static bool bv_grow(struct bitvec* v, size_t need) {
  if (v->n + need <= v->cap) return true;
  size_t ncap = v->cap ? v->cap : 256;
  while (v->n + need > ncap) ncap *= 2;
  uint8_t* nb = realloc(v->bits, ncap);
  if (!nb) {
    v->oom = true;
    return false;
  }
  memset(nb + v->cap, 0, ncap - v->cap);
  v->bits = nb;
  v->cap  = ncap;
  return true;
}

static bool bv_push(struct bitvec* v, uint32_t val, int n) {
  if (!bv_grow(v, (size_t)n)) return false;
  for (int i = n - 1; i >= 0; i--) {
    size_t bit = v->n++;
    if ((val >> i) & 1) v->bits[bit >> 3] |= (uint8_t)(1 << (7 - (bit & 7)));
  }
  return true;
}

static bool bv_set(struct bitvec* v, size_t pos, bool val) {
  if (pos >= v->n) return false;
  uint8_t mask = (uint8_t)(1 << (7 - (pos & 7)));
  if (val)
    v->bits[pos >> 3] |= mask;
  else
    v->bits[pos >> 3] &= (uint8_t)~mask;
  return true;
}

static bool bv_bit_at(const struct bitvec* v, size_t pos) {
  return (v->bits[pos >> 3] >> (7 - (pos & 7))) & 1;
}

/* Insert n bits from src (MSB-first packed) at position pos, shifting the
 * existing tail right. Bit-by-bit to stay correct for non-byte-aligned n. */
static bool bv_insert(struct bitvec* v, size_t pos, const uint8_t* src,
                      size_t n) {
  if (!bv_grow(v, n)) return false;
  for (size_t i = v->n; i-- > pos;) bv_set(v, i + n, bv_bit_at(v, i));
  v->n += n;
  for (size_t i = 0; i < n; i++)
    if (!bv_set(v, pos + i, (src[i >> 3] >> (7 - (i & 7))) & 1)) return false;
  return true;
}

/* ---- SPS parsing up to vui_parameters_present_flag ---- */

static bool high_profile(uint32_t p) {
  switch (p) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135:
      return true;
    default:
      return false;
  }
}

static bool parse_sps_to_vui_flag(const uint8_t* rbsp, size_t len,
                                  size_t* vui_bit) {
  struct bit_reader r = {rbsp, len, 0, false};
  uint32_t profile_idc = br_read_bits(&r, 8);
  br_read_bits(&r, 8); /* constraint flags + reserved */
  br_read_bits(&r, 8); /* level_idc */
  br_read_ue(&r);      /* seq_parameter_set_id */
  if (r.err) return false;

  if (high_profile(profile_idc)) {
    uint32_t chroma_format_idc = br_read_ue(&r);
    if (chroma_format_idc == 3) br_read_bits(&r, 1);
    br_read_ue(&r); /* bit_depth_luma_minus8 */
    br_read_ue(&r); /* bit_depth_chroma_minus8 */
    br_read_bits(&r, 1); /* qpprime_y_zero_transform_bypass_flag */
    if (br_read_bits(&r, 1)) { /* seq_scaling_matrix_present_flag */
      int n_lists = (chroma_format_idc == 3) ? 12 : 8;
      for (int i = 0; i < n_lists && !r.err; i++) {
        if (!br_read_bits(&r, 1)) continue;
        int size = (i < 6) ? 16 : 64;
        int last = 8;
        int next = 8;
        for (int j = 0; j < size && !r.err; j++) {
          if (next != 0) {
            int32_t delta = br_read_se(&r);
            next = (last + delta + 256) % 256;
          }
          last = (next == 0) ? last : next;
        }
      }
    }
  }
  br_read_ue(&r); /* log2_max_frame_num_minus4 */
  uint32_t poc_type = br_read_ue(&r);
  if (poc_type == 0) {
    br_read_ue(&r);
  } else if (poc_type == 1) {
    br_read_bits(&r, 1); /* delta_pic_order_always_zero_flag */
    br_read_se(&r);      /* offset_for_non_ref_pic */
    br_read_se(&r);      /* offset_for_top_to_bottom_field */
    uint32_t num_ref = br_read_ue(&r);
    for (uint32_t i = 0; i < num_ref && i < 255 && !br_exhausted(&r); i++)
      br_read_se(&r);
  }
  br_read_ue(&r); /* max_num_ref_frames */
  br_read_bits(&r, 1); /* gaps_in_frame_num_value_allowed_flag */
  br_read_ue(&r); /* pic_width_in_mbs_minus1 */
  br_read_ue(&r); /* pic_height_in_map_units_minus1 */
  bool frame_mbs_only = br_read_bits(&r, 1) != 0;
  if (!frame_mbs_only) br_read_bits(&r, 1); /* mb_adaptive_frame_field_flag */
  br_read_bits(&r, 1); /* direct_8x8_inference_flag */
  if (br_read_bits(&r, 1)) { /* frame_cropping_flag */
    br_read_ue(&r);
    br_read_ue(&r);
    br_read_ue(&r);
    br_read_ue(&r);
  }
  if (br_exhausted(&r)) return false;
  *vui_bit = r.bit;
  return true;
}

static bool parse_vui_to_signal_type_flag(const uint8_t* rbsp, size_t len,
                                          size_t vui_bit, size_t* vst_bit) {
  size_t total = len * 8;
  size_t pos   = vui_bit + 1;
  if (pos >= total) return false;
  bool aspect = (rbsp[pos >> 3] >> (7 - (pos & 7))) & 1;
  pos += aspect ? (1 + 8) : 1; /* +aspect_ratio_idc (non-extended SAR) */
  if (pos >= total) return false;
  bool overscan = (rbsp[pos >> 3] >> (7 - (pos & 7))) & 1;
  pos += overscan ? 2 : 1; /* +overscan_appropriate_flag */
  if (pos >= total) return false;
  *vst_bit = pos; /* video_signal_type_present_flag */
  return true;
}

/* Append the video_signal_type fields (fmt=5, full=1, cd=1, 3x BT.709=1). */
static bool push_signal_type_fields(struct bitvec* bv) {
  return bv_push(bv, 5, 3) && /* video_format = 5 (unspecified) */
         bv_push(bv, 1, 1) && /* video_full_range_flag = 1 */
         bv_push(bv, 1, 1) && /* colour_description_present_flag */
         bv_push(bv, 1, 8) && /* colour_primaries = 1 (BT.709) */
         bv_push(bv, 1, 8) && /* transfer_characteristics = 1 */
         bv_push(bv, 1, 8);   /* matrix_coefficients = 1 */
}

int sps_vui_inject(const uint8_t* data, size_t len, uint8_t* out,
                   size_t out_cap) {
  if (!data || len < 8 || !out) return -1;

  /* 1. locate SPS NAL (type 7): start code + NAL header */
  size_t sps_payload = 0, sps_end = len;
  bool   found = false;
  size_t pos   = 0;
  while (pos + 3 <= len) {
    size_t sc = pos;
    while (sc + 3 <= len &&
           !(data[sc] == 0 && data[sc + 1] == 0 && data[sc + 2] == 1))
      sc++;
    if (sc + 3 > len) break;
    size_t sc_len = (sc + 3 < len && data[sc + 3] == 0) ? 4 : 3;
    size_t nal    = sc + sc_len;
    if (nal >= len) break;
    uint8_t nal_type = data[nal] & 0x1F;
    size_t  next     = nal + 1;
    size_t  next_sc  = len;
    while (next + 3 <= len &&
           !(data[next] == 0 && data[next + 1] == 0 && data[next + 2] == 1))
      next++;
    if (next + 3 <= len) next_sc = next;
    /* a 4-byte start code (00 00 00 01) is hit at its last three bytes;
     * back up one byte so the suffix keeps the full start code */
    if (next_sc > nal + 1 && data[next_sc - 1] == 0) next_sc--;
    if (nal_type == 7) {
      sps_payload = nal;
      sps_end     = next_sc;
      found       = true;
      break;
    }
    pos = next_sc;
    if (next_sc >= len) break;
  }
  if (!found || sps_end <= sps_payload + 1) return -1;

  /* 2. extract SPS RBSP (skip the NAL header byte) */
  const uint8_t* nal_data = data + sps_payload;
  size_t         nal_len  = sps_end - sps_payload;
  uint8_t        rbsp[512];
  size_t rbsp_len =
      rbsp_from_ebsp(nal_data + 1, nal_len - 1, rbsp, sizeof(rbsp));
  if (rbsp_len < 4) return -1;

  /* 3. parse up to vui_parameters_present_flag */
  size_t vui_bit = 0;
  if (!parse_sps_to_vui_flag(rbsp, rbsp_len, &vui_bit)) return -1;

  /* 4. rebuild as a bit vector and patch the colour signalling */
  struct bitvec bv = {0};
  bv.cap           = rbsp_len * 8 + 128;
  bv.bits          = calloc(1, bv.cap);
  if (!bv.bits) return -1;
  memcpy(bv.bits, rbsp, rbsp_len);
  bv.n = rbsp_len * 8;

  bool    ok = true;
  uint8_t ins[8];
  bool    vui = bv_bit_at(&bv, vui_bit);
  if (!vui) {
    /* vui_parameters_present_flag = 0: set it, drop the old rbsp trailing,
     * append a complete Rec.709 full-range VUI, then a new trailing. */
    bv.bits[vui_bit >> 3] |= (uint8_t)(1 << (7 - (vui_bit & 7)));
    /* clear stale bits (old rbsp trailing) before overwriting the tail —
     * pushes only OR, so leftover 1-bits would corrupt the new fields */
    for (size_t i = vui_bit + 1; i < rbsp_len * 8; i++) bv_set(&bv, i, false);
    bv.n = vui_bit + 1;
    ok   = bv_push(&bv, 0, 1) && /* aspect_ratio_info_present_flag */
         bv_push(&bv, 0, 1) &&   /* overscan_info_present_flag */
         bv_push(&bv, 1, 1) &&   /* video_signal_type_present_flag */
         push_signal_type_fields(&bv) &&
         bv_push(&bv, 0, 1) && /* chroma_loc_info_present_flag */
         bv_push(&bv, 0, 1) && /* timing_info_present_flag */
         bv_push(&bv, 0, 1) && /* nal_hrd_parameters_present_flag */
         bv_push(&bv, 0, 1) && /* vcl_hrd_parameters_present_flag */
         bv_push(&bv, 0, 1) && /* low_delay_hrd_flag */
         bv_push(&bv, 0, 1) && /* pic_struct_present_flag */
         bv_push(&bv, 0, 1) && /* bitstream_restriction_flag */
         bv_push(&bv, 1, 1);   /* rbsp_stop_one_bit */
  } else {
    size_t vst_bit = 0;
    if (!parse_vui_to_signal_type_flag(rbsp, rbsp_len, vui_bit, &vst_bit)) {
      free(bv.bits);
      return -1;
    }
    if (!bv_bit_at(&bv, vst_bit)) {
      /* video_signal_type_present_flag = 0: set it, insert the fields after */
      bv.bits[vst_bit >> 3] |= (uint8_t)(1 << (7 - (vst_bit & 7)));
      memset(ins, 0, sizeof(ins));
      struct bitvec tmp = {ins, 0, sizeof(ins) * 8, false};
      ok = push_signal_type_fields(&tmp);
      if (ok) ok = bv_insert(&bv, vst_bit + 1, ins, tmp.n);
    } else {
      size_t fr_bit = vst_bit + 4; /* video_format(3) + full_range_flag */
      size_t cd_bit = vst_bit + 5; /* colour_description_present_flag */
      if (fr_bit < bv.n)
        bv.bits[fr_bit >> 3] |= (uint8_t)(1 << (7 - (fr_bit & 7)));
      if (cd_bit < bv.n && bv_bit_at(&bv, cd_bit)) {
        /* colour description already present: rewrite 3 fields to BT.709 */
        for (int k = 0; k < 3; k++) {
          size_t base = cd_bit + 1 + (size_t)k * 8;
          for (int b = 0; b < 8 && base + 8 <= bv.n; b++)
            if (!bv_set(&bv, base + (size_t)b, b == 7)) ok = false;
        }
      } else {
        if (cd_bit < bv.n)
          bv.bits[cd_bit >> 3] |= (uint8_t)(1 << (7 - (cd_bit & 7)));
        memset(ins, 0, sizeof(ins));
        struct bitvec tmp = {ins, 0, sizeof(ins) * 8, false};
        ok = bv_push(&tmp, 1, 8) && bv_push(&tmp, 1, 8) &&
             bv_push(&tmp, 1, 8);
        if (ok) ok = bv_insert(&bv, cd_bit + 1, ins, tmp.n);
      }
    }
  }

  if (!ok || bv.oom) {
    free(bv.bits);
    return -1;
  }

  /* 5. byte-align (stop bit already in place except the !vui branch handled
   * above; pad with zeros as rbsp_trailing_bits alignment) */
  while (bv.n % 8 != 0) {
    if (!bv_push(&bv, 0, 1)) {
      free(bv.bits);
      return -1;
    }
  }
  size_t new_rbsp_len = bv.n / 8;
  uint8_t ebsp[600];
  size_t  ebsp_len =
      ebsp_from_rbsp(bv.bits, new_rbsp_len, ebsp, sizeof(ebsp));
  free(bv.bits);
  if (ebsp_len == 0) return -1;

  /* 6. reassemble: [prefix incl. start code] + NAL header + EBSP + [suffix] */
  size_t prefix_len = sps_payload; /* everything up to the SPS NAL header */
  size_t total      = prefix_len + 1 + ebsp_len + (len - sps_end);
  if (total > out_cap) return -1;
  memcpy(out, data, prefix_len);
  out[prefix_len] = nal_data[0]; /* NAL header (0x67) */
  memcpy(out + prefix_len + 1, ebsp, ebsp_len);
  memcpy(out + prefix_len + 1 + ebsp_len, data + sps_end, len - sps_end);
  return (int)total;
}
