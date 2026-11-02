/*
 * gs_client.c - GameStream HTTP orchestration for the native Moonlight client:
 * serverinfo -> (pair) -> launch/resume -> RTSP session URL. Ported "dry"
 * (not yet tested against a live host) from moonlight-embedded client.c, adapted
 * to the brightcraft/moonlight-tizen fork's trimmed libgamestream:
 *   - http_request(url, ppkstr, data)  (ppkstr = pinned server cert PEM, or NULL)
 *   - gs_pair(serverMajorVersion, address, httpPort, pin, &server_cert_pem)
 * Also provides a tiny dependency-free xml_search/xml_status (the fork's
 * libgamestream references these but ships no xml.c; moonlight-embedded used
 * expat - we avoid that dependency).
 *
 * NOTE: UNTESTED end-to-end. Needs a Sunshine/GFE host on the LAN. The GameStream
 * URLs, query parameters and field names mirror moonlight-embedded exactly.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/rand.h>

#include "Limelight.h"
#include "http.h"
#include "errors.h"

extern char *g_UniqueId;
extern int gs_pair(int serverMajorVersion, const char *address, unsigned short httpPort,
                   const char *pin, char **server_cert_pem);

/* ---- tiny XML helpers (plain <tag>value</tag>; attributes on tag tolerated) ---- */
int xml_search(char *data, size_t len, char *node, char **result) {
  char open[128], close[128];
  snprintf(open, sizeof open, "<%s", node);
  snprintf(close, sizeof close, "</%s>", node);
  /* bound the search to the given length */
  char *hay = data;
  char *end = data + len;
  for (char *p = hay; (p = memmem(p, (size_t)(end - p), open, strlen(open))); p++) {
    char *gt = memchr(p, '>', (size_t)(end - p));
    if (!gt) return GS_INVALID;
    /* make sure the tag name ended at '>' or whitespace (avoid <appversionfoo>) */
    char after = *(p + strlen(open));
    if (after != '>' && after != ' ' && after != '\t' && after != '/') continue;
    char *val = gt + 1;
    char *c = memmem(val, (size_t)(end - val), close, strlen(close));
    if (!c) return GS_INVALID;
    size_t n = (size_t)(c - val);
    char *out = malloc(n + 1);
    if (!out) return GS_OUT_OF_MEMORY;
    memcpy(out, val, n); out[n] = 0;
    *result = out;
    return GS_OK;
  }
  return GS_INVALID;
}

int xml_status(char *data, size_t len) {
  char *p = memmem(data, len, "status_code=\"", 13);
  if (!p) return GS_INVALID;
  int code = atoi(p + 13);
  return code == 200 ? GS_OK : GS_FAILED;
}

/* ---- helpers ---- */
static void bytes_to_hex(const unsigned char *in, char *out, size_t len) {
  for (size_t i = 0; i < len; i++) sprintf(out + i * 2, "%02x", in[i]);
  out[len * 2] = 0;
}

static void gen_uuid(char *out /* >=37 */) {
  unsigned char u[16];
  RAND_bytes(u, sizeof u);
  u[6] = (u[6] & 0x0f) | 0x40; u[8] = (u[8] & 0x3f) | 0x80;
  sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
          u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
}

static char *read_all(const char *path) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
  char *b = (n > 0) ? malloc(n + 1) : NULL;
  if (!b || fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
  b[n] = 0; fclose(f); return b;
}
static void write_all(const char *path, const char *s) {
  FILE *f = fopen(path, "wb"); if (!f) return;
  fwrite(s, 1, strlen(s), f); fclose(f);
}

/*
 * Fills si (serverInfoAppVersion, serverInfoGfeVersion, rtspSessionUrl,
 * serverCodecModeSupport left to caller) and sc (remoteInputAesKey/Iv).
 * pin: 4-digit string shown to the user to type on the host when pairing is
 * required; pass NULL to skip pairing (fails if the host is unpaired).
 * Returns GS_OK (0) on success.
 */
int gs_orchestrate(const char *host, unsigned short httpPort, unsigned short httpsPort,
                   int appId, const char *pin, STREAM_CONFIGURATION *sc, SERVER_INFORMATION *si,
                   const char *datadir) {
  PHTTP_DATA data = http_create_data();
  if (!data) return GS_OUT_OF_MEMORY;
  char url[4096], uuid[40], certpath[1024];
  char *result = NULL, *server_cert = NULL;
  int ret, paired = 0;
  snprintf(certpath, sizeof certpath, "%s/server.pem", datadir ? datadir : "/tmp");

  /* 1. If we have a stored server cert from a previous pairing, try HTTPS
     serverinfo pinned to it. Success = already paired (HTTP PairStatus is always
     0 without a client cert, so it can't be used to detect pairing). */
  server_cert = read_all(certpath);
  if (server_cert) {
    gen_uuid(uuid);
    snprintf(url, sizeof url, "https://%s:%u/serverinfo?uniqueid=%s&uuid=%s", host, httpsPort, g_UniqueId, uuid);
    if (http_request(url, server_cert, data) == GS_OK &&
        xml_search(data->memory, data->size, "appversion", &result) == GS_OK) {
      paired = 1;
      si->serverInfoAppVersion = result; result = NULL;
      if (xml_search(data->memory, data->size, "GfeVersion", &result) == GS_OK) { si->serverInfoGfeVersion = result; result = NULL; }
      fprintf(stderr, "[gs] already paired (stored server cert)\n");
    } else {
      free(server_cert); server_cert = NULL;   /* stale/unpaired - fall through */
    }
  }

  /* 2. Not paired: HTTP serverinfo for the major version, then pair. */
  if (!paired) {
    data->size = 0; if (data->memory) data->memory[0] = 0;
    gen_uuid(uuid);
    snprintf(url, sizeof url, "http://%s:%u/serverinfo?uniqueid=%s&uuid=%s", host, httpPort, g_UniqueId, uuid);
    if ((ret = http_request(url, NULL, data)) != GS_OK) { fprintf(stderr, "[gs] serverinfo(http) failed %d\n", ret); goto fail; }
    int serverMajorVersion = 7;
    if (xml_search(data->memory, data->size, "appversion", &result) == GS_OK) { serverMajorVersion = atoi(result); free(result); result = NULL; }

    if (!pin) { fprintf(stderr, "[gs] host not paired and no PIN given\n"); ret = GS_FAILED; goto fail; }
    fprintf(stderr, "[gs] pairing - enter PIN %s on the host\n", pin);
    if ((ret = gs_pair(serverMajorVersion, host, httpPort, pin, &server_cert)) != GS_OK) {
      fprintf(stderr, "[gs] pairing failed %d\n", ret); goto fail;
    }
    fprintf(stderr, "[gs] paired\n");
    if (server_cert) write_all(certpath, server_cert);   /* persist for next run */

    /* serverinfo over HTTPS (pinned) -> app/GFE version. */
    data->size = 0; if (data->memory) data->memory[0] = 0;
    gen_uuid(uuid);
    snprintf(url, sizeof url, "https://%s:%u/serverinfo?uniqueid=%s&uuid=%s", host, httpsPort, g_UniqueId, uuid);
    if ((ret = http_request(url, server_cert, data)) != GS_OK) { fprintf(stderr, "[gs] serverinfo(https) failed %d\n", ret); goto fail; }
    if (xml_search(data->memory, data->size, "appversion", &result) == GS_OK) { si->serverInfoAppVersion = result; result = NULL; }
    if (xml_search(data->memory, data->size, "GfeVersion", &result) == GS_OK) { si->serverInfoGfeVersion = result; result = NULL; }
  }

  /* 4. remote input key (AES) + rikeyid, same derivation as moonlight-embedded. */
  RAND_bytes((unsigned char *)sc->remoteInputAesKey, sizeof sc->remoteInputAesKey);
  memset(sc->remoteInputAesIv, 0, sizeof sc->remoteInputAesIv);
  uint32_t rikeyid = 0;
  RAND_bytes((unsigned char *)sc->remoteInputAesIv, sizeof rikeyid);
  memcpy(&rikeyid, sc->remoteInputAesIv, sizeof rikeyid);
  rikeyid = htonl(rikeyid);
  char rikey_hex[sizeof sc->remoteInputAesKey * 2 + 1];
  bytes_to_hex((unsigned char *)sc->remoteInputAesKey, rikey_hex, sizeof sc->remoteInputAesKey);

  /* 5. launch (or resume if a game is already running). */
  int sops = 1;
  int surround = SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(sc->audioConfiguration);
  int current = 0;
  if (xml_search(data->memory, data->size, "currentgame", &result) == GS_OK) { current = atoi(result); free(result); result = NULL; }
  const char *verb = (current != 0) ? "resume" : "launch";
  data->size = 0; if (data->memory) data->memory[0] = 0;
  gen_uuid(uuid);
  snprintf(url, sizeof url,
    "https://%s:%u/%s?uniqueid=%s&uuid=%s&appid=%d&mode=%dx%dx%d&additionalStates=1&sops=%d"
    "&rikey=%s&rikeyid=%d&localAudioPlayMode=0&surroundAudioInfo=%d&remoteControllersBitmap=0&gcmap=0%s",
    host, httpsPort, verb, g_UniqueId, uuid, appId, sc->width, sc->height, sc->fps, sops,
    rikey_hex, (int)rikeyid, surround, LiGetLaunchUrlQueryParameters());
  if ((ret = http_request(url, server_cert, data)) != GS_OK) { fprintf(stderr, "[gs] %s failed %d\n", verb, ret); goto fail; }
  if ((ret = xml_status(data->memory, data->size)) != GS_OK) { fprintf(stderr, "[gs] %s status not 200\n", verb); goto fail; }
  if (xml_search(data->memory, data->size, "gamesession", &result) != GS_OK &&
      xml_search(data->memory, data->size, "resume", &result) != GS_OK) {
    fprintf(stderr, "[gs] no gamesession in %s response\n", verb); ret = GS_FAILED; goto fail;
  }
  if (!strcmp(result, "0")) { fprintf(stderr, "[gs] %s returned session 0\n", verb); free(result); ret = GS_FAILED; goto fail; }
  free(result); result = NULL;
  if (xml_search(data->memory, data->size, "sessionUrl0", &result) == GS_OK) { si->rtspSessionUrl = result; result = NULL; }

  fprintf(stderr, "[gs] %s ok, rtsp=%s app=%s\n", verb, si->rtspSessionUrl ? si->rtspSessionUrl : "(none)",
          si->serverInfoAppVersion ? si->serverInfoAppVersion : "?");
  http_free_data(data);
  free(server_cert);
  return GS_OK;

fail:
  free(result); free(server_cert);
  http_free_data(data);
  return ret;
}
