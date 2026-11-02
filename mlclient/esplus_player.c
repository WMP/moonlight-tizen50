/*
 * esplus_player.c - one shared esplusplayer instance for both the video and audio
 * sinks (the TV allows only one player per process; a second create() deletes the
 * first). moonlight-common-c calls the sinks in this guaranteed order (from the
 * connection stages in Connection.c):
 *   AUDIO_STREAM_INIT (ar.init) -> VIDEO_STREAM_INIT (dr.setup)
 *   -> VIDEO_STREAM_START (dr.start) -> AUDIO_STREAM_START (ar.start)
 * so: first caller creates+opens the player; audio and video each add their
 * stream; the window is attached in video setup (it has the dimensions);
 * prepare+start runs from video start, by which point both streams are added.
 * This gives audio and video one clock (tight A/V sync), like the fork's EMSS path.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "esplusplayer_tv50.h"

/* EFL opaque decls (same as esppplay.c). */
int ecore_init(void);
int ecore_shutdown(void);
int ecore_wl2_init(void);
int ecore_wl2_shutdown(void);
void *ecore_wl2_display_connect(const char *name);
void ecore_wl2_display_disconnect(void *d);
void *ecore_wl2_window_new(void *d, void *parent, int x, int y, int w, int h);
void ecore_wl2_window_show(void *w);
void ecore_wl2_window_alpha_set(void *w, unsigned char alpha);
void ecore_wl2_window_free(void *w);
void ecore_wl2_display_flush(void *d);

#define PLOG(...) fprintf(stderr, "[esplus] " __VA_ARGS__)

static esplusplayer_handle s_pl;
static void *s_disp, *s_win;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int s_prepared, s_prepare_ok, s_started;

static void on_prepared(int ok) { s_prepare_ok = ok; s_prepared = 1; PLOG("prepare_async_done=%d\n", ok); }
static void on_ready(uint32_t st) { (void)st; }
static void on_error(uint32_t e) { PLOG("player error 0x%x\n", e); }
static void on_buffer(uint32_t st, uint32_t s) { if (s) PLOG("OVERRUN stream=%u\n", st); }

/* idempotent: create + open the player once. */
esplusplayer_handle esplus_create_open(void) {
  pthread_mutex_lock(&s_lock);
  if (!s_pl) {
    s_pl = esplusplayer_create();
    if (s_pl) {
      esplusplayer_set_error_cb(s_pl, on_error);
      esplusplayer_set_buffer_status_cb(s_pl, on_buffer);
      esplusplayer_set_ready_to_prepare_cb(s_pl, on_ready);
      esplusplayer_set_prepare_async_done_cb(s_pl, on_prepared);
      if (!esplusplayer_open(s_pl)) { PLOG("open failed\n"); }
      else PLOG("created+opened\n");
    } else PLOG("create failed\n");
  }
  pthread_mutex_unlock(&s_lock);
  return s_pl;
}

esplusplayer_handle esplus_handle(void) { return s_pl; }

/* video setup supplies the window (it has width/height). */
int esplus_set_window(int w, int h) {
  ecore_init();
  ecore_wl2_init();
  s_disp = ecore_wl2_display_connect(NULL);
  if (!s_disp) { PLOG("display_connect failed (XDG_RUNTIME_DIR/WAYLAND_DISPLAY?)\n"); return -1; }
  s_win = ecore_wl2_window_new(s_disp, NULL, 0, 0, w, h);
  if (!s_win) { PLOG("window_new failed\n"); return -1; }
  ecore_wl2_window_alpha_set(s_win, 1);
  ecore_wl2_window_show(s_win);
  ecore_wl2_display_flush(s_disp);
  if (!esplusplayer_set_ecore_display(s_pl, ESPLUSPLAYER_DISPLAY_TYPE_OVERLAY, s_win, 0, 0, w, h)) {
    PLOG("set_ecore_display failed\n"); return -1;
  }
  return 0;
}

/* called from video start, after both streams are added. */
void esplus_prepare(void) {
  if (!esplusplayer_prepare_async(s_pl)) PLOG("prepare_async failed\n");
}

/* submit + start-once-prepared; safe to call from the video and audio threads. */
int esplus_submit(const esplusplayer_es_packet *pkt) {
  pthread_mutex_lock(&s_lock);
  if (s_prepared && s_prepare_ok == 1 && !s_started) {
    s_started = 1; esplusplayer_start(s_pl); PLOG("started\n");
  }
  int r = esplusplayer_submit_packet(s_pl, pkt);
  pthread_mutex_unlock(&s_lock);
  return r;
}

void esplus_close(void) {
  pthread_mutex_lock(&s_lock);
  if (s_pl) { esplusplayer_stop(s_pl); esplusplayer_close(s_pl); s_pl = NULL; }
  pthread_mutex_unlock(&s_lock);
  if (s_win) { ecore_wl2_window_free(s_win); s_win = NULL; }
  if (s_disp) { ecore_wl2_display_disconnect(s_disp); s_disp = NULL; }
  ecore_wl2_shutdown(); ecore_shutdown();
}
