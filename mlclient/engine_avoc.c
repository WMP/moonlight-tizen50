/*
 * engine_avoc.c - Samsung TV "Game Mode" picture/latency mode for a stream.
 *
 * The TV's avoc daemon (org.tizen.tv.avoc on the system bus) owns the game
 * picture mode. Its client library exposes a plain C call:
 *   int avoc_set_game_mode(int save_type, int mode)
 * save_type 1 keeps the setting per source (restored when off), mode 1 = on,
 * 0 = off. We dlopen the library at first use so the engine still loads on
 * TVs without it; a failed call is logged and ignored - it must never break
 * streaming. Not part of the public ABI (used by engine_api.c internally).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

#include "mltrace.h"

typedef int (*avoc_set_game_mode_fn)(int save_type, int mode);

static void *g_avoc;                  /* dlopen handle, cached for the process */
static avoc_set_game_mode_fn g_set;   /* resolved symbol, NULL until resolved */
static int g_on = -1;                 /* last mode we asked for (-1 = never) */
static int g_tried;                   /* dlopen attempted */

/* Resolves avoc_set_game_mode once; NULL when the library or symbol is missing. */
static avoc_set_game_mode_fn avoc_fn(void) {
  if (!g_tried) {
    g_tried = 1;
    g_avoc = dlopen("libavoc.so", 2 /* RTLD_NOW */);
    if (g_avoc) g_set = (avoc_set_game_mode_fn)dlsym(g_avoc, "avoc_set_game_mode");
    MLT("avoc: dlopen %s, set_game_mode %s",
        g_avoc ? "ok" : "failed", g_set ? "resolved" : "missing");
    if (!g_set) {
      if (g_avoc) { dlclose(g_avoc); g_avoc = 0; }
      fprintf(stderr, "[ml] avoc game mode unavailable (no avoc_set_game_mode)\n");
    }
  }
  return g_set;
}

/* Returns 0 when the call went through, non-zero otherwise. Thread-safe enough:
   called from the session start/stop paths only. */
int ml_avoc_set_game_mode(int on) {
  avoc_set_game_mode_fn set = avoc_fn();
  if (!set) return -1;
  if (g_on == on) { MLT("avoc: game mode already %d", on); return 0; }
  int rc = set(1, on ? 1 : 0);
  MLT("avoc: set_game_mode(1,%d) -> %d", on ? 1 : 0, rc);
  fprintf(stderr, "[ml] avoc game mode %d -> %d\n", on, rc);
  g_on = rc == 0 ? on : -1;   /* on failure retry the next call */
  return rc;
}

/* Immediate call for recovery from the UI console (mlengine.h): no g_on shortcut. */
int ml_engine_tv_game_mode(int on, int save_type) {
  avoc_set_game_mode_fn set = avoc_fn();
  if (!set) return -1;
  int rc = set(save_type, on ? 1 : 0);
  MLT("avoc: tv_game_mode(%d,%d) -> %d", save_type, on ? 1 : 0, rc);
  fprintf(stderr, "[ml] avoc tv game mode (%d,%d) -> %d\n", save_type, on ? 1 : 0, rc);
  if (save_type == 1 && rc == 0) g_on = on ? 1 : 0;   /* keep the session logic consistent */
  return rc;
}
