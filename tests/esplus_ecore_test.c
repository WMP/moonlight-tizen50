/*
 * Host self-test for mlclient/esplus_player.c: the player/window setup keeps the
 * ecore/ecore_wl2 reference counts balanced on every path. An extra ecore_shutdown()
 * would shut down the .NET host's own ecore loop. EFL and esplusplayer are stubbed.
 * Run: sh tests/build-esplus-ecore-test.sh
 */
#include <assert.h>
#include <stdio.h>
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
void esplusplayer_set_error_cb(esplusplayer_handle h, esplusplayer_error_cb c) { (void)h;(void)c; }
void esplusplayer_set_buffer_status_cb(esplusplayer_handle h, esplusplayer_buffer_status_cb c) { (void)h;(void)c; }
void esplusplayer_set_ready_to_prepare_cb(esplusplayer_handle h, esplusplayer_ready_to_prepare_cb c) { (void)h;(void)c; }
void esplusplayer_set_prepare_async_done_cb(esplusplayer_handle h, esplusplayer_prepare_async_done_cb c) { (void)h;(void)c; }
int esplusplayer_prepare_async(esplusplayer_handle h) { (void)h; return 1; }
int esplusplayer_start(esplusplayer_handle h) { (void)h; return 1; }
int esplusplayer_submit_packet(esplusplayer_handle h, const esplusplayer_es_packet *p) { (void)h;(void)p; return 4; }
int esplusplayer_set_ecore_display(esplusplayer_handle h, esplusplayer_display_type t, void *win, int x, int y, int ww, int hh) { (void)h;(void)t;(void)win;(void)x;(void)y;(void)ww;(void)hh; return 1; }
extern esplusplayer_handle esplus_create_open(void);
extern int esplus_set_window(int, int);
extern void esplus_close(void);
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
  puts("ecore balance ok"); return 0;
}
