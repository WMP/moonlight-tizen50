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
#include <stdlib.h>
#include <string.h>

#include "esplus_stats.h"
#include "esplusplayer_tv50.h"
#include "mltrace.h"

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
void *ecore_main_loop_thread_safe_call_sync(void *(*cb)(void *data), void *data);

#define PLOG(...) fprintf(stderr, "[esplus] " __VA_ARGS__)

static esplusplayer_handle s_pl;
static void *s_disp, *s_win;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int s_prepared, s_prepare_ok, s_started;

esplus_stats_t esplus_stats;
esplus_cfg_t esplus_cfg;
static volatile int s_conflicted;   /* set by the resource-conflicted callback; cleared per session */

static int mlc_env_int(const char *name, int def) {
  const char *v = getenv(name);
  char *end;
  if (!v || !*v) return def;
  long n = strtol(v, &end, 10);
  return *end ? def : (int)n;
}

static void load_cfg(void) {
  const char *m = getenv("MLENGINE_LATENCY_MODE");
  esplus_cfg.latency_mode = !m ? ESPLUS_LAT_OFF : !strcmp(m, "nudge") ? ESPLUS_LAT_NUDGE
                          : !strcmp(m, "arrival") ? ESPLUS_LAT_ARRIVAL : ESPLUS_LAT_OFF;
  esplus_cfg.target_depth_ms = mlc_env_int("MLENGINE_TARGET_DEPTH_MS", 50);
  esplus_cfg.nudge_max_ms_per_s = mlc_env_int("MLENGINE_NUDGE_MAX_MS_PER_S", 10);
  esplus_cfg.drop_after_ms = mlc_env_int("MLENGINE_DROP_AFTER_MS", 0);
  esplus_cfg.depth_poll_ms = mlc_env_int("MLENGINE_DEPTH_POLL_MS", 250);
  esplus_cfg.stall_ms = mlc_env_int("MLENGINE_STALL_MS", 700);
  /* 0 = never recover. Recommended 3, once validated on a TV. */
  esplus_cfg.recover_max = mlc_env_int("MLENGINE_RECOVER_MAX", 0);
  esplus_cfg.recover_back_ms = mlc_env_int("MLENGINE_RECOVER_BACK_MS", 250);
  esplus_cfg.idr_cooldown_ms = mlc_env_int("MLENGINE_IDR_COOLDOWN_MS", 500);
  MLT("cfg latency=%d target=%d nudge=%d drop_after=%d poll=%d stall=%d recover_max=%d back=%d idr_cd=%d",
      esplus_cfg.latency_mode, esplus_cfg.target_depth_ms, esplus_cfg.nudge_max_ms_per_s, esplus_cfg.drop_after_ms,
      esplus_cfg.depth_poll_ms, esplus_cfg.stall_ms, esplus_cfg.recover_max, esplus_cfg.recover_back_ms,
      esplus_cfg.idr_cooldown_ms);
}

static int s_ecore;   /* 1 while we hold an ecore_init()/ecore_wl2_init() reference */

static void on_prepared(int ok) { s_prepare_ok = ok; s_prepared = 1; PLOG("prepare_async_done=%d\n", ok); MLT("cb prepare_async_done ok=%d", ok); }
static void on_ready(uint32_t st) { (void)st; MLT("cb ready_to_prepare stream=%u", st); }
/* Player callbacks may run on player threads while we hold s_lock inside a player call:
 * they must never take s_lock or call the player; atomics and flags only. */
static void on_error(uint32_t e) {
  __atomic_fetch_add(&esplus_stats.errors, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&esplus_stats.last_error, e, __ATOMIC_RELAXED);
  PLOG("player error 0x%x\n", e); MLT("cb player error 0x%x", e);
}
static void on_buffer(uint32_t st, uint32_t s) {
  if (s == 0) __atomic_fetch_add(&esplus_stats.underruns, 1, __ATOMIC_RELAXED);
  else if (s == 1) __atomic_fetch_add(&esplus_stats.overruns, 1, __ATOMIC_RELAXED);
  if (s) PLOG("OVERRUN stream=%u\n", st);
}
static void on_conflict(void) {
  s_conflicted = 1; esplus_stats.recovery_state = 2;
  PLOG("resource conflict\n"); MLT("cb resource_conflicted");
}

/* idempotent: create + open the player once. */
esplusplayer_handle esplus_create_open(void) {
  pthread_mutex_lock(&s_lock);
  if (!s_pl) {
    load_cfg();
    memset((void *)&esplus_stats, 0, sizeof esplus_stats);   /* new session */
    s_conflicted = 0;
    s_pl = esplusplayer_create();
    MLT("esplusplayer_create -> %p", (void *)s_pl);
    if (s_pl) {
      esplusplayer_set_error_cb(s_pl, on_error);
      esplusplayer_set_buffer_status_cb(s_pl, on_buffer);
      esplusplayer_set_resource_conflicted_cb(s_pl, on_conflict);
      esplusplayer_set_ready_to_prepare_cb(s_pl, on_ready);
      esplusplayer_set_prepare_async_done_cb(s_pl, on_prepared);
      int ok = esplusplayer_open(s_pl);
      MLT("esplusplayer_open -> %d", ok);
      if (!ok) { PLOG("open failed\n"); }
      else PLOG("created+opened\n");
    } else PLOG("create failed\n");
  }
  MLT("esplus_create_open -> %p", (void *)s_pl);
  pthread_mutex_unlock(&s_lock);
  return s_pl;
}

esplusplayer_handle esplus_handle(void) { return s_pl; }

/* EFL is not thread-safe, and LiStartConnection runs video setup on the caller's
 * thread (a worker thread in the .NET host). So the Wayland display/window work runs
 * on the ecore main loop (the host's app loop; called directly when the caller is
 * already the main loop thread, as in the CLI). */
struct win_req { int w, h, rc; };

static void *win_create_main(void *arg) {
  struct win_req *r = arg;
  MLT("win: XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)",
      getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
  int n = ecore_wl2_init();
  MLT("ecore_wl2_init -> %d", n);
  if (n <= 0) { r->rc = -1; return NULL; }
  s_ecore = 1;
  s_disp = ecore_wl2_display_connect(NULL);
  MLT("ecore_wl2_display_connect -> %p", s_disp);
  if (!s_disp) { PLOG("display_connect failed (XDG_RUNTIME_DIR/WAYLAND_DISPLAY?)\n"); r->rc = -1; return NULL; }
  s_win = ecore_wl2_window_new(s_disp, NULL, 0, 0, r->w, r->h);
  MLT("ecore_wl2_window_new -> %p", s_win);
  if (!s_win) { PLOG("window_new failed\n"); r->rc = -1; return NULL; }
  ecore_wl2_window_alpha_set(s_win, 1);
  ecore_wl2_window_show(s_win);
  ecore_wl2_display_flush(s_disp);
  MLT("window shown");
  int ok = esplusplayer_set_ecore_display(s_pl, ESPLUSPLAYER_DISPLAY_TYPE_OVERLAY, s_win, 0, 0, r->w, r->h);
  MLT("esplusplayer_set_ecore_display(OVERLAY) -> %d", ok);
  if (!ok) { PLOG("set_ecore_display failed\n"); r->rc = -2; return NULL; }
  r->rc = 0;
  return NULL;
}

static void *win_destroy_main(void *arg) {
  (void)arg;
  if (s_win) { ecore_wl2_window_free(s_win); s_win = NULL; }
  if (s_disp) { ecore_wl2_display_disconnect(s_disp); s_disp = NULL; }
  if (s_ecore) { ecore_wl2_shutdown(); s_ecore = 0; }
  MLT("window destroyed");
  return NULL;
}

/* video setup supplies the window (it has width/height). */
int esplus_set_window(int w, int h) {
  MLT("esplus_set_window %dx%d pl=%p", w, h, (void *)s_pl);
  if (!s_pl) { MLT("esplus_set_window -> -1 (no player)"); return -1; }
  if (w <= 0 || h <= 0) { MLT("esplus_set_window -> -1 (size)"); return -1; }
  /* ecore_init() is cheap and refcounted; in the host the app has already done it.
     It must precede the thread-safe call (the CLI has no ecore yet). */
  int n = ecore_init();
  MLT("ecore_init -> %d", n);
  if (n <= 0) { MLT("esplus_set_window -> -1 (ecore_init)"); return -1; }
  struct win_req r = { w, h, -1 };
  MLT("window: running on the main loop");
  ecore_main_loop_thread_safe_call_sync(win_create_main, &r);
  if (r.rc != 0) {
    ecore_main_loop_thread_safe_call_sync(win_destroy_main, NULL);
    ecore_shutdown();
    MLT("esplus_set_window -> -1 (%s)", r.rc == -2 ? "set_ecore_display" : "no window: a service may have no display access");
    return -1;
  }
  MLT("esplus_set_window -> 0");
  return 0;
}

/* called from video start, after both streams are added. */
void esplus_prepare(void) {
  int ok = esplusplayer_prepare_async(s_pl);
  MLT("esplus_prepare prepare_async -> %d", ok);
  if (!ok) PLOG("prepare_async failed\n");
}

/* submit + start-once-prepared; safe to call from the video and audio threads. */
int esplus_submit(const esplusplayer_es_packet *pkt) {
  pthread_mutex_lock(&s_lock);
  if (s_prepared && s_prepare_ok == 1 && !s_started) {
    s_started = 1; int ok = esplusplayer_start(s_pl); (void)ok; PLOG("started\n"); MLT("esplus_start -> %d", ok);
  }
  int r = esplusplayer_submit_packet(s_pl, pkt);
  if (r == ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS && pkt->type == ESPLUSPLAYER_STREAM_TYPE_VIDEO)
    __atomic_fetch_add(&esplus_stats.submitted, 1, __ATOMIC_RELAXED);
  /* shortcut: sample per-frame trace (first 30, then every 300th, plus failures);
     raise the rate here if a hang needs every frame. Counter is under s_lock. */
  static unsigned n; n++;
  if (n <= 30 || n % 300 == 0 || r != ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS) MLT("esplus_submit #%u -> %d", n, r);
  pthread_mutex_unlock(&s_lock);
  return r;
}

void esplus_close(void) {
  MLT("esplus_close");
  pthread_mutex_lock(&s_lock);
  if (s_pl) {
    int st = esplusplayer_stop(s_pl), cl = esplusplayer_close(s_pl); (void)st; (void)cl;
    MLT("esplusplayer_stop -> %d, close -> %d", st, cl);
    s_pl = NULL;
  }
  s_prepared = s_prepare_ok = s_started = 0;
  pthread_mutex_unlock(&s_lock);
  /* Release only what esplus_set_window took: an unbalanced ecore_shutdown() would
     shut down the host app's own ecore loop. */
  if (s_win || s_disp || s_ecore) {
    ecore_main_loop_thread_safe_call_sync(win_destroy_main, NULL);
    ecore_shutdown();
  }
}
