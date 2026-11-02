/*
 * mlservice.c - native streaming service of the hybrid Moonlight Tizen 5.0 package.
 *
 * A Tizen service-application that registers a message port "moonlight" and
 * implements the same function surface the moonlight-tizen web UI expects from
 * the (WASM) native module, dispatched over the message port instead of embind:
 *   makeCert, httpInit, openUrl, pair, startStream, stopStream, cancelRequest,
 *   sendKeyboardEvent, stun, wakeOnLan, toggleStats
 * Each maps to our native engine (gs_client http/pair, esplusplayer video/audio,
 * input). The UI sends a bundle {cmd, id, arg0..argN}; we reply with a bundle
 * {id, result|error} to the UI's reply port.
 *
 * Status: compiles+links against the TV's appfw/message-port/esplusplayer libs.
 * On-device behaviour (message port round-trips, two-track prepare) is the next
 * validation step - needs the hybrid package installed. See pkg/ARCHITECTURE.md.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <service_app.h>
#include <message_port.h>
#include <bundle.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <curl/curl.h>

#include "Limelight.h"
#include "mkcert.h"
#include "http.h"

/* ---- client cert state referenced by libgamestream (http.c/pairing.c) ---- */
X509 *g_Cert;
EVP_PKEY *g_PrivateKey;
char *g_UniqueId;
char *g_CertHex;

/* ---- engine entry points (our other .c files) ---- */
extern void VidEsplus_FillCallbacks(PDECODER_RENDERER_CALLBACKS);
extern void AudEsplus_FillCallbacks(PAUDIO_RENDERER_CALLBACKS);
extern void esplus_close(void);
extern int gs_pair(int, const char *, unsigned short, const char *, char **);
extern void http_cancel_request(void);

#define SLOG(...) fprintf(stderr, "[mlservice] " __VA_ARGS__)
static int s_local_port = -1;

/* ---- reply helper ---- */
static void reply(const char *remote_app, const char *remote_port, const char *id,
                  const char *key, const char *val) {
  if (!remote_app || !remote_port) return;
  bundle *b = bundle_create();
  if (id) bundle_add_str(b, "id", id);
  bundle_add_str(b, key, val ? val : "");
  message_port_send_message_with_local_port(remote_app, remote_port, b, s_local_port);
  bundle_free(b);
}

/* ---- cert: makeCert / httpInit (native equivalents of http.cpp) ---- */
static int make_cert(char **certOut, char **keyOut) {
  CERT_KEY_PAIR ck = mkcert_generate();
  BIO *bc = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bc, ck.x509);
  BUF_MEM *m = NULL; BIO_get_mem_ptr(bc, &m);
  *certOut = strndup(m->data, m->length); BIO_free(bc);
  BIO *bk = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bk, ck.pkey, NULL, NULL, 0, NULL, NULL);
  BIO_get_mem_ptr(bk, &m); *keyOut = strndup(m->data, m->length); BIO_free(bk);
  mkcert_free(ck);
  return 0;
}

static int http_init(const char *certStr, const char *keyStr, const char *uniqueId) {
  BIO *bio = BIO_new_mem_buf(certStr, -1);
  g_Cert = PEM_read_bio_X509(bio, NULL, NULL, NULL); BIO_free_all(bio);
  bio = BIO_new_mem_buf(keyStr, -1);
  g_PrivateKey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL); BIO_free_all(bio);
  if (!g_Cert || !g_PrivateKey) return -1;
  size_t n = strlen(certStr);
  g_CertHex = malloc(n * 2 + 1);
  for (size_t i = 0; i < n; i++) sprintf(&g_CertHex[i * 2], "%02x", (unsigned char)certStr[i]);
  g_UniqueId = strdup(uniqueId);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  return 0;
}

static void hex_to_bytes(const char *hex, unsigned char *out, int n) {
  for (int i = 0; i < n; i++) sscanf(hex + i * 2, "%2hhx", &out[i]);
}

/* ---- streaming ---- */
static STREAM_CONFIGURATION s_sc;
static SERVER_INFORMATION s_si;
static char s_host[128], s_rtsp[512], s_appver[64], s_gfever[64];
static pthread_t s_stream_th;
static volatile int s_streaming;
static char s_ui_app[128], s_ui_port[64];   /* for async callbacks back to UI */

static void cl_stageFailed(int stage, int err) { SLOG("stage %d failed %d\n", stage, err); }
static void cl_connTerminated(int err) {
  SLOG("connection terminated %d\n", err);
  s_streaming = 0;
  reply(s_ui_app, s_ui_port, NULL, "event", "streamTerminated");
}
static void cl_logMessage(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }

static void *stream_thread(void *arg) {
  (void)arg;
  DECODER_RENDERER_CALLBACKS dr; VidEsplus_FillCallbacks(&dr);
  AUDIO_RENDERER_CALLBACKS ar; AudEsplus_FillCallbacks(&ar);
  CONNECTION_LISTENER_CALLBACKS cl; LiInitializeConnectionCallbacks(&cl);
  cl.stageFailed = cl_stageFailed; cl.connectionTerminated = cl_connTerminated; cl.logMessage = cl_logMessage;
  int err = LiStartConnection(&s_si, &s_sc, &cl, &dr, &ar, NULL, 0, NULL, 0);
  if (err != 0) { SLOG("LiStartConnection failed %d\n", err); s_streaming = 0;
    reply(s_ui_app, s_ui_port, NULL, "event", "streamFailed"); }
  return NULL;
}

/* startStream args (subset of moonlight-tizen startRequest):
   host, httpPort, w, h, fps, bitrate, rikeyHex, rikeyid, appVer, gfeVer, rtspUrl, codecMode */
static int start_stream(bundle *b) {
  if (s_streaming) return -1;
  char *v;
  #define G(k) (bundle_get_str(b, k, &v) == BUNDLE_ERROR_NONE ? v : NULL)
  const char *host = G("host"); if (!host) return -1;
  strncpy(s_host, host, sizeof s_host - 1);
  LiInitializeStreamConfiguration(&s_sc);
  s_sc.width = G("w") ? atoi(v) : 1280;
  s_sc.height = G("h") ? atoi(v) : 720;
  s_sc.fps = G("fps") ? atoi(v) : 60;
  s_sc.bitrate = G("bitrate") ? atoi(v) : 20000;
  s_sc.packetSize = 1024;
  s_sc.supportedVideoFormats = VIDEO_FORMAT_H264;
  s_sc.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
  const char *rikey = G("rikey"); if (rikey && strlen(rikey) >= 32) hex_to_bytes(rikey, (unsigned char *)s_sc.remoteInputAesKey, 16);
  memset(s_sc.remoteInputAesIv, 0, sizeof s_sc.remoteInputAesIv);
  int rikeyid = G("rikeyid") ? atoi(v) : 0;
  memcpy(s_sc.remoteInputAesIv, &rikeyid, sizeof rikeyid);
  if (G("appVer")) strncpy(s_appver, v, sizeof s_appver - 1);
  if (G("gfeVer")) strncpy(s_gfever, v, sizeof s_gfever - 1);
  if (G("rtspUrl")) strncpy(s_rtsp, v, sizeof s_rtsp - 1);
  LiInitializeServerInformation(&s_si);
  s_si.address = s_host;
  s_si.serverInfoAppVersion = s_appver;
  s_si.serverInfoGfeVersion = s_gfever;
  s_si.rtspSessionUrl = s_rtsp;
  s_si.serverCodecModeSupport = G("codecMode") ? atoi(v) : SCM_H264;
  #undef G
  s_streaming = 1;
  pthread_create(&s_stream_th, NULL, stream_thread, NULL);
  return 0;
}

/* ---- message dispatch ---- */
static void on_message(int local_port_id, const char *remote_app, const char *remote_port,
                       bool trusted, bundle *msg, void *user_data) {
  (void)local_port_id; (void)trusted; (void)user_data;
  char *cmd = NULL, *id = NULL, *a0 = NULL, *a1 = NULL, *a2 = NULL;
  bundle_get_str(msg, "cmd", &cmd);
  bundle_get_str(msg, "id", &id);
  if (!cmd) return;
  if (remote_app) strncpy(s_ui_app, remote_app, sizeof s_ui_app - 1);
  if (remote_port) strncpy(s_ui_port, remote_port, sizeof s_ui_port - 1);
  SLOG("cmd=%s\n", cmd);

  if (!strcmp(cmd, "makeCert")) {
    char *c = NULL, *k = NULL; make_cert(&c, &k);
    bundle *r = bundle_create();
    if (id) bundle_add_str(r, "id", id);
    bundle_add_str(r, "cert", c ? c : ""); bundle_add_str(r, "key", k ? k : "");
    message_port_send_message_with_local_port(remote_app, remote_port, r, s_local_port);
    bundle_free(r); free(c); free(k);
  } else if (!strcmp(cmd, "httpInit")) {
    bundle_get_str(msg, "cert", &a0); bundle_get_str(msg, "key", &a1); bundle_get_str(msg, "uniqueId", &a2);
    reply(remote_app, remote_port, id, "result", http_init(a0, a1, a2) == 0 ? "ok" : "error");
  } else if (!strcmp(cmd, "openUrl")) {
    bundle_get_str(msg, "url", &a0); bundle_get_str(msg, "ppk", &a1);
    PHTTP_DATA d = http_create_data();
    int rc = http_request(a0, a1, d);
    bundle *r = bundle_create();
    if (id) bundle_add_str(r, "id", id);
    if (rc == 0 && d->memory) bundle_add_byte(r, "data", d->memory, d->size + 1);
    else bundle_add_str(r, "error", "http_request failed");
    message_port_send_message_with_local_port(remote_app, remote_port, r, s_local_port);
    bundle_free(r); http_free_data(d);
  } else if (!strcmp(cmd, "pair")) {
    bundle_get_str(msg, "addr", &a0); bundle_get_str(msg, "pin", &a1);
    char *major = NULL, *port = NULL; bundle_get_str(msg, "major", &major); bundle_get_str(msg, "port", &port);
    char *scert = NULL;
    int rc = gs_pair(major ? atoi(major) : 7, a0, port ? atoi(port) : 47989, a1, &scert);
    bundle *r = bundle_create();
    if (id) bundle_add_str(r, "id", id);
    bundle_add_str(r, rc == 0 ? "serverCert" : "error", rc == 0 && scert ? scert : "pair failed");
    message_port_send_message_with_local_port(remote_app, remote_port, r, s_local_port);
    bundle_free(r); free(scert);
  } else if (!strcmp(cmd, "startStream")) {
    reply(remote_app, remote_port, id, "result", start_stream(msg) == 0 ? "ok" : "error");
  } else if (!strcmp(cmd, "stopStream")) {
    if (s_streaming) { LiStopConnection(); s_streaming = 0; }
    reply(remote_app, remote_port, id, "result", "ok");
  } else if (!strcmp(cmd, "cancelRequest")) {
    http_cancel_request();
    reply(remote_app, remote_port, id, "result", "ok");
  } else if (!strcmp(cmd, "sendKeyboardEvent")) {
    char *kc = NULL, *ac = NULL, *md = NULL;
    bundle_get_str(msg, "key", &kc); bundle_get_str(msg, "action", &ac); bundle_get_str(msg, "mods", &md);
    if (kc && ac) LiSendKeyboardEvent((short)atoi(kc), (char)atoi(ac), md ? (char)atoi(md) : 0);
  } else {
    SLOG("unhandled cmd %s\n", cmd);   /* stun/wakeOnLan/toggleStats: TODO */
  }
}

/* ---- service lifecycle ---- */
static bool app_create(void *data) {
  (void)data;
  s_local_port = message_port_register_local_port("moonlight", on_message, NULL);
  SLOG("local port 'moonlight' = %d\n", s_local_port);
  return true;
}
static void app_terminate(void *data) {
  (void)data;
  if (s_streaming) { LiStopConnection(); s_streaming = 0; }
  esplus_close();
}
static void app_control(app_control_h ac, void *data) { (void)ac; (void)data; }

int main(int argc, char *argv[]) {
  service_app_lifecycle_callback_s cb = {0};
  cb.create = app_create;
  cb.terminate = app_terminate;
  cb.app_control = app_control;
  return service_app_main(argc, argv, &cb, NULL);
}
