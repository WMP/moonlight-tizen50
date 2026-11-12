/*
 * Host self-test for mlclient/h264_sps.c. The h264bitstream library (third_party/moonlight-tizen/h264bitstream)
 * is the independent oracle: it writes the input SPS NALs and reads both the input and the patched NAL.
 * Run: sh tests/build-esplus-ecore-test.sh
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "h264_sps.h"
#include "h264_stream.h"

#define MAXV 64
static uint8_t vec[MAXV][H264_SPS_MAX_IN]; static size_t vlen[MAXV]; static int nvec;

/* one Annex-B SPS NAL from an sps_t, written by the library (its buf[0] is a zero byte, the NAL starts at buf[1]) */
static size_t gen(const sps_t *s, int sc, uint8_t *dst) {
  h264_stream_t *h = h264_new();
  uint8_t tmp[400]; memset(tmp, 0, sizeof tmp);
  h->nal->nal_ref_idc = 3; h->nal->nal_unit_type = NAL_UNIT_TYPE_SPS;
  memcpy(h->sps, s, sizeof *s);
  int n = write_nal_unit(h, tmp, sizeof tmp);
  assert(n > 1 && (size_t)n + 4 <= H264_SPS_MAX_IN);
  h264_free(h);
  memset(dst, 0, (size_t)sc - 1); dst[sc - 1] = 1;
  memcpy(dst + sc, tmp + 1, (size_t)n - 1);
  return (size_t)sc + (size_t)n - 1;
}
static int oracle(const uint8_t *nal, size_t len, sps_t *s) {   /* 0 = parsed */
  uint8_t tmp[H264_SPS_MAX_IN + H264_SPS_EXTRA];
  size_t sc = nal[2] == 1 ? 3 : 4;
  memcpy(tmp, nal + sc, len - sc);
  h264_stream_t *h = h264_new();
  int n = read_nal_unit(h, tmp, (int)(len - sc));
  if (n > 0) memcpy(s, h->sps, sizeof *s);
  h264_free(h);
  return n > 0 ? 0 : -1;
}
static sps_t base(int profile) {
  sps_t s; memset(&s, 0, sizeof s);
  s.profile_idc = profile; s.level_idc = 41; s.chroma_format_idc = 1; s.log2_max_frame_num_minus4 = 4;
  s.log2_max_pic_order_cnt_lsb_minus4 = 4; s.num_ref_frames = 4;
  s.pic_width_in_mbs_minus1 = 119; s.pic_height_in_map_units_minus1 = 67; s.frame_mbs_only_flag = 1; s.direct_8x8_inference_flag = 1;
  return s;
}
static void no_emul(const uint8_t *nal, size_t len) {   /* no 00 00 {00,01,02} in the payload, no trailing 00 */
  size_t sc = nal[2] == 1 ? 3 : 4;
  for (size_t i = sc + 1; i + 2 < len; i++) assert(!(!nal[i] && !nal[i + 1] && nal[i + 2] <= 2));
  assert(nal[len - 1] != 0);
}
static uint32_t rnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 16; }
static int maxi(int a, int b) { return a > b ? a : b; }

/* patch must give ADDED; the oracle agrees on all old fields and the new ones */
static void check_added(const uint8_t *in, size_t n, const char *what) {
  uint8_t out[H264_SPS_MAX_IN + H264_SPS_EXTRA + 1]; size_t on = 0; h264_sps_info si;
  assert(h264_sps_patch(in, n, out, sizeof out - 1, &on, &si) == H264_SPS_ADDED);
  assert(on > 0 && on <= n + H264_SPS_EXTRA && si.restr == 0);
  if (strcmp(what, "sweep")) printf("  %-28s in=%3zu out=%3zu growth=%+d\n", what, n, on, (int)on - (int)n);
  assert(!memcmp(in, out, in[2] == 1 ? 4 : 5));   /* same start code and NAL header */
  no_emul(out, on);
  sps_t a, b;
  assert(oracle(in, n, &a) == 0 && oracle(out, on, &b) == 0);
  assert(si.profile == a.profile_idc && si.level == a.level_idc && si.ref_frames == a.num_ref_frames
         && si.poc_type == a.pic_order_cnt_type && si.vui == a.vui_parameters_present_flag);
  sps_t e = a;
  e.vui_parameters_present_flag = 1; e.vui.bitstream_restriction_flag = 1;
  e.vui.motion_vectors_over_pic_boundaries_flag = 1; e.vui.max_bytes_per_pic_denom = 2; e.vui.max_bits_per_mb_denom = 1;
  e.vui.log2_max_mv_length_horizontal = 16; e.vui.log2_max_mv_length_vertical = 16;
  e.vui.num_reorder_frames = 0; e.vui.max_dec_frame_buffering = maxi(1, a.num_ref_frames);
  assert(!memcmp(&e, &b, sizeof e));
  /* a second pass sees our restriction and keeps it */
  uint8_t o2[H264_SPS_MAX_IN + H264_SPS_EXTRA]; size_t n2;
  assert(h264_sps_patch(out, on, o2, sizeof o2, &n2, &si) == H264_SPS_KEPT && si.restr == 1 && si.reorder == 0 && si.dec_buf == maxi(1, a.num_ref_frames));
  /* every smaller out_cap fails and never writes past the cap */
  for (size_t cap = 0; cap < on; cap++) {
    uint8_t g[H264_SPS_MAX_IN + H264_SPS_EXTRA + 1]; memset(g, 0xA5, sizeof g);
    assert(h264_sps_patch(in, n, g, cap, &n2, &si) == H264_SPS_ERR);
    for (size_t i = cap; i < sizeof g; i++) assert(g[i] == 0xA5);
  }
  (void)what;
}
static void check_kept(const uint8_t *in, size_t n, int reorder, int dec_buf) {
  uint8_t out[64]; memset(out, 0x5A, sizeof out); size_t on = 99; h264_sps_info si;
  assert(h264_sps_patch(in, n, out, sizeof out, &on, &si) == H264_SPS_KEPT);
  assert(si.restr == 1 && si.vui == 1 && si.reorder == reorder && si.dec_buf == dec_buf);
  for (size_t i = 0; i < sizeof out; i++) assert(out[i] == 0x5A);
}
static void add(const sps_t *s, int sc) { assert(nvec < MAXV); vlen[nvec] = gen(s, sc, vec[nvec]); nvec++; }

int main(void) {
  sps_t s;
  puts("h264_sps vectors:");
  /* 1 baseline, no VUI */
  s = base(66); s.level_idc = 31; s.num_ref_frames = 1; s.pic_order_cnt_type = 2; add(&s, 4); check_added(vec[nvec - 1], vlen[nvec - 1], "baseline no vui");
  /* 2 high, VUI with timing + video signal (colour description) */
  s = base(100); s.vui_parameters_present_flag = 1;
  s.vui.video_signal_type_present_flag = 1; s.vui.video_format = 5; s.vui.video_full_range_flag = 1; s.vui.colour_description_present_flag = 1;
  s.vui.colour_primaries = 1; s.vui.transfer_characteristics = 1; s.vui.matrix_coefficients = 1;
  s.vui.timing_info_present_flag = 1; s.vui.num_units_in_tick = 1001; s.vui.time_scale = 120000; s.vui.fixed_frame_rate_flag = 1;
  add(&s, 4); check_added(vec[nvec - 1], vlen[nvec - 1], "high vui timing+signal");
  /* VUI with aspect (idc 255), overscan, chroma loc, pic_struct */
  s = base(100); s.vui_parameters_present_flag = 1;
  s.vui.aspect_ratio_info_present_flag = 1; s.vui.aspect_ratio_idc = 255; s.vui.sar_width = 16; s.vui.sar_height = 9;
  s.vui.overscan_info_present_flag = 1; s.vui.overscan_appropriate_flag = 1;
  s.vui.chroma_loc_info_present_flag = 1; s.vui.chroma_sample_loc_type_top_field = 1; s.vui.chroma_sample_loc_type_bottom_field = 2;
  s.vui.pic_struct_present_flag = 1;
  add(&s, 3); check_added(vec[nvec - 1], vlen[nvec - 1], "high vui aspect/chroma_loc");
  /* 3 VUI with NAL HRD (2 cpb), and with both HRDs */
  s = base(100); s.vui_parameters_present_flag = 1; s.vui.nal_hrd_parameters_present_flag = 1; s.vui.low_delay_hrd_flag = 1;
  s.hrd_nal.cpb_cnt_minus1 = 1; s.hrd_nal.bit_rate_scale = 3; s.hrd_nal.cpb_size_scale = 4;
  s.hrd_nal.bit_rate_value_minus1[0] = 1000; s.hrd_nal.cpb_size_value_minus1[0] = 2000; s.hrd_nal.cbr_flag[0] = 1;
  s.hrd_nal.bit_rate_value_minus1[1] = 3000; s.hrd_nal.cpb_size_value_minus1[1] = 4000;
  s.hrd_nal.initial_cpb_removal_delay_length_minus1 = 23; s.hrd_nal.cpb_removal_delay_length_minus1 = 23;
  s.hrd_nal.dpb_output_delay_length_minus1 = 23; s.hrd_nal.time_offset_length = 24;
  add(&s, 4); check_added(vec[nvec - 1], vlen[nvec - 1], "high vui nal hrd");
  s.vui.vcl_hrd_parameters_present_flag = 1; s.hrd_vcl = s.hrd_nal; s.hrd_vcl.cpb_cnt_minus1 = 0;
  add(&s, 4); check_added(vec[nvec - 1], vlen[nvec - 1], "high vui nal+vcl hrd");
  /* 4 VUI with restriction: kept, never modified */
  s = base(100); s.level_idc = 31; s.num_ref_frames = 1; s.vui_parameters_present_flag = 1; s.vui.bitstream_restriction_flag = 1;
  s.vui.motion_vectors_over_pic_boundaries_flag = 1; s.vui.max_bytes_per_pic_denom = 3; s.vui.max_bits_per_mb_denom = 1;
  s.vui.log2_max_mv_length_horizontal = 15; s.vui.log2_max_mv_length_vertical = 15;
  s.vui.num_reorder_frames = 0; s.vui.max_dec_frame_buffering = 1;
  add(&s, 4); check_kept(vec[nvec - 1], vlen[nvec - 1], 0, 1);
  s.vui.num_reorder_frames = 2; s.vui.max_dec_frame_buffering = 4;
  add(&s, 3); check_kept(vec[nvec - 1], vlen[nvec - 1], 2, 4);
  /* 5 scaling matrix: ERR */
  s = base(100); s.seq_scaling_matrix_present_flag = 1;
  {
    uint8_t o[64]; size_t on; h264_sps_info si; add(&s, 4);
    assert(h264_sps_patch(vec[nvec - 1], vlen[nvec - 1], o, sizeof o, &on, &si) == H264_SPS_ERR);
    nvec--;
  }
  /* 6 poc types 0, 1, 2; cropping; both start codes; interlaced; 4:4:4 and 10 bit; level 1b style odd values */
  for (int poc = 0; poc <= 2; poc++) for (int sc = 3; sc <= 4; sc++) {
    s = base(poc == 2 ? 77 : 100); s.pic_order_cnt_type = poc;
    if (poc == 1) {
      s.delta_pic_order_always_zero_flag = 1; s.offset_for_non_ref_pic = -3; s.offset_for_top_to_bottom_field = 2;
      s.num_ref_frames_in_pic_order_cnt_cycle = 3; s.offset_for_ref_frame[0] = 1; s.offset_for_ref_frame[1] = -7; s.offset_for_ref_frame[2] = 300;
    }
    s.frame_cropping_flag = 1; s.frame_crop_bottom_offset = 4; s.frame_crop_left_offset = 1;
    add(&s, sc); check_added(vec[nvec - 1], vlen[nvec - 1], "poc/crop/startcode");
  }
  s = base(122); s.chroma_format_idc = 2; s.bit_depth_luma_minus8 = 2; s.bit_depth_chroma_minus8 = 2; s.frame_mbs_only_flag = 0; s.mb_adaptive_frame_field_flag = 1;
  add(&s, 4); check_added(vec[nvec - 1], vlen[nvec - 1], "422 10bit interlaced");
  s = base(244); s.chroma_format_idc = 3; s.residual_colour_transform_flag = 0; s.num_ref_frames = 0;   /* refs 0: dec_buf = 1 */
  add(&s, 4); check_added(vec[nvec - 1], vlen[nvec - 1], "444 refs0");
  /* 7 emulation prevention bytes in the input: timing num_units_in_tick = 1, time_scale = 120 */
  s = base(100); s.vui_parameters_present_flag = 1; s.vui.timing_info_present_flag = 1; s.vui.num_units_in_tick = 1; s.vui.time_scale = 120;
  add(&s, 4);
  {
    int has03 = 0; for (size_t i = 5; i + 2 < vlen[nvec - 1]; i++) if (!vec[nvec - 1][i] && !vec[nvec - 1][i + 1] && vec[nvec - 1][i + 2] == 3) has03 = 1;
    assert(has03);
  }
  check_added(vec[nvec - 1], vlen[nvec - 1], "emulation bytes in input");
  /* the new fields themselves can need escaping: refs 0 gives dec_buf 1, the zero reorder code 1 follows 00 bits; run many sizes */
  for (int w = 0; w < 40; w++) {
    s = base(66); s.num_ref_frames = w % 17; s.pic_width_in_mbs_minus1 = w; s.pic_height_in_map_units_minus1 = w * 7;
    add(&s, w & 1 ? 3 : 4); check_added(vec[nvec - 1], vlen[nvec - 1], "sweep");
    nvec--;
  }
  int valid = nvec;

  /* 8 wrong NAL type, no start code, short, too long */
  {
    uint8_t o[H264_SPS_MAX_IN + H264_SPS_EXTRA], x[H264_SPS_MAX_IN + 8]; size_t on; h264_sps_info si;
    s = base(100); size_t n = gen(&s, 4, x);
    x[4] = 0x68; assert(h264_sps_patch(x, n, o, sizeof o, &on, &si) == H264_SPS_ERR);
    x[4] = 0xE7; assert(h264_sps_patch(x, n, o, sizeof o, &on, &si) == H264_SPS_ERR);   /* forbidden_zero_bit */
    n = gen(&s, 4, x); memmove(x, x + 4, n - 4);
    assert(h264_sps_patch(x, n - 4, o, sizeof o, &on, &si) == H264_SPS_ERR);   /* no start code */
    x[0] = 0; x[1] = 0; x[2] = 2; assert(h264_sps_patch(x, n - 4, o, sizeof o, &on, &si) == H264_SPS_ERR);
    for (size_t l = 0; l <= 4; l++) assert(h264_sps_patch(vec[0], l, o, sizeof o, &on, &si) == H264_SPS_ERR);
    uint8_t big[H264_SPS_MAX_IN + 1]; memcpy(big, vec[0], vlen[0]); memset(big + vlen[0], 0x55, sizeof big - vlen[0]);
    assert(h264_sps_patch(big, sizeof big, o, sizeof o, &on, &si) == H264_SPS_ERR);
    assert(h264_sps_patch(NULL, 10, o, sizeof o, &on, &si) == H264_SPS_ERR);
    assert(si.profile == 0 && si.vui == 0);   /* info is zeroed first */
  }
  /* 10 truncation: every prefix: no crash; ERR, or a result that parses */
  for (int v = 0; v < valid; v++) for (size_t l = 0; l <= vlen[v]; l++) {
    uint8_t o[H264_SPS_MAX_IN + H264_SPS_EXTRA], o2[H264_SPS_MAX_IN + H264_SPS_EXTRA]; size_t on, n2; h264_sps_info si;
    int r = h264_sps_patch(vec[v], l, o, sizeof o, &on, &si);
    if (r == H264_SPS_ADDED) {
      sps_t b; assert(oracle(o, on, &b) == 0 && b.vui.bitstream_restriction_flag == 1 && b.vui.num_reorder_frames == 0);
      assert(h264_sps_patch(o, on, o2, sizeof o2, &n2, &si) == H264_SPS_KEPT);
    }
  }
  /* 11 fixed-seed fuzz: random bodies, and mutated valid vectors */
  {
    uint32_t seed = 12345; unsigned added = 0, kept = 0;
#define RND() rnd(&seed)
    for (int it = 0; it < 30000; it++) {
      uint8_t in[H264_SPS_MAX_IN], o[H264_SPS_MAX_IN + H264_SPS_EXTRA + 1]; size_t n, on = 0; h264_sps_info si;
      if (it % 2) {
        int sc = 3 + (int)(RND() & 1); memset(in, 0, 4); in[sc - 1] = 1; in[sc] = 0x67;
        n = (size_t)sc + 1 + RND() % 60;
        for (size_t i = (size_t)sc + 1; i < n; i++) in[i] = (RND() & 3) ? (uint8_t)RND() : 0;
      } else {
        int v = (int)(RND() % (unsigned)valid); n = vlen[v]; memcpy(in, vec[v], n);
        for (int k = (int)(RND() % 3) + 1; k > 0; k--) { size_t at = RND() % n; in[at] ^= (uint8_t)(1u << (RND() & 7)); }
        if (RND() & 1) n -= RND() % 3;
      }
      int r = h264_sps_patch(in, n, o, sizeof o - 1, &on, &si);
      if (r == H264_SPS_ADDED) { assert(on <= n + H264_SPS_EXTRA && on > 0); no_emul(o, on); added++; }
      else if (r == H264_SPS_KEPT) kept++;
      else assert(r == H264_SPS_ERR);
    }
    printf("  fuzz: added=%u kept=%u\n", added, kept);
    assert(added > 100);
  }
  puts("h264_sps ok");
  return 0;
}
