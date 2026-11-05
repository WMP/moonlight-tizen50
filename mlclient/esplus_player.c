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
#include <time.h>

#include "esplus_stats.h"
#include "esplusplayer_tv50.h"
#include "mldiag.h"
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
  return *end || n < 0 ? def : (int)n;   /* every knob is a non-negative amount */
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
  MLT("cfg latency=%d target=%d nudge=%d drop_after=%d poll=%d stall=%d recover_max=%d back=%d",
      esplus_cfg.latency_mode, esplus_cfg.target_depth_ms, esplus_cfg.nudge_max_ms_per_s, esplus_cfg.drop_after_ms,
      esplus_cfg.depth_poll_ms, esplus_cfg.stall_ms, esplus_cfg.recover_max, esplus_cfg.recover_back_ms);
}

static uint64_t s_last_video_pts_ms, s_last_audio_pts_ms;   /* under s_lock */
static uint64_t s_fps_t0;     /* start of the current fps second; 0 = not started */
static uint32_t s_fps_sub0;

/* set by recover(): the video sink returns DR_NEED_IDR for its next non-IDR frame, so
   moonlight-common-c drops the frames that reference the flushed ones and asks for an IDR */
static volatile int s_need_idr;
int esplus_take_need_idr(void) { return __atomic_exchange_n(&s_need_idr, 0, __ATOMIC_RELAXED); }

static int64_t s_corr_us;            /* nudge correction, microseconds, under s_lock */
static volatile int32_t s_corr_ms;   /* rounded, read lock-free by the video sink */
static uint64_t s_tick_prev_ms;      /* previous successful tick; 0 = none */
static volatile uint32_t s_tick_seq;

int32_t esplus_correction_ms(void) { return s_corr_ms; }
uint32_t esplus_sample_seq(void) { return s_tick_seq; }


static void nudge(int32_t depth, uint64_t now_ms) {
  if (esplus_cfg.latency_mode != ESPLUS_LAT_NUDGE) return;
  if (s_tick_prev_ms) {
    int64_t err = (int64_t)depth - esplus_cfg.target_depth_ms, hyst = esplus_cfg.target_depth_ms / 2;
    if (hyst < 10) hyst = 10;
    int64_t step = (int64_t)esplus_cfg.nudge_max_ms_per_s * (int64_t)(now_ms - s_tick_prev_ms);   /* us */
    if (err > hyst) s_corr_us += err * 1000 < step ? err * 1000 : step;
    else if (err < -hyst) s_corr_us -= -err * 1000 < step ? -err * 1000 : step;
    /* guard against a broken clock reading */
    if (s_corr_us > 1000000) s_corr_us = 1000000;
    if (s_corr_us < -1000000) s_corr_us = -1000000;
    s_corr_ms = (int32_t)((s_corr_us + (s_corr_us < 0 ? -500 : 500)) / 1000);
  }
}

static pthread_t s_smp_th;
static pthread_mutex_t s_smp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_smp_cv;
static int s_smp_run, s_smp_stop;

/* Watchdog state, under s_lock (reset per session in esplus_create_open). */
static volatile int s_error_pending;   /* set by on_error; the tick recovers, the callback must not */
static uint64_t s_stall_play, s_stall_t, s_last_recover_ms;   /* s_stall_t 0 = no sample yet */
static uint32_t s_stall_sub;
static int s_stall_armed, s_recover_count, s_have_recover, s_off_logged;

/* Flushing seek a little behind the last video PTS, then a fresh IDR (s_need_idr). Returns -1
 * when it is too soon after the last one (an error then stays pending), else 0. Does not wait
 * for seek_done; never ends the session. */
static int recover(const char *why, uint64_t now_ms) {
  if (esplus_cfg.recover_max == 0) {
    if (!s_off_logged) { s_off_logged = 1; MLT("recovery off (%s)", why); }
    return 0;
  }
  if (esplus_stats.recovery_state == 2 || s_conflicted) return 0;
  if (s_have_recover && now_ms - s_last_recover_ms < 2000) return -1;
  if (s_recover_count >= esplus_cfg.recover_max) {
    esplus_stats.recovery_state = 2; MLT("recovery gave up"); PLOG("recovery gave up\n");
    return 0;
  }
  esplus_stats.recovery_state = 1;
  __atomic_fetch_add(&esplus_stats.recoveries, 1, __ATOMIC_RELAXED);
  s_recover_count++; s_have_recover = 1; s_last_recover_ms = now_ms;
  uint64_t target = s_last_video_pts_ms > (uint64_t)esplus_cfg.recover_back_ms
                  ? s_last_video_pts_ms - (uint64_t)esplus_cfg.recover_back_ms : 0;
  int ok = esplusplayer_seek(s_pl, target);
  PLOG("recover (%s) seek %llu ms -> %d\n", why, (unsigned long long)target, ok);
  MLT("recover (%s) seek %llu ms -> %d", why, (unsigned long long)target, ok);
  s_corr_us = 0; s_corr_ms = 0;
  s_stall_t = now_ms; s_stall_sub = esplus_stats.submitted;
  s_need_idr = 1;
  int one = 1;   /* on_conflict may store 2 meanwhile, without s_lock: keep it */
  __atomic_compare_exchange_n(&esplus_stats.recovery_state, &one, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  return 0;
}

/* One sampler step; the thread calls it with the monotonic clock, the host test with a fake one. */
void esplus_sample_tick(uint64_t now_ms) {
  pthread_mutex_lock(&s_lock);
  if (s_pl && s_started) {
    const char *why = NULL;
    uint64_t play;
    if (esplusplayer_get_playing_time(s_pl, &play)) {
      esplus_stats.playing_ms = (uint32_t)play;
      esplus_stats.depth_ms = (int32_t)(s_last_video_pts_ms - play);
      esplus_stats.audio_depth_ms = (int32_t)(s_last_audio_pts_ms - play);
      if (!s_fps_t0) { s_fps_t0 = now_ms; s_fps_sub0 = esplus_stats.submitted; }
      else if (now_ms - s_fps_t0 >= 1000) {
        esplus_stats.fps_submitted = esplus_stats.submitted - s_fps_sub0;
        s_fps_t0 = now_ms; s_fps_sub0 = esplus_stats.submitted;
      }
      if (play > 0) nudge(esplus_stats.depth_ms, now_ms);
      s_tick_prev_ms = now_ms;
      MLT("stats depth=%dms audio=%dms fps=%u submitted=%u", esplus_stats.depth_ms,
          esplus_stats.audio_depth_ms, esplus_stats.fps_submitted, esplus_stats.submitted);
      /* stall: playing time stopped while we keep submitting. Armed after the first non-zero time. */
      if (!s_stall_t || play != s_stall_play) {
        s_stall_play = play; s_stall_t = now_ms; s_stall_sub = esplus_stats.submitted;
        if (play > 0) s_stall_armed = 1;
      } else if (s_stall_armed && now_ms - s_stall_t > (uint64_t)esplus_cfg.stall_ms
                 && esplus_stats.submitted != s_stall_sub) why = "stall";
    }
    int err = __atomic_exchange_n(&s_error_pending, 0, __ATOMIC_RELAXED);
    if (err) why = "error";
    if (why && !s_conflicted && recover(why, now_ms) < 0 && err) s_error_pending = 1;   /* retry after the cooldown */
  }
  s_tick_seq++;
  pthread_mutex_unlock(&s_lock);
}

uint64_t esplus_mono_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void *sampler(void *arg) {
  (void)arg;
  pthread_mutex_lock(&s_smp_mu);
  while (!s_smp_stop) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += esplus_cfg.depth_poll_ms / 1000;
    t.tv_nsec += (long)(esplus_cfg.depth_poll_ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    if (pthread_cond_timedwait(&s_smp_cv, &s_smp_mu, &t) == 0 || s_smp_stop) continue;
    pthread_mutex_unlock(&s_smp_mu);
    esplus_sample_tick(esplus_mono_ms());
    pthread_mutex_lock(&s_smp_mu);
  }
  pthread_mutex_unlock(&s_smp_mu);
  return NULL;
}

static void sampler_start(void) {
  if (s_smp_run || esplus_cfg.depth_poll_ms < 50) return;
  pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  pthread_cond_init(&s_smp_cv, &a); pthread_condattr_destroy(&a);
  s_smp_stop = 0;
  s_smp_run = pthread_create(&s_smp_th, NULL, sampler, NULL) == 0;
  MLT("sampler start -> %d", s_smp_run);
}

static void sampler_stop(void) {   /* never call with s_lock held: the tick takes it */
  if (!s_smp_run) return;
  pthread_mutex_lock(&s_smp_mu); s_smp_stop = 1; pthread_cond_signal(&s_smp_cv); pthread_mutex_unlock(&s_smp_mu);
  pthread_join(s_smp_th, NULL);
  pthread_cond_destroy(&s_smp_cv);   /* sampler_start inits it again */
  s_smp_run = 0;
  MLT("sampler stopped");
}

static int s_ecore;   /* 1 while we hold an ecore_init()/ecore_wl2_init() reference */

static void on_prepared(int ok) { s_prepare_ok = ok; s_prepared = 1; mldiag_player("pdone", ok); PLOG("prepare_async_done=%d\n", ok); MLT("cb prepare_async_done ok=%d", ok); }
static void on_ready(uint32_t st) { (void)st; MLT("cb ready_to_prepare stream=%u", st); }
/* Player callbacks may run on player threads while we hold s_lock inside a player call:
 * they must never take s_lock or call the player; atomics and flags only. */
static void on_error(uint32_t e) {
  s_error_pending = 1;
  __atomic_fetch_add(&esplus_stats.errors, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&esplus_stats.last_error, e, __ATOMIC_RELAXED);
  PLOG("player error 0x%x\n", e); MLT("cb player error 0x%x", e);
}
static void on_buffer(uint32_t st, uint32_t s) {
  if (s == 0) __atomic_fetch_add(&esplus_stats.underruns, 1, __ATOMIC_RELAXED);
  else if (s == 1) __atomic_fetch_add(&esplus_stats.overruns, 1, __ATOMIC_RELAXED);
  if (s) PLOG("OVERRUN stream=%u\n", st);
}
static void on_seek_done(void) { MLT("cb seek_done"); }
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
    s_conflicted = 0; s_error_pending = 0; s_need_idr = 0;
    s_stall_play = s_stall_t = s_last_recover_ms = 0; s_stall_sub = 0;
    s_stall_armed = s_recover_count = s_have_recover = s_off_logged = 0;
    s_pl = esplusplayer_create();
    MLT("esplusplayer_create -> %p", (void *)s_pl);
    if (s_pl) {
      esplusplayer_set_error_cb(s_pl, on_error);
      esplusplayer_set_buffer_status_cb(s_pl, on_buffer);
      esplusplayer_set_resource_conflicted_cb(s_pl, on_conflict);
      esplusplayer_set_seek_done_cb(s_pl, on_seek_done);
      esplusplayer_set_ready_to_prepare_cb(s_pl, on_ready);
      esplusplayer_set_prepare_async_done_cb(s_pl, on_prepared);
      int ok = esplusplayer_open(s_pl);
      MLT("esplusplayer_open -> %d", ok);
      mldiag_player("open", ok);
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
  mldiag_player("prep", ok);
  if (!ok) PLOG("prepare_async failed\n");
  sampler_start();
}

/* submit + start-once-prepared; safe to call from the video and audio threads. */
int esplus_submit(const esplusplayer_es_packet *pkt) {
  pthread_mutex_lock(&s_lock);
  if (s_prepared && s_prepare_ok == 1 && !s_started) {
    s_started = 1; s_error_pending = 0;   /* an error before start is not a playback failure */
    int ok = esplusplayer_start(s_pl); (void)ok; PLOG("started\n"); MLT("esplus_start -> %d", ok); mldiag_player("start", ok);
  }
  int r = esplusplayer_submit_packet(s_pl, pkt);
  mldiag_submit(pkt->type == ESPLUSPLAYER_STREAM_TYPE_VIDEO, r == ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS, r);
  if (r == ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS) {
    uint64_t ms = pkt->pts / 1000000;
    if (pkt->type == ESPLUSPLAYER_STREAM_TYPE_VIDEO) {
      s_last_video_pts_ms = ms; __atomic_fetch_add(&esplus_stats.submitted, 1, __ATOMIC_RELAXED);
    } else s_last_audio_pts_ms = ms;
  }
  /* shortcut: sample per-frame trace (first 30, then every 300th, plus each change of the
     result); raise the rate here if a hang needs every frame. Logging every failure filled
     the console buffer and pushed the session start out of the log; failures are counted
     in the ML-ERROR line. Counters are under s_lock. */
  static unsigned n; static int last_r = ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS; n++;
  if (n <= 30 || n % 300 == 0 || r != last_r) MLT("esplus_submit #%u -> %d", n, r);
  last_r = r;
  pthread_mutex_unlock(&s_lock);
  return r;
}

void esplus_close(void) {
  MLT("esplus_close");
  sampler_stop();
  pthread_mutex_lock(&s_lock);
  if (s_pl) {
    int st = esplusplayer_stop(s_pl), cl = esplusplayer_close(s_pl); (void)st; (void)cl;
    MLT("esplusplayer_stop -> %d, close -> %d", st, cl);
    s_pl = NULL;
  }
  s_prepared = s_prepare_ok = s_started = 0;
  s_last_video_pts_ms = s_last_audio_pts_ms = 0; s_fps_t0 = 0;
  s_corr_us = 0; s_corr_ms = 0; s_tick_prev_ms = 0;
  pthread_mutex_unlock(&s_lock);
  /* Release only what esplus_set_window took: an unbalanced ecore_shutdown() would
     shut down the host app's own ecore loop. */
  if (s_win || s_disp || s_ecore) {
    ecore_main_loop_thread_safe_call_sync(win_destroy_main, NULL);
    ecore_shutdown();
  }
}
