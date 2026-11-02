/*
 * engine_api.c - implements the libmlengine.so C ABI (mlengine.h).
 *
 * This is the former body of main.c, turned into a reusable, non-blocking engine
 * so the .NET host and the CLI harness can both drive it.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "Limelight.h"
#include "mlengine.h"
#include "mltrace.h"

extern int  ml_cert_init(const char *dir);
extern void VidEsplus_FillCallbacks(PDECODER_RENDERER_CALLBACKS cb);
extern void AudEsplus_FillCallbacks(PAUDIO_RENDERER_CALLBACKS cb);  /* audio_opus.c */
extern int  input_init(void);                                      /* input_evdev.c */
extern void input_shutdown(void);
extern int  gs_orchestrate(const char *host, unsigned short httpPort, unsigned short httpsPort,
                           int appId, const char *pin, STREAM_CONFIGURATION *sc,
                           SERVER_INFORMATION *si, const char *datadir);

static volatile int g_running;   /* 1 while a session is up */
static volatile int g_started;   /* 1 once LiStartConnection succeeded (needs LiStop) */
static char g_host[256];         /* si.address is held by moonlight-common-c for the
                                    session, so the host string must outlive the call */

static void cl_stageFailed(int stage, int errorCode) {
  fprintf(stderr, "[ml] stage %d FAILED err=%d\n", stage, errorCode);
  MLT("stageFailed stage=%d err=%d", stage, errorCode);
}
/* Host dropped the stream -> clear running so ml_engine_wait() returns and the
   caller runs ml_engine_stop(), which releases the HW overlay. Without this the
   overlay stays grabbed and blocks the TV's own broadcast video. */
static void cl_connTerminated(int errorCode) {
  fprintf(stderr, "[ml] connection terminated err=%d\n", errorCode); g_running = 0;
  MLT("connectionTerminated err=%d", errorCode);
}
static void cl_logMessage(const char *format, ...) {
  va_list a; va_start(a, format); vfprintf(stderr, format, a); va_end(a);
#ifdef MLT_TRACE   /* moonlight-common-c RTSP/control logs */
  { char b[512]; va_start(a, format); vsnprintf(b, sizeof b, format, a); va_end(a); MLT("cc: %s", b); }
#endif
}

int ml_engine_start(const char *host, int appId, const char *pin,
                    int width, int height, int fps, int bitrate_kbps,
                    const char *datadir) {
  MLT("ml_engine_start host=%s appId=%d pin=%s %dx%d@%d %dkbps datadir=%s",
      host ? host : "(null)", appId, pin ? "(set)" : "(null)", width, height, fps,
      bitrate_kbps, datadir ? datadir : "(null)");
  if (g_running) { MLT("ml_engine_start -> 0 (already streaming)"); return 0; }
  if (!host) { MLT("ml_engine_start -> 2 (no host)"); return 2; }
  if (!datadir) datadir = "/tmp";
  if (!width)  width = 1920;
  if (!height) height = 1080;
  if (!fps)    fps = 60;
  if (!bitrate_kbps) bitrate_kbps = 20000;
  snprintf(g_host, sizeof g_host, "%s", host);
  const unsigned short httpPort = 47989, httpsPort = 47984;

  int certrc = ml_cert_init(datadir);
  MLT("cert init rc=%d", certrc);
  if (certrc != 0) { fprintf(stderr, "[ml] cert init failed\n"); MLT("ml_engine_start -> 1 (cert)"); return 1; }

  static STREAM_CONFIGURATION sc;        /* static: held for the session */
  static SERVER_INFORMATION si;
  LiInitializeStreamConfiguration(&sc);
  sc.width = width; sc.height = height; sc.fps = fps; sc.bitrate = bitrate_kbps;
  sc.packetSize = 1024;
  sc.supportedVideoFormats = VIDEO_FORMAT_H264;   /* HEVC later */
  sc.audioConfiguration = AUDIO_CONFIGURATION_STEREO;

  LiInitializeServerInformation(&si);
  si.address = g_host;

  if (gs_orchestrate(g_host, httpPort, httpsPort, appId, pin, &sc, &si, datadir) != 0) {
    fprintf(stderr, "[ml] GameStream orchestration failed; cannot start stream.\n");
    MLT("ml_engine_start -> 3 (orchestrate)");
    return 3;
  }
  si.serverCodecModeSupport = SCM_H264;

  DECODER_RENDERER_CALLBACKS dr; VidEsplus_FillCallbacks(&dr);
  AUDIO_RENDERER_CALLBACKS   ar; AudEsplus_FillCallbacks(&ar);
  CONNECTION_LISTENER_CALLBACKS cl;
  LiInitializeConnectionCallbacks(&cl);
  cl.stageFailed = cl_stageFailed;
  cl.connectionTerminated = cl_connTerminated;
  cl.logMessage = cl_logMessage;

  fprintf(stderr, "[ml] LiStartConnection %dx%d@%d %dkbps -> %s\n",
          width, height, fps, bitrate_kbps, g_host);
  int err = LiStartConnection(&si, &sc, &cl, &dr, &ar, NULL, 0, NULL, 0);
  if (err != 0) { fprintf(stderr, "[ml] LiStartConnection failed: %d\n", err); MLT("ml_engine_start -> 1 (LiStartConnection %d)", err); return 1; }
  g_started = 1; g_running = 1;

  if (input_init() != 0) fprintf(stderr, "[ml] input disabled (no evdev access)\n");
  fprintf(stderr, "[ml] streaming\n");
  MLT("ml_engine_start -> 0");
  return 0;
}

int ml_engine_running(void) { MLT("ml_engine_running -> %d", g_running); return g_running; }

void ml_engine_wait(void) {
  MLT("ml_engine_wait");
  while (g_running) { struct timespec ts = {0, 200000000}; nanosleep(&ts, NULL); }
  MLT("ml_engine_wait -> returned");
}

void ml_engine_stop(void) {
  MLT("ml_engine_stop started=%d", g_started);
  g_running = 0;
  if (!g_started) { MLT("ml_engine_stop -> nothing to stop"); return; }
  g_started = 0;
  fprintf(stderr, "[ml] stopping, releasing overlay\n");
  input_shutdown();
  LiStopConnection();   /* tears down stages -> video sink cleanup -> esplus_close */
  MLT("ml_engine_stop -> done");
}
