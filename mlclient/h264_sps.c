/* h264_sps.c - see h264_sps.h. Parse order: H.264 spec 7.3.2.1.1 and E.1.1. */
#include <string.h>
#include "h264_sps.h"

typedef struct { const uint8_t *d; uint32_t pos, n; int err; } br_t;   /* n, pos in bits; err is sticky */
typedef struct { uint8_t *d; uint32_t pos, cap; int err; } bw_t;

static uint32_t br_u(br_t *r, int bits) {
  uint32_t v = 0;
  for (int i = 0; i < bits; i++) {
    if (r->pos >= r->n) { r->err = 1; return 0; }
    v = (v << 1) | ((r->d[r->pos >> 3] >> (7 - (r->pos & 7))) & 1u);
    r->pos++;
  }
  return v;
}
/* ue(v), also used to skip se(v) (same bit layout); more than 31 leading zeros: error */
static uint32_t br_ue(br_t *r) {
  int lz = 0;
  while (!br_u(r, 1)) { if (r->err || ++lz > 31) { r->err = 1; return 0; } }
  return lz ? ((1u << lz) - 1u) + br_u(r, lz) : 0;
}
static void br_skip(br_t *r, int bits) { br_u(r, bits); }
/* ue(v) with an upper limit */
static uint32_t br_ue_max(br_t *r, uint32_t max) {
  uint32_t v = br_ue(r);
  if (v > max) r->err = 1;
  return r->err ? 0 : v;
}

static void bw_bit(bw_t *w, unsigned b) {
  if (w->pos >= w->cap * 8u) { w->err = 1; return; }
  if (b) w->d[w->pos >> 3] |= (uint8_t)(0x80u >> (w->pos & 7)); else w->d[w->pos >> 3] &= (uint8_t)~(0x80u >> (w->pos & 7));
  w->pos++;
}
static void bw_u(bw_t *w, uint32_t v, int bits) { for (int i = bits - 1; i >= 0; i--) bw_bit(w, (v >> i) & 1u); }
static void bw_ue(bw_t *w, uint32_t v) {   /* v is small here */
  uint32_t x = v + 1; int k = 0;
  while ((x >> k) > 1u) k++;
  bw_u(w, 0, k); bw_u(w, x, k + 1);
}

static void hrd(br_t *r) {
  uint32_t cnt = br_ue_max(r, 31);
  br_skip(r, 8);
  for (uint32_t i = 0; i <= cnt && !r->err; i++) { br_ue(r); br_ue(r); br_skip(r, 1); }
  br_skip(r, 20);
}

static int is_high(uint32_t p) {
  static const uint8_t hp[] = { 100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135 };
  for (size_t i = 0; i < sizeof hp; i++) if (hp[i] == p) return 1;
  return 0;
}

int h264_sps_patch(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len, h264_sps_info *info) {
  if (info) memset(info, 0, sizeof *info);
  if (out_len) *out_len = 0;
  if (!in || !out || !out_len || !info || in_len > H264_SPS_MAX_IN) return H264_SPS_ERR;
  size_t sc;
  if (in_len >= 4 && !in[0] && !in[1] && in[2] == 1) sc = 3;
  else if (in_len >= 5 && !in[0] && !in[1] && !in[2] && in[3] == 1) sc = 4;
  else return H264_SPS_ERR;
  uint8_t hdr = in[sc];
  if ((hdr & 0x80) || (hdr & 0x1f) != 7) return H264_SPS_ERR;

  /* un-escape the payload */
  uint8_t rb[H264_SPS_MAX_IN], wb[H264_SPS_MAX_IN + 16];
  size_t n = 0; int zeros = 0;
  for (size_t i = sc + 1; i < in_len; i++) {
    if (zeros >= 2 && in[i] == 3) { zeros = 0; continue; }
    zeros = in[i] ? 0 : zeros + 1;
    rb[n++] = in[i];
  }
  br_t r = { rb, 0, (uint32_t)n * 8u, 0 };

  uint32_t profile = br_u(&r, 8); br_skip(&r, 8); uint32_t level = br_u(&r, 8);
  br_ue_max(&r, 31);
  if (is_high(profile)) {
    if (br_ue_max(&r, 3) == 3) br_skip(&r, 1);
    br_ue_max(&r, 6); br_ue_max(&r, 6); br_skip(&r, 1);
    if (br_u(&r, 1)) return H264_SPS_ERR;   /* scaling lists: not parsed */
  }
  br_ue_max(&r, 12);
  uint32_t poc = br_ue_max(&r, 2);
  if (poc == 0) br_ue_max(&r, 12);
  else if (poc == 1) {
    br_skip(&r, 1); br_ue(&r); br_ue(&r);
    uint32_t c = br_ue_max(&r, 255);
    for (uint32_t i = 0; i < c && !r.err; i++) br_ue(&r);
  }
  uint32_t refs = br_ue_max(&r, 16);
  br_skip(&r, 1); br_ue_max(&r, 8191); br_ue_max(&r, 8191);
  if (!br_u(&r, 1)) br_skip(&r, 1);   /* frame_mbs_only_flag 0: mb_adaptive_frame_field_flag */
  br_skip(&r, 1);
  if (br_u(&r, 1)) { br_ue(&r); br_ue(&r); br_ue(&r); br_ue(&r); }
  info->profile = (int)profile; info->level = (int)level; info->ref_frames = (int)refs; info->poc_type = (int)poc;
  if (r.err) return H264_SPS_ERR;
  uint32_t cut = r.pos;   /* the vui_parameters_present_flag */
  info->vui = (int)br_u(&r, 1);
  if (r.err) return H264_SPS_ERR;
  if (info->vui) {
    if (br_u(&r, 1) && br_u(&r, 8) == 255) br_skip(&r, 32);   /* aspect ratio: idc 255 = extended SAR */
    if (br_u(&r, 1)) br_skip(&r, 1);
    if (br_u(&r, 1)) { br_skip(&r, 4); if (br_u(&r, 1)) br_skip(&r, 24); }
    if (br_u(&r, 1)) { br_ue(&r); br_ue(&r); }
    if (br_u(&r, 1)) br_skip(&r, 65);
    uint32_t nal = br_u(&r, 1); if (nal) hrd(&r);
    uint32_t vcl = br_u(&r, 1); if (vcl) hrd(&r);
    if (nal || vcl) br_skip(&r, 1);
    br_skip(&r, 1);   /* pic_struct_present_flag */
    cut = r.pos;      /* the bitstream_restriction_flag */
    info->restr = (int)br_u(&r, 1);
    if (r.err) return H264_SPS_ERR;
    if (info->restr) {
      br_skip(&r, 1); br_ue(&r); br_ue(&r); br_ue(&r); br_ue(&r);
      uint32_t ro = br_ue(&r), db = br_ue(&r);
      if (r.err || ro > 0x7fffffffu || db > 0x7fffffffu) return H264_SPS_ERR;
      info->reorder = (int)ro; info->dec_buf = (int)db;
      return H264_SPS_KEPT;
    }
  }
  /* the rest must be rbsp_trailing_bits (a 1, then zeros): anything else means the parse went wrong */
  if (br_u(&r, 1) != 1) return H264_SPS_ERR;
  while (r.pos < r.n) if (br_u(&r, 1)) return H264_SPS_ERR;
  if (r.err) return H264_SPS_ERR;

  /* copy bits [0, cut) as they are, then append the new fields */
  bw_t w = { wb, 0, sizeof wb, 0 };
  memset(wb, 0, sizeof wb);
  for (uint32_t i = 0; i < cut; i++) bw_bit(&w, (rb[i >> 3] >> (7 - (i & 7))) & 1u);
  bw_bit(&w, 1);                                   /* vui_parameters_present_flag, or the flag we cut */
  if (!info->vui) bw_u(&w, 0, 8);                  /* no aspect, overscan, video_signal, chroma_loc, timing, nal_hrd, vcl_hrd, pic_struct */
  if (!info->vui) bw_bit(&w, 1);                   /* bitstream_restriction_flag */
  bw_bit(&w, 1); bw_ue(&w, 2); bw_ue(&w, 1); bw_ue(&w, 16); bw_ue(&w, 16);   /* the values the spec infers when absent */
  bw_ue(&w, 0); bw_ue(&w, refs > 1 ? refs : 1);    /* max_num_reorder_frames, max_dec_frame_buffering */
  bw_bit(&w, 1);                                   /* rbsp_trailing_bits */
  while (w.pos & 7) bw_bit(&w, 0);
  if (w.err) return H264_SPS_ERR;

  /* escape into out; check out_cap on every byte */
  size_t o = 0, wn = w.pos / 8;
  if (out_cap < sc + 1) return H264_SPS_ERR;
  memcpy(out, in, sc + 1); o = sc + 1;
  zeros = 0;
  for (size_t i = 0; i < wn; i++) {
    if (zeros >= 2 && wb[i] <= 3) { if (o >= out_cap) return H264_SPS_ERR; out[o++] = 3; zeros = 0; }
    if (o >= out_cap) return H264_SPS_ERR;
    out[o++] = wb[i];
    zeros = wb[i] ? 0 : zeros + 1;
  }
  if (o > in_len + H264_SPS_EXTRA) return H264_SPS_ERR;
  *out_len = o;
  return H264_SPS_ADDED;
}
