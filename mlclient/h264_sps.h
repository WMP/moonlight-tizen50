/*
 * h264_sps.h - add a missing VUI bitstream_restriction to an H.264 SPS (max_num_reorder_frames = 0),
 * so the TV decoder does not wait for the largest reorder delay of the level before the first output.
 * Pure C, no engine headers, no malloc, no globals. Host self-test: tests/h264_sps_test.c.
 */
#ifndef H264_SPS_H
#define H264_SPS_H
#include <stddef.h>
#include <stdint.h>

/* what the parse saw; reorder/dec_buf are the ORIGINAL values and valid only when restr != 0 */
typedef struct { int profile, level, ref_frames, poc_type, vui, restr, reorder, dec_buf; } h264_sps_info;
enum { H264_SPS_ERR = -1, H264_SPS_KEPT = 0, H264_SPS_ADDED = 1 };
#define H264_SPS_MAX_IN 256   /* longer input: ERR */
#define H264_SPS_EXTRA  32    /* the patched NAL is at most this much longer than the input */

/* in: ONE Annex-B SPS NAL (start code 00 00 01 or 00 00 00 01, NAL header 0x67-style, escaped payload).
 * KEPT: the SPS already has a restriction (never modified); out is not written.
 * ADDED: out and the value of out_len hold the complete new NAL.
 * ERR: any doubt (bad input, truncated, scaling lists, out_cap too small); out is undefined, use the original.
 * info is always zeroed first and filled as far as the parse got. */
int h264_sps_patch(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len, h264_sps_info *info);

#endif
