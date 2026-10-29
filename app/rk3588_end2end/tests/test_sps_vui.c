/* SPDX-License-Identifier: BSD-3-Clause
 * SPS VUI injection self-test: builds a synthetic baseline SPS Annex-B packet,
 * injects VUI colour signalling, and re-parses the output to verify
 * vui_parameters_present_flag / video_signal_type / colour description.
 * Pure bitstream logic — runs on any host, no RK3588 hardware needed.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../codec/sps_vui.h"

/* ---- helpers to build the synthetic SPS ---- */

struct bw {
  uint8_t  buf[128];
  size_t   bit;
};

static void bw_bit(struct bw* w, uint32_t val, int n) {
  for (int i = n - 1; i >= 0; i--) {
    size_t byte = w->bit >> 3;
    w->buf[byte] &= (uint8_t)~(0x80 >> (w->bit & 7));
    if ((val >> i) & 1) w->buf[byte] |= (uint8_t)(0x80 >> (w->bit & 7));
    w->bit++;
  }
}

static void bw_ue(struct bw* w, uint32_t v) {
  uint32_t k = v + 1;
  int      n = 0;
  while ((k >> n) > 1) n++;
  bw_bit(w, 0, n);
  bw_bit(w, k, n + 1);
}

static void bw_rbsp_trailing(struct bw* w) {
  bw_bit(w, 1, 1);
  while (w->bit % 8 != 0) bw_bit(w, 0, 1);
}

/* Build a baseline-profile SPS RBSP (1920x1080, no VUI). */
static size_t build_sps_rbsp(uint8_t* out) {
  struct bw w = {.bit = 0};
  memset(w.buf, 0, sizeof(w.buf));
  bw_bit(&w, 66, 8);  /* profile_idc = baseline */
  bw_bit(&w, 0, 8);   /* constraint flags + reserved */
  bw_bit(&w, 42, 8);  /* level_idc 4.2 */
  bw_ue(&w, 0);       /* seq_parameter_set_id */
  bw_ue(&w, 0);       /* log2_max_frame_num_minus4 */
  bw_ue(&w, 0);       /* pic_order_cnt_type = 0 */
  bw_ue(&w, 0);       /* log2_max_pic_order_cnt_lsb_minus4 */
  bw_ue(&w, 1);       /* max_num_ref_frames */
  bw_bit(&w, 0, 1);   /* gaps_in_frame_num_value_allowed_flag */
  bw_ue(&w, 119);     /* pic_width_in_mbs_minus1 = 1920/16-1 */
  bw_ue(&w, 67);      /* pic_height_in_map_units_minus1 = 1088/16-1 */
  bw_bit(&w, 1, 1);   /* frame_mbs_only_flag */
  bw_bit(&w, 1, 1);   /* direct_8x8_inference_flag */
  bw_bit(&w, 1, 1);   /* frame_cropping_flag */
  bw_ue(&w, 0);       /* crop left */
  bw_ue(&w, 0);       /* crop right */
  bw_ue(&w, 0);       /* crop top */
  bw_ue(&w, 4);       /* crop bottom (1088->1080) */
  bw_bit(&w, 0, 1);   /* vui_parameters_present_flag = 0 */
  bw_rbsp_trailing(&w);
  memcpy(out, w.buf, (w.bit + 7) / 8);
  return (w.bit + 7) / 8;
}

/* ---- re-parse helpers for verification ---- */

struct br {
  const uint8_t* d;
  size_t         len;
  size_t         bit;
};

static uint32_t br_bit(struct br* r) {
  uint32_t v = (r->d[r->bit >> 3] >> (7 - (r->bit & 7))) & 1;
  r->bit++;
  return v;
}

static uint32_t br_bits(struct br* r, int n) {
  uint32_t v = 0;
  while (n--) v = (v << 1) | br_bit(r);
  return v;
}

static uint32_t br_ue(struct br* r) {
  int zeros = 0;
  while (!br_bit(r)) zeros++;
  return (1u << zeros) - 1 + (zeros ? br_bits(r, zeros) : 0);
}

/* Parse SPS RBSP, verify VUI was injected with Rec.709 full range. */
static int verify_vui_709_full(const uint8_t* rbsp, size_t len) {
  struct br r = {rbsp, len * 8, 0};
  (void)r.len;
  br_bits(&r, 24); /* profile/constraint/level */
  br_ue(&r);       /* sps_id */
  br_ue(&r);       /* log2_max_frame_num_minus4 */
  uint32_t poc = br_ue(&r);
  if (poc == 0) br_ue(&r);
  br_ue(&r);         /* max_num_ref_frames */
  br_bit(&r);        /* gaps */
  br_ue(&r);         /* width */
  br_ue(&r);         /* height */
  br_bit(&r);        /* frame_mbs_only */
  br_bit(&r);        /* direct_8x8 */
  if (br_bit(&r)) {  /* frame_cropping */
    br_ue(&r); br_ue(&r); br_ue(&r); br_ue(&r);
  }
  if (!br_bit(&r)) {
    printf("  FAIL: vui_parameters_present_flag still 0\n");
    return -1;
  }
  if (br_bit(&r)) {  /* aspect_ratio_info_present — should be 0 in our VUI */
    printf("  FAIL: unexpected aspect_ratio_info_present=1\n");
    return -1;
  }
  if (br_bit(&r)) {  /* overscan_info_present */
    printf("  FAIL: unexpected overscan_info_present=1\n");
    return -1;
  }
  if (!br_bit(&r)) {
    printf("  FAIL: video_signal_type_present_flag = 0\n");
    return -1;
  }
  uint32_t vfmt = br_bits(&r, 3);
  uint32_t frng = br_bit(&r);
  if (vfmt != 5) {
    printf("  FAIL: video_format=%u (want 5)\n", vfmt);
    return -1;
  }
  if (frng != 1) {
    printf("  FAIL: video_full_range_flag=%u (want 1)\n", frng);
    return -1;
  }
  if (!br_bit(&r)) {
    printf("  FAIL: colour_description_present_flag = 0\n");
    return -1;
  }
  uint32_t cp = br_bits(&r, 8);
  uint32_t tc = br_bits(&r, 8);
  uint32_t mc = br_bits(&r, 8);
  if (cp != 1 || tc != 1 || mc != 1) {
    printf("  FAIL: colour %u/%u/%u (want 1/1/1)\n", cp, tc, mc);
    return -1;
  }
  return 0;
}

static int test_inject_no_vui(void) {
  printf("  [sps_vui] inject into SPS without VUI...\n");
  uint8_t rbsp[128];
  size_t  rbsp_len = build_sps_rbsp(rbsp);

  /* Annex-B packet: SPS + PPS */
  uint8_t pkt[256];
  size_t  n = 0;
  pkt[n++] = 0; pkt[n++] = 0; pkt[n++] = 0; pkt[n++] = 1;
  pkt[n++] = 0x67; /* SPS NAL header */
  memcpy(pkt + n, rbsp, rbsp_len); n += rbsp_len;
  pkt[n++] = 0; pkt[n++] = 0; pkt[n++] = 0; pkt[n++] = 1;
  pkt[n++] = 0x68; /* PPS NAL header */
  pkt[n++] = 0xAB; pkt[n++] = 0xCD;

  uint8_t out[512];
  int     ret = sps_vui_inject(pkt, n, out, sizeof(out));
  if (ret < 0) {
    printf("  FAIL: inject returned %d\n", ret);
    return -1;
  }
  if ((size_t)ret <= n) {
    printf("  FAIL: output not larger than input (%d vs %zu)\n", ret, n);
    return -1;
  }
  /* PPS suffix must survive byte-for-byte */
  if (memcmp(out + ret - 7, pkt + n - 7, 7) != 0) {
    printf("  FAIL: PPS suffix corrupted\n");
    return -1;
  }
  /* Re-parse output SPS: strip start code + NAL header (5 bytes) */
  uint8_t out_rbsp[256];
  size_t  olen = 0;
  /* de-emulation: simply strip 00 00 03 sequences */
  for (int i = 5; i < ret - 7;) {
    if (i + 2 < ret - 7 && out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 3) {
      out_rbsp[olen++] = 0; out_rbsp[olen++] = 0; i += 3;
    } else {
      out_rbsp[olen++] = out[i++];
    }
  }
  /* drop trailing bits to byte boundary already aligned */
  if (verify_vui_709_full(out_rbsp, olen) != 0) return -1;
  printf("  PASS (out %d bytes, was %zu)\n", ret, n);
  return 0;
}

static int test_no_sps(void) {
  printf("  [sps_vui] packet without SPS must fail cleanly...\n");
  uint8_t pkt[] = {0, 0, 0, 1, 0x68, 0xAB, 0xCD, 0, 0, 1, 0x65, 0x11};
  uint8_t out[128];
  int     ret = sps_vui_inject(pkt, sizeof(pkt), out, sizeof(out));
  if (ret >= 0) {
    printf("  FAIL: expected -1 for packet without SPS, got %d\n", ret);
    return -1;
  }
  printf("  PASS\n");
  return 0;
}

static int test_garbage(void) {
  printf("  [sps_vui] garbage/truncated SPS must fail cleanly...\n");
  uint8_t pkt[] = {0, 0, 0, 1, 0x67, 0x11, 0x22}; /* way too short */
  uint8_t out[128];
  int     ret = sps_vui_inject(pkt, sizeof(pkt), out, sizeof(out));
  if (ret >= 0) {
    printf("  FAIL: expected -1 for truncated SPS, got %d\n", ret);
    return -1;
  }
  ret = sps_vui_inject(NULL, 100, out, sizeof(out));
  if (ret >= 0) {
    printf("  FAIL: expected -1 for NULL input, got %d\n", ret);
    return -1;
  }
  printf("  PASS\n");
  return 0;
}

int main(void) {
  int fails = 0;
  printf("test_sps_vui:\n");
  fails += test_inject_no_vui();
  fails += test_no_sps();
  fails += test_garbage();
  if (fails) {
    printf("test_sps_vui: %d FAILURES\n", fails);
    return 1;
  }
  printf("test_sps_vui: all passed\n");
  return 0;
}
