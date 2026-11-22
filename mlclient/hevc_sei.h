/*
 * hevc_sei.h - diagnostic scan of an Annex-B buffer for HEVC prefix SEI NAL units (header-only, pure C).
 * Not a parser: emulation prevention bytes are ignored; every read is bounded by the buffer.
 */
#ifndef HEVC_SEI_H
#define HEVC_SEI_H
#include <stddef.h>
#include <stdint.h>

/* Returns the number of prefix SEI NALs (type 39) in p[0..n); *mask |= 1 for payload 137 (mastering display), |= 2 for 144 (content light level). */
static inline int hevc_sei_scan(const uint8_t *p, size_t n, int *mask) {
  int cnt = 0;
  for (size_t i = 0; i + 3 < n; i++) {
    if (p[i] || p[i + 1] || p[i + 2] != 1) continue;
    size_t s = i + 3, e = s;   /* the NAL is [s, e): up to the next 00 00 01 or the end */
    while (e + 2 < n && !(p[e] == 0 && p[e + 1] == 0 && p[e + 2] == 1)) e++;
    if (e + 2 >= n) e = n;
    if (s + 2 <= e && ((p[s] >> 1) & 0x3f) == 39) {
      cnt++;
      size_t q = s + 2;
      while (q + 2 <= e && p[q] != 0x80) {
        unsigned type = 0, size = 0;
        while (q < e && p[q] == 0xFF) { type += 255; q++; }
        if (q >= e) break;
        type += p[q++];
        while (q < e && p[q] == 0xFF) { size += 255; q++; }
        if (q >= e) break;
        size += p[q++];
        if (type == 137) *mask |= 1;
        if (type == 144) *mask |= 2;
        if (size > e - q) break;
        q += size;
      }
    }
    i = e > i + 2 ? e - 1 : i + 2;   /* the next loop step lands on e (the next start code) */
  }
  return cnt;
}
#endif
