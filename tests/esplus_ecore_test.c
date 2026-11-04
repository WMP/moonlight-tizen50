/*
 * Host self-test for mlclient/esplus_player.c: the player/window setup keeps the
 * ecore/ecore_wl2 reference counts balanced on every path. An extra ecore_shutdown()
 * would shut down the .NET host's own ecore loop. EFL and esplusplayer are stubbed.
 * Run: sh tests/build-esplus-ecore-test.sh
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "esplus_stats.h"
#include "esplusplayer_tv50.h"
int ecore_cnt, wl2_cnt, disp_ok = 1, win_ok = 1, calls_off_main;
int ecore_init(void) { return ++ecore_cnt; }
int ecore_shutdown(void) { return --ecore_cnt; }
int ecore_wl2_init(void) { return ++wl2_cnt; }
int ecore_wl2_shutdown(void) { return --wl2_cnt; }
static int d, w;
void *ecore_wl2_display_connect(const char *n) { (void)n; return disp_ok ? &d : NULL; }
void ecore_wl2_display_disconnect(void *x) { (void)x; }
void *ecore_wl2_window_new(void *a, void *b, int x, int y, int ww, int hh) { (void)a;(void)b;(void)x;(void)y;(void)ww;(void)hh; return win_ok ? &w : NULL; }
void ecore_wl2_window_show(void *x) { (void)x; }
void ecore_wl2_window_alpha_set(void *x, unsigned char a) { (void)x;(void)a; }
void ecore_wl2_window_free(void *x) { (void)x; }
void ecore_wl2_display_flush(void *x) { (void)x; }
void *ecore_main_loop_thread_safe_call_sync(void *(*cb)(void *), void *data) { return cb(data); }
static int pl;
esplusplayer_handle esplusplayer_create(void) { return (esplusplayer_handle)&pl; }
int esplusplayer_open(esplusplayer_handle h) { (void)h; return 1; }
int esplusplayer_close(esplusplayer_handle h) { (void)h; return 1; }
int esplusplayer_stop(esplusplayer_handle h) { (void)h; return 1; }
static esplusplayer_prepare_async_done_cb cb_prepared;
static esplusplayer_error_cb cb_error;
static esplusplayer_buffer_status_cb cb_buffer;
static esplusplayer_resource_conflicted_cb cb_conflict;
void esplusplayer_set_error_cb(esplusplayer_handle h, esplusplayer_error_cb c) { (void)h; cb_error = c; }
void esplusplayer_set_buffer_status_cb(esplusplayer_handle h, esplusplayer_buffer_status_cb c) { (void)h; cb_buffer = c; }
void esplusplayer_set_resource_conflicted_cb(esplusplayer_handle h, esplusplayer_resource_conflicted_cb c) { (void)h; cb_conflict = c; }
void esplusplayer_set_ready_to_prepare_cb(esplusplayer_handle h, esplusplayer_ready_to_prepare_cb c) { (void)h;(void)c; }
void esplusplayer_set_prepare_async_done_cb(esplusplayer_handle h, esplusplayer_prepare_async_done_cb c) { (void)h; cb_prepared = c; }
int esplusplayer_prepare_async(esplusplayer_handle h) { (void)h; return 1; }
static uint64_t play_ms; static int play_ok = 1, play_calls;
int esplusplayer_get_playing_time(esplusplayer_handle h, uint64_t *t) { (void)h; play_calls++; *t = play_ms; return play_ok; }
int esplusplayer_start(esplusplayer_handle h) { (void)h; return 1; }
static int submit_ret = 4;
int esplusplayer_submit_packet(esplusplayer_handle h, const esplusplayer_es_packet *p) { (void)h;(void)p; return submit_ret; }
int esplusplayer_set_ecore_display(esplusplayer_handle h, esplusplayer_display_type t, void *win, int x, int y, int ww, int hh) { (void)h;(void)t;(void)win;(void)x;(void)y;(void)ww;(void)hh; return 1; }
extern esplusplayer_handle esplus_create_open(void);
extern int esplus_set_window(int, int);
extern void esplus_close(void);
extern int esplus_submit(const esplusplayer_es_packet *pkt);
extern void esplus_prepare(void);
static void vsub(uint64_t pts_ms) { esplusplayer_es_packet p = {ESPLUSPLAYER_STREAM_TYPE_VIDEO, NULL, 1, pts_ms * 1000000ull, 0}; esplus_submit(&p); }
static void asub(uint64_t pts_ms) { esplusplayer_es_packet p = {ESPLUSPLAYER_STREAM_TYPE_AUDIO, NULL, 1, pts_ms * 1000000ull, 0}; esplus_submit(&p); }
/* start a session on a stubbed, prepared and started player */
static void begin(void) { esplus_create_open(); cb_prepared(1); esplus_prepare(); play_ms = 0; play_ok = 1; vsub(0); }
int main(void) {
  /* the host app already holds one ecore reference */
  ecore_cnt = 1;
  /* 1: close without any window (setup failed before set_window) must not shut down ecore */
  esplus_create_open(); esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0);
  /* 2: success, then close */
  esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); assert(ecore_cnt == 2 && wl2_cnt == 1);
  esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0);
  /* 3: no display (service without display access) -> error, balanced, close is a no-op */
  disp_ok = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == -1); assert(ecore_cnt == 1 && wl2_cnt == 0);
  esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0); disp_ok = 1;
  /* 4: no window */
  win_ok = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == -1); esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0);
  /* 5: no player */
  assert(esplus_set_window(1280, 720) == -1 && ecore_cnt == 1);
  /* 6: counters. Only video SUCCESS counts as submitted; callbacks bump the rest. */
  win_ok = 1; esplus_create_open();
  assert(esplus_stats.submitted == 0 && cb_error && cb_buffer && cb_conflict);
  esplusplayer_es_packet v = {ESPLUSPLAYER_STREAM_TYPE_VIDEO, NULL, 1, 40000000ull, 0}, a = v;
  a.type = ESPLUSPLAYER_STREAM_TYPE_AUDIO;
  esplus_submit(&v); esplus_submit(&a); submit_ret = 3; esplus_submit(&v); submit_ret = 4;
  assert(esplus_stats.submitted == 1);
  cb_buffer(1, 0); cb_buffer(1, 1); cb_buffer(1, 1); cb_error(0x42); cb_error(0x43);
  assert(esplus_stats.underruns == 1 && esplus_stats.overruns == 2 && esplus_stats.errors == 2 && esplus_stats.last_error == 0x43);
  cb_conflict(); assert(esplus_stats.recovery_state == 2);
  esplus_close(); esplus_create_open(); assert(esplus_stats.submitted == 0 && esplus_stats.recovery_state == 0);   /* new session */
  esplus_close();
  /* a: depth and fps from the sampler tick (thread off) */
  setenv("MLENGINE_DEPTH_POLL_MS", "0", 1);
  begin();
  esplus_sample_tick(1000);                        /* first tick starts the fps second */
  for (int i = 1; i <= 60; i++) vsub(i * 16);      /* 60 + the begin() packet = 61 */
  asub(900); play_ms = 500;
  esplus_sample_tick(2000);
  assert(esplus_stats.playing_ms == 500 && esplus_stats.depth_ms == 960 - 500 && esplus_stats.audio_depth_ms == 400);
  assert(esplus_stats.fps_submitted == 60);
  play_ok = 0; play_ms = 9999; esplus_sample_tick(3000); assert(esplus_stats.playing_ms == 500);   /* failed read: unchanged */
  esplus_close();
  /* e: sampler thread starts at prepare, stops promptly at close */
  setenv("MLENGINE_DEPTH_POLL_MS", "50", 1);
  begin(); play_calls = 0;
  usleep(150000); assert(play_calls >= 1);
  esplus_close(); int n = play_calls; usleep(120000); assert(play_calls == n);
  unsetenv("MLENGINE_DEPTH_POLL_MS");
  puts("ecore balance ok"); return 0;
}
