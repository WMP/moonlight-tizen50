/*
 * engine_ui.c - per-step engine calls for the web UI (see mlengine.h, pkg/ui/PROTOCOL.md).
 * Native equivalent of the WASM fork's http.cpp (MakeCert/HttpInit/OpenUrl) and main.cpp
 * (Pair/CancelRequest). ml_engine_start_session lives in engine_api.c (shares start code).
 * Never log pin, cert, key or rikey values.
 */
#define _GNU_SOURCE
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Limelight.h"
#include "errors.h"
#include "http.h"
#include "mkcert.h"
#include "mlengine.h"
#include "mldiag.h"
#include "mltrace.h"
#include "pairing.h"

extern X509 *g_Cert;          /* cert_init.c */
extern EVP_PKEY *g_PrivateKey;
extern char *g_UniqueId;
extern char *g_CertHex;
extern void http_cancel_request(void);  /* libgamestream http.c */

static char *bio_to_str(BIO *b) {
  BUF_MEM *m = NULL; BIO_get_mem_ptr(b, &m);
  return m ? strndup(m->data, m->length) : NULL;
}

int ml_engine_make_cert(char **cert_pem, char **key_pem) {
  MLT("ml_engine_make_cert");
  if (!cert_pem || !key_pem) return GS_INVALID;
  *cert_pem = *key_pem = NULL;
  CERT_KEY_PAIR ck = mkcert_generate();
  BIO *bc = BIO_new(BIO_s_mem()), *bk = BIO_new(BIO_s_mem());
  if (bc && bk && PEM_write_bio_X509(bc, ck.x509) &&
      PEM_write_bio_PrivateKey(bk, ck.pkey, NULL, NULL, 0, NULL, NULL)) {
    *cert_pem = bio_to_str(bc); *key_pem = bio_to_str(bk);
  }
  BIO_free(bc); BIO_free(bk);
  mkcert_free(ck);
  int rc = (*cert_pem && *key_pem) ? 0 : GS_FAILED;
  if (rc) { free(*cert_pem); free(*key_pem); *cert_pem = *key_pem = NULL; }
  MLT("ml_engine_make_cert -> %d", rc);
  return rc;
}

int ml_engine_http_init(const char *cert_pem, const char *key_pem, const char *unique_id) {
  static int curl_ready;
  MLT("ml_engine_http_init cert=%s key=%s id=%s", cert_pem ? "(set)" : "(null)",
      key_pem ? "(set)" : "(null)", unique_id ? "(set)" : "(null)");
  if (!cert_pem || !key_pem || !unique_id) return GS_INVALID;
  BIO *bio = BIO_new_mem_buf((void *)cert_pem, -1);
  X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free_all(bio);
  if (!cert) { MLT("ml_engine_http_init -> 1 (cert)"); return 1; }
  bio = BIO_new_mem_buf((void *)key_pem, -1);
  EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  BIO_free_all(bio);
  if (!key) { X509_free(cert); MLT("ml_engine_http_init -> 2 (key)"); return 2; }

  size_t clen = strlen(cert_pem);
  char *hex = malloc(clen * 2 + 1);
  if (!hex) { X509_free(cert); EVP_PKEY_free(key); return GS_OUT_OF_MEMORY; }
  for (size_t i = 0; i < clen; i++) sprintf(&hex[i * 2], "%02x", (unsigned char)cert_pem[i]);

  if (g_Cert) X509_free(g_Cert);
  if (g_PrivateKey) EVP_PKEY_free(g_PrivateKey);
  free(g_CertHex); free(g_UniqueId);
  g_Cert = cert; g_PrivateKey = key; g_CertHex = hex; g_UniqueId = strdup(unique_id);

  if (!curl_ready) { curl_global_init(CURL_GLOBAL_DEFAULT); curl_ready = 1; }
  MLT("ml_engine_http_init -> 0");
  return 0;
}

int ml_engine_open_url(const char *url, const char *ppk, unsigned char **out, int *out_len) {
  MLT("ml_engine_open_url url=%s ppk=%s", url ? url : "(null)", ppk && *ppk ? "(set)" : "(none)");
  if (!url || !out || !out_len) return GS_INVALID;
  *out = NULL; *out_len = 0;
  char *full = NULL;
  const char *u = url;
  if (strstr(url, "/launch?") || strstr(url, "/resume?")) {
    mldiag_note_launch(url);
    const char *q = LiGetLaunchUrlQueryParameters();
    size_t n = strlen(url) + strlen(q) + 1;
    full = malloc(n);
    if (!full) return GS_OUT_OF_MEMORY;
    snprintf(full, n, "%s%s", url, q);
    u = full;
  }
  PHTTP_DATA data = http_create_data();
  if (!data) { free(full); return GS_OUT_OF_MEMORY; }
  int rc = http_request(u, ppk && *ppk ? ppk : NULL, data);
  if (rc == 0) {
    *out = malloc(data->size + 1);
    if (!*out) rc = GS_OUT_OF_MEMORY;
    else {
      if (data->size) memcpy(*out, data->memory, data->size);
      (*out)[data->size] = 0;
      *out_len = (int)data->size;
    }
  }
  http_free_data(data);
  free(full);
  MLT("ml_engine_open_url -> %d len=%d", rc, *out_len);
  return rc;
}

int ml_engine_pair(int server_major, const char *address, int http_port, const char *pin,
                   char **server_cert_pem) {
  MLT("ml_engine_pair major=%d addr=%s port=%d pin=%s", server_major, address ? address : "(null)",
      http_port, pin ? "(set)" : "(null)");
  if (!address || !pin || !server_cert_pem) return GS_INVALID;
  *server_cert_pem = NULL;
  int rc = gs_pair(server_major, address, (unsigned short)http_port, pin, server_cert_pem);
  MLT("ml_engine_pair -> %d", rc);
  return rc;
}

void ml_engine_cancel_request(void) { MLT("ml_engine_cancel_request"); http_cancel_request(); }

void ml_engine_send_key(int key_code, int key_action, int modifiers) {
  MLT("ml_engine_send_key code=%d action=%d mod=%d", key_code, key_action, modifiers);
  LiSendKeyboardEvent((short)key_code, (char)key_action, (char)modifiers);
}

void ml_engine_free(void *p) { free(p); }
