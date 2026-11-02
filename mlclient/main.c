/*
 * main.c - thin native CLI over libmlengine.so (root path / bring-up harness).
 *
 * All the real work lives in the engine .so (mlengine.h / engine_api.c); this just
 * parses argv, starts a session, waits, and stops cleanly on the host drop or a
 * signal. The no-root .NET host drives the same four ml_engine_* functions.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "mlengine.h"

static void on_sig(int s) { (void)s; ml_engine_stop(); }

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
      "usage: %s <host-ip> <appid> [width=1920] [height=1080] [fps=60] [bitrate_kbps=20000]\n"
      "  env: MOONLIGHT_PIN (4 digits, first-time pairing), MOONLIGHT_DATADIR (default /tmp)\n",
      argv[0]);
    return 2;
  }
  const char *host = argv[1];
  int appId  = atoi(argv[2]);
  int width  = argc > 3 ? atoi(argv[3]) : 0;
  int height = argc > 4 ? atoi(argv[4]) : 0;
  int fps    = argc > 5 ? atoi(argv[5]) : 0;
  int bitrate= argc > 6 ? atoi(argv[6]) : 0;
  const char *pin = getenv("MOONLIGHT_PIN");
  const char *datadir = getenv("MOONLIGHT_DATADIR");

  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  int rc = ml_engine_start(host, appId, pin, width, height, fps, bitrate, datadir);
  if (rc != 0) { fprintf(stderr, "[ml] start failed rc=%d\n", rc); return rc == 2 ? 2 : 1; }

  fprintf(stderr, "[ml] streaming; Ctrl+C to stop\n");
  ml_engine_wait();
  ml_engine_stop();
  return 0;
}
