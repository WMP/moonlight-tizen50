/*
 * cert_init.c - client certificate state for the native Moonlight client.
 * Defines the globals libgamestream (http.c/pairing.c) references as externs and
 * provides ml_cert_init(): generate the client cert on first run, persist it as
 * PEM, load it on later runs, build the hex form, pick a unique id, curl init.
 * Native equivalent of the WASM fork's http.cpp MakeCert/LoadCert/HttpInit.
 */
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mkcert.h"

X509 *g_Cert;
EVP_PKEY *g_PrivateKey;
char *g_UniqueId;
char *g_CertHex;

static char *read_file(const char *path, long *len_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
  char *b = malloc(n + 1);
  if (!b || n < 0 || fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
  b[n] = 0; fclose(f);
  if (len_out) *len_out = n;
  return b;
}

static int write_file(const char *path, const char *data, size_t len) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  int ok = fwrite(data, 1, len, f) == len;
  fclose(f);
  return ok ? 0 : -1;
}

/* dir must be writable (e.g. /tmp on the TV). Returns 0 on success. */
int ml_cert_init(const char *dir) {
  char certpath[1024], keypath[1024], idpath[1024];
  snprintf(certpath, sizeof certpath, "%s/client.pem", dir);
  snprintf(keypath, sizeof keypath, "%s/client.key", dir);
  snprintf(idpath, sizeof idpath, "%s/uniqueid", dir);

  char *certStr = read_file(certpath, NULL);
  char *keyStr = read_file(keypath, NULL);

  if (!certStr || !keyStr) {
    /* First run: generate and persist as PEM. */
    free(certStr); free(keyStr);
    CERT_KEY_PAIR ck = mkcert_generate();
    BIO *bc = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bc, ck.x509);
    BUF_MEM *m = NULL; BIO_get_mem_ptr(bc, &m);
    certStr = strndup(m->data, m->length);
    BIO_free(bc);
    BIO *bk = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bk, ck.pkey, NULL, NULL, 0, NULL, NULL);
    BIO_get_mem_ptr(bk, &m);
    keyStr = strndup(m->data, m->length);
    BIO_free(bk);
    mkcert_free(ck);
    if (write_file(certpath, certStr, strlen(certStr)) || write_file(keypath, keyStr, strlen(keyStr))) {
      fprintf(stderr, "[cert] cannot write cert to %s\n", dir);
      /* keep going with in-memory cert */
    }
  }

  BIO *bio = BIO_new_mem_buf(certStr, -1);
  g_Cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free_all(bio);
  bio = BIO_new_mem_buf(keyStr, -1);
  g_PrivateKey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
  BIO_free_all(bio);
  if (!g_Cert || !g_PrivateKey) { fprintf(stderr, "[cert] load failed\n"); return -1; }

  size_t clen = strlen(certStr);
  g_CertHex = malloc(clen * 2 + 1);
  for (size_t i = 0; i < clen; i++) sprintf(&g_CertHex[i * 2], "%02x", (unsigned char)certStr[i]);

  /* unique id: 16 hex chars, persisted. */
  char *id = read_file(idpath, NULL);
  if (id && strlen(id) >= 16) {
    id[16] = 0; g_UniqueId = strdup(id);
  } else {
    unsigned char r[8]; RAND_bytes(r, sizeof r);
    g_UniqueId = malloc(17);
    for (int i = 0; i < 8; i++) sprintf(&g_UniqueId[i * 2], "%02x", r[i]);
    write_file(idpath, g_UniqueId, 16);
  }
  free(id);
  free(certStr); free(keyStr);

  curl_global_init(CURL_GLOBAL_DEFAULT);
  fprintf(stderr, "[cert] ready, uniqueid=%s\n", g_UniqueId);
  return 0;
}
