/*
 * esppplay - feed an H.264 Annex-B file to libesplusplayer.so (Tizen 5.0 TV)
 * at a fixed frame rate and show it on an Ecore_Wl2 overlay window.
 *
 * usage: esppplay FILE.h264 [fps=60] [width=1920] [height=1080] [loops=0 (endless)]
 * Log lines on stderr:  SUBMIT <frame> pts_ms=<pts> ret=<status> t=<CLOCK_REALTIME ms>
 *
 * Build: see build-esppplay.sh. Self-test of the AU splitter on host:
 *   gcc -DSELFTEST -o /tmp/t esppplay.c && /tmp/t
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------- Annex-B access unit splitter ---------- */

typedef struct { size_t off, len; } au_t;

/* Returns offset of next start code (00 00 01) at or after p, or n. */
static size_t next_sc(const uint8_t *b, size_t p, size_t n) {
  for (; p + 3 <= n; p++)
    if (b[p] == 0 && b[p + 1] == 0 && b[p + 2] == 1) return p;
  return n;
}

/* Split into access units: new AU at AUD (9), or at SPS/PPS/SEI (7/8/6) or a
 * slice with first_mb_in_slice == 0 (1/5) once the current AU has a slice.
 * Leading zero bytes (4-byte start codes) stay with the next NAL. */
static size_t split_aus(const uint8_t *b, size_t n, au_t *out, size_t max) {
  size_t cnt = 0, au_start = 0;
  int have_vcl = 0, started = 0;
  for (size_t sc = next_sc(b, 0, n); sc < n;) {
    size_t nal = sc + 3, beg = sc;
    while (beg > 0 && b[beg - 1] == 0) beg--;
    if (nal >= n) break;
    int t = b[nal] & 0x1f;
    int vcl = (t == 1 || t == 5);
    int first_mb0 = vcl && nal + 1 < n && (b[nal + 1] & 0x80);
    int boundary = t == 9 || (have_vcl && (t == 7 || t == 8 || t == 6 || first_mb0));
    if (!started) { au_start = beg; started = 1; }
    else if (boundary) {
      if (cnt < max) out[cnt++] = (au_t){au_start, beg - au_start};
      au_start = beg;
      have_vcl = 0;
    }
    if (vcl) have_vcl = 1;
    sc = next_sc(b, nal, n);
  }
  if (started && n > au_start && cnt < max) out[cnt++] = (au_t){au_start, n - au_start};
  return cnt;
}

#ifdef SELFTEST
#include <assert.h>
int main(void) {
  /* AUD SPS PPS IDR(first_mb=0) IDR(first_mb!=0) | AUD P | P(no AUD, first_mb=0) */
  const uint8_t s[] = {0, 0, 0, 1, 9, 0xf0, 0, 0, 1, 7, 1, 0, 0, 1, 8, 2, 0, 0, 1, 5, 0x88,
                       0, 0, 1, 5, 0x40, 0, 0, 0, 1, 9, 0xf0, 0, 0, 1, 1, 0x9a, 0, 0, 1, 1, 0x9b};
  au_t a[8];
  size_t c = split_aus(s, sizeof s, a, 8);
  assert(c == 3);
  assert(a[0].off == 0 && a[0].len == 26);
  assert(a[1].off == 26 && a[1].len == 11);
  assert(a[2].off == 37 && a[2].len == sizeof s - 37);
  puts("selftest ok");
  return 0;
}
#else

#include "esplusplayer_tv50.h"

/* Minimal EFL declarations (EFL 1.20 on the TV), opaque pointers only. */
int ecore_init(void);
int ecore_shutdown(void);
void ecore_main_loop_iterate(void);
int ecore_wl2_init(void);
int ecore_wl2_shutdown(void);
void *ecore_wl2_display_connect(const char *name);
void ecore_wl2_display_disconnect(void *d);
void *ecore_wl2_window_new(void *d, void *parent, int x, int y, int w, int h);
void ecore_wl2_window_show(void *w);
void ecore_wl2_window_alpha_set(void *w, unsigned char alpha);
void ecore_wl2_window_free(void *w);
void ecore_wl2_display_flush(void *d);

static volatile sig_atomic_t g_quit;
static volatile int g_feed, g_prepared, g_prepare_result = -1;
static esplusplayer_handle g_pl;
static const uint8_t *g_buf;
static au_t *g_aus;
static size_t g_naus;
static unsigned g_fps = 60, g_loops;
static volatile uint64_t g_last_pts_ms;

static double now_ms(clockid_t c) {
  struct timespec ts;
  clock_gettime(c, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

#define LOG(...) (fprintf(stderr, "[%.3f] ", now_ms(CLOCK_MONOTONIC)), fprintf(stderr, __VA_ARGS__))
#define CHECK_BOOL(call) do { int r_ = (call); if (!r_) LOG("FAIL %s -> %d\n", #call, r_); else LOG("ok   %s\n", #call); } while (0)

static void on_error(uint32_t e) { LOG("CB error type=0x%x\n", e); }
static void on_buffer(uint32_t st, uint32_t s) { LOG("CB buffer_status stream=%u %s\n", st, s ? "OVERRUN" : "UNDERRUN"); }
static void on_conflict(void) { LOG("CB resource_conflicted\n"); g_quit = 1; }
static void on_eos(void) { LOG("CB eos\n"); g_quit = 1; }
static void on_ready(uint32_t st) { LOG("CB ready_to_prepare stream=%u\n", st); g_feed = 1; }
static void on_prepared(int ok) { LOG("CB prepare_async_done result=%d\n", ok); g_prepare_result = ok; g_prepared = 1; }

static void sleep_until(double t_ms) {
  struct timespec ts = {(time_t)(t_ms / 1e3), (long)((t_ms - (time_t)(t_ms / 1e3) * 1e3) * 1e6)};
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !g_quit) {}
}

static void *feeder(void *arg) {
  (void)arg;
  while (!g_feed && !g_quit) usleep(1000);
  const uint64_t frame_ns = 1000000000ull / g_fps;
  /* ESPP_PREROLL_STEP_MS: pts step before prepare_done. Frames queued for
   * preroll stay as constant latency after start, so a small step lets the
   * clock catch up with live submission. Default = normal frame step. */
  const char *e = getenv("ESPP_PREROLL_STEP_MS");
  const uint64_t pre_ns = e ? (uint64_t)(atof(e) * 1e6) : frame_ns;
  uint64_t pts = 0;
  double t0 = now_ms(CLOCK_MONOTONIC);
  unsigned full = 0;
  for (uint64_t f = 0; !g_quit; f++) {
    if (g_loops && f >= (uint64_t)g_loops * g_naus) break;
    const au_t *a = &g_aus[f % g_naus];
    esplusplayer_es_packet p = {ESPLUSPLAYER_STREAM_TYPE_VIDEO, (char *)g_buf + a->off,
                                (uint32_t)a->len, pts, frame_ns};
    pts += g_prepared ? frame_ns : pre_ns;
    sleep_until(t0 + f * 1e3 / g_fps);
    int r;
    while ((r = esplusplayer_submit_packet(g_pl, &p)) == ESPLUSPLAYER_SUBMIT_STATUS_FULL && !g_quit) {
      full++;
      usleep(2000);
    }
    LOG("SUBMIT %llu pts_ms=%llu len=%zu ret=%d full_retries=%u t=%.3f\n", (unsigned long long)f,
        (unsigned long long)(p.pts / 1000000), a->len, r, full, now_ms(CLOCK_REALTIME));
    full = 0;
    g_last_pts_ms = p.pts / 1000000;
    if (r != ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS && r != ESPLUSPLAYER_SUBMIT_STATUS_FULL)
      LOG("submit_packet error %d\n", r);
  }
  if (!g_quit) LOG("submit_eos -> %d\n", esplusplayer_submit_eos_packet(g_pl, ESPLUSPLAYER_STREAM_TYPE_VIDEO));
  return NULL;
}

static void on_sig(int s) { (void)s; g_quit = 1; }

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s FILE.h264 [fps=60] [width=1920] [height=1080] [loops=0]\n", argv[0]);
    return 2;
  }
  g_fps = argc > 2 ? (unsigned)atoi(argv[2]) : 60;
  unsigned w = argc > 3 ? (unsigned)atoi(argv[3]) : 1920, h = argc > 4 ? (unsigned)atoi(argv[4]) : 1080;
  g_loops = argc > 5 ? (unsigned)atoi(argv[5]) : 0;
  if (!g_fps) g_fps = 60;

  FILE *fp = fopen(argv[1], "rb");
  if (!fp) { perror(argv[1]); return 1; }
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  rewind(fp);
  uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
  if (n <= 0 || !buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { fprintf(stderr, "read failed\n"); return 1; }
  fclose(fp);
  g_buf = buf;
  g_aus = malloc(sizeof(au_t) * ((size_t)n / 4 + 1));
  g_naus = split_aus(buf, (size_t)n, g_aus, (size_t)n / 4 + 1);
  LOG("file %s: %ld bytes, %zu access units\n", argv[1], n, g_naus);
  if (!g_naus) return 1;

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  ecore_init();
  ecore_wl2_init();
  void *disp = ecore_wl2_display_connect(NULL);
  if (!disp) { LOG("ecore_wl2_display_connect failed (XDG_RUNTIME_DIR / WAYLAND_DISPLAY?)\n"); return 1; }
  void *win = ecore_wl2_window_new(disp, NULL, 0, 0, (int)w, (int)h);
  if (!win) { LOG("ecore_wl2_window_new failed\n"); return 1; }
  ecore_wl2_window_alpha_set(win, 1); /* transparent UI layer so the video plane below is visible */
  ecore_wl2_window_show(win);
  ecore_wl2_display_flush(disp);
  for (int i = 0; i < 10; i++) { ecore_main_loop_iterate(); usleep(10000); }

  g_pl = esplusplayer_create();
  LOG("esplusplayer_create -> %p\n", g_pl);
  if (!g_pl) return 1;
  esplusplayer_set_error_cb(g_pl, on_error);
  esplusplayer_set_buffer_status_cb(g_pl, on_buffer);
  esplusplayer_set_resource_conflicted_cb(g_pl, on_conflict);
  esplusplayer_set_eos_cb(g_pl, on_eos);
  esplusplayer_set_ready_to_prepare_cb(g_pl, on_ready);
  esplusplayer_set_prepare_async_done_cb(g_pl, on_prepared);

  CHECK_BOOL(esplusplayer_open(g_pl));
  CHECK_BOOL(esplusplayer_set_ecore_display(g_pl, ESPLUSPLAYER_DISPLAY_TYPE_OVERLAY, win, 0, 0, (int)w, (int)h));
  esplusplayer_video_stream_info vi = {NULL, 0, ESPLUSPLAYER_VIDEO_MIME_TYPE_H264, w, h, w, h, g_fps, 1};
  CHECK_BOOL(esplusplayer_add_video_stream_info(g_pl, &vi));

  pthread_t th;
  pthread_create(&th, NULL, feeder, NULL);
  CHECK_BOOL(esplusplayer_prepare_async(g_pl));

  int started = 0;
  double t_prep = now_ms(CLOCK_MONOTONIC);
  while (!g_quit) {
    ecore_main_loop_iterate();
    if (g_prepared && !started) {
      started = 1;
      LOG("prepare took %.1f ms\n", now_ms(CLOCK_MONOTONIC) - t_prep);
      if (g_prepare_result) CHECK_BOOL(esplusplayer_start(g_pl));
      else { LOG("prepare failed\n"); g_quit = 1; }
    }
    static double t_lat;
    if (started && now_ms(CLOCK_MONOTONIC) - t_lat >= 100) {
      uint64_t play = 0;
      t_lat = now_ms(CLOCK_MONOTONIC);
      int r = esplusplayer_get_playing_time(g_pl, &play);
      LOG("PLAYTIME ret=%d play=%llu last_pts_ms=%llu queue_ms=%lld\n", r, (unsigned long long)play,
          (unsigned long long)g_last_pts_ms, (long long)g_last_pts_ms - (long long)play);
    }
    if (!g_prepared && now_ms(CLOCK_MONOTONIC) - t_prep > 15000) { LOG("prepare timeout 15 s\n"); g_quit = 1; }
    usleep(2000);
  }

  LOG("shutting down\n");
  g_quit = 1;
  pthread_join(th, NULL);
  CHECK_BOOL(esplusplayer_stop(g_pl));
  CHECK_BOOL(esplusplayer_close(g_pl));
  ecore_wl2_window_free(win);
  ecore_wl2_display_disconnect(disp);
  ecore_wl2_shutdown();
  ecore_shutdown();
  free(g_aus);
  free(buf);
  return 0;
}
#endif
