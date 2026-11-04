/* mldiag.c - see mldiag.h. Host self-test: tests/mldiag_test.c. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mldiag.h"

#define LINE_MAX_CC 160
#define FIRST_N 4
#define LAST_N 8
#define OUT_MAX 2048

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static mldiag_cfg s_cfg;
static char s_appver[64], s_gfever[64];
static long long s_t0;
static char s_first[FIRST_N][LINE_MAX_CC + 1], s_last[LAST_N][LINE_MAX_CC + 1];
static int s_nlines;                      /* total cc lines seen */
static char s_out[OUT_MAX]; static int s_code;
static int s_vfmt, s_vw, s_vh, s_vfps, s_vrc, s_vset;
static int s_open = -99, s_win = -99, s_prep = -99, s_pdone = -99, s_start = -99;   /* -99 = not reached */
static int s_first_pkt_ms = -1, s_sub_last_bad;
static long long s_first_du_ms = -1;

/* hot counters: __atomic */
enum { C_UNREC, C_DROP, C_WAITIDR, C_BADLEN, C_DECRYPT, C_CORRUPT, C_UNKFT, C_IDRREQ, C_FEC, C_QOVF,
       C_DROPLIM, C_DU, C_IDR, C_VSUB, C_ASUB, C_SUBFAIL, C_N };
static unsigned s_c[C_N];
#define INC(i) __atomic_fetch_add(&s_c[i], 1, __ATOMIC_RELAXED)
#define GET(i) __atomic_load_n(&s_c[i], __ATOMIC_RELAXED)

static long long now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void mldiag_reset(const mldiag_cfg *cfg) {
  pthread_mutex_lock(&s_mu);
  memset(s_c, 0, sizeof s_c);
  memset(s_first, 0, sizeof s_first); memset(s_last, 0, sizeof s_last);
  s_nlines = 0; s_out[0] = 0; s_code = 0;
  s_vset = 0; s_open = s_win = s_prep = s_pdone = s_start = -99; s_first_pkt_ms = -1; s_sub_last_bad = 0; s_first_du_ms = -1;
  memset(&s_cfg, 0, sizeof s_cfg);
  if (cfg) {
    s_cfg = *cfg;
    snprintf(s_appver, sizeof s_appver, "%s", cfg->app_version ? cfg->app_version : "");
    snprintf(s_gfever, sizeof s_gfever, "%s", cfg->gfe_version ? cfg->gfe_version : "");
  } else s_appver[0] = s_gfever[0] = 0;
  s_cfg.app_version = s_appver; s_cfg.gfe_version = s_gfever;
  s_t0 = now_ms();
  pthread_mutex_unlock(&s_mu);
}

void mldiag_cc_line(const char *line) {
  if (!line) return;
  char b[LINE_MAX_CC + 1];
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
  if (n > LINE_MAX_CC) n = LINE_MAX_CC;
  memcpy(b, line, n); b[n] = 0;
  static const char FP[] = "Received first video packet after ";
  const char *fp = strstr(b, FP);
  int ms;
  if (fp && sscanf(fp + sizeof FP - 1, "%d", &ms) == 1) __atomic_store_n(&s_first_pkt_ms, ms, __ATOMIC_RELAXED);
  else if (strstr(b, "Unrecoverable frame")) INC(C_UNREC);
  else if (strstr(b, "Network dropped")) INC(C_DROP);
  else if (strstr(b, "Waiting for IDR frame")) INC(C_WAITIDR);
  else if (strstr(b, "Invalid last payload length")) INC(C_BADLEN);
  else if (strstr(b, "Failed to decrypt video packet")) INC(C_DECRYPT);
  else if (strstr(b, "Depacketizer detected corrupt frame") || strstr(b, "FEC recovery returned corrupt") ||
           strstr(b, "Recovery error")) INC(C_CORRUPT);
  else if (strstr(b, "Unrecognized frame type")) INC(C_UNKFT);
  else if (strstr(b, "Requesting IDR frame") || strstr(b, "Sending RFI request") ||
           strstr(b, "Sending speculative RFI")) INC(C_IDRREQ);
  else if (strstr(b, "Recovered ")) INC(C_FEC);
  else if (strstr(b, "Video decode unit queue overflow")) INC(C_QOVF);
  else if (strstr(b, "Reached consecutive drop limit")) INC(C_DROPLIM);
  pthread_mutex_lock(&s_mu);
  if (s_nlines < FIRST_N) memcpy(s_first[s_nlines], b, n + 1);
  memcpy(s_last[s_nlines % LAST_N], b, n + 1);
  s_nlines++;
  pthread_mutex_unlock(&s_mu);
}

void mldiag_video_setup(int fmt, int w, int h, int fps, int rc) {
  pthread_mutex_lock(&s_mu);
  s_vfmt = fmt; s_vw = w; s_vh = h; s_vfps = fps; s_vrc = rc; s_vset = 1;
  pthread_mutex_unlock(&s_mu);
}

void mldiag_video_du(int is_idr) {
  INC(C_DU);
  if (is_idr) INC(C_IDR);
  pthread_mutex_lock(&s_mu);   /* once per frame: cheap */
  if (s_first_du_ms < 0) s_first_du_ms = now_ms() - s_t0;
  pthread_mutex_unlock(&s_mu);
}

void mldiag_submit(int is_video, int status) {
  INC(is_video ? C_VSUB : C_ASUB);
  if (status != 0) { INC(C_SUBFAIL); __atomic_store_n(&s_sub_last_bad, status, __ATOMIC_RELAXED); }
}

void mldiag_player(const char *what, int rc) {
  pthread_mutex_lock(&s_mu);
  if (!strcmp(what, "open")) s_open = rc; else if (!strcmp(what, "pdone")) s_pdone = rc;
  else if (!strcmp(what, "win")) s_win = rc; else if (!strcmp(what, "prep")) s_prep = rc; else if (!strcmp(what, "start")) s_start = rc;
  pthread_mutex_unlock(&s_mu);
}

static const char *code_name(int c) {
  switch (c) {
    case 0: return "GRACEFUL"; case -100: return "NO_VIDEO_TRAFFIC"; case -101: return "NO_VIDEO_FRAME";
    case -102: return "UNEXPECTED_EARLY_TERMINATION"; case -103: return "PROTECTED_CONTENT";
    case -104: return "FRAME_CONVERSION"; default: return "OTHER";
  }
}

/* snprintf that advances and never overruns */
#define ADD(...) do { if (o < OUT_MAX - 1) { int r_ = snprintf(s_out + o, OUT_MAX - o, __VA_ARGS__); \
                      if (r_ > 0) { o += r_; if (o > OUT_MAX - 1) o = OUT_MAX - 1; } } } while (0)

static void add_lines(int *po, char (*ring)[LINE_MAX_CC + 1], int ringn, int start, int count) {
  int o = *po;
  for (int i = 0; i < count; i++) ADD("%s%s", i ? " ; " : "", ring[(start + i) % ringn]);
  *po = o;
}

void mldiag_fail(int code, int stage, const char *stage_name, int host_feature_flags,
                 int rtt, int rtt_var, unsigned player_errors, unsigned player_last_error) {
  long long t = now_ms();
  pthread_mutex_lock(&s_mu);
  int o = 0;
  s_out[0] = 0;
  ADD("ML-ERROR code=%d name=%s stage=%s", code, code_name(code), stage >= 0 && stage_name ? stage_name : "-");
  ADD(" t=%lldms | cfg %dx%d@%d br=%d pkt=%d fmts=0x%x cs=%d cr=%d audio=0x%x scm=0x%x enc=0x%x app=%s gfe=%s hostflags=0x%x rtt=%d/%d",
      t - s_t0, s_cfg.width, s_cfg.height, s_cfg.fps, s_cfg.bitrate_kbps, s_cfg.packet_size,
      s_cfg.supported_video_formats, s_cfg.color_space, s_cfg.color_range, s_cfg.audio_config,
      s_cfg.server_codec_mode_support, s_cfg.encryption_flags, s_appver, s_gfever,
      host_feature_flags, rtt, rtt_var);
  long long fdu = s_first_du_ms;
  char fduS[24], fpS[24];
  if (fdu < 0) snprintf(fduS, sizeof fduS, "-"); else snprintf(fduS, sizeof fduS, "%lld", fdu);
  if (s_first_pkt_ms < 0) snprintf(fpS, sizeof fpS, "-"); else snprintf(fpS, sizeof fpS, "%d", s_first_pkt_ms);
  if (s_vset) ADD(" | video setup=0x%x %dx%d@%d rc=%d", s_vfmt, s_vw, s_vh, s_vfps, s_vrc);
  else ADD(" | video setup=none");
  ADD(" du=%u idr=%u first_du_ms=%s first_pkt_ms=%s", GET(C_DU), GET(C_IDR), fduS, fpS);
  ADD(" | player open=%d win=%d prep=%d pdone=%d start=%d vsub=%u asub=%u subfail=%u/%d err=%u/%u", s_open, s_win, s_prep, s_pdone, s_start,
      GET(C_VSUB), GET(C_ASUB), GET(C_SUBFAIL), s_sub_last_bad, player_errors, player_last_error);
  ADD(" | cc unrec=%u drop=%u waitidr=%u badlen=%u decrypt=%u corrupt=%u unkft=%u idrreq=%u fec=%u qovf=%u droplim=%u",
      GET(C_UNREC), GET(C_DROP), GET(C_WAITIDR), GET(C_BADLEN), GET(C_DECRYPT), GET(C_CORRUPT), GET(C_UNKFT),
      GET(C_IDRREQ), GET(C_FEC), GET(C_QOVF), GET(C_DROPLIM));
  int nf = s_nlines < FIRST_N ? s_nlines : FIRST_N;
  ADD(" | cc-first: ");
  add_lines(&o, s_first, FIRST_N, 0, nf);
  int nl = s_nlines < LAST_N ? s_nlines : LAST_N;
  ADD(" | cc-last: ");
  add_lines(&o, s_last, LAST_N, s_nlines - nl, nl);
  for (char *p = s_out; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
  s_code = code ? code : -1;   /* a failure always reports non-zero */
  s_out[OUT_MAX - 1] = 0;
  fprintf(stderr, "[ml] %s\n", s_out);
  pthread_mutex_unlock(&s_mu);
}

int mldiag_last(char *buf, int len) {
  pthread_mutex_lock(&s_mu);
  int c = s_code;
  if (buf && len > 0) { snprintf(buf, (size_t)len, "%s", s_out); }
  pthread_mutex_unlock(&s_mu);
  return c;
}
