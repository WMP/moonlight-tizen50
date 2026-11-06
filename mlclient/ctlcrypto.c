/* ctlcrypto.c - engine only (linked with -Wl,--wrap=PltEncryptMessage, see build-engine-so.sh).
 * Checks every control-stream AES-GCM encrypt: decrypts the result again with OpenSSL and compares
 * it with the plaintext. The result goes to the ML-ERROR line (mldiag_ctl_encrypt). Never keeps or
 * traces the key, plaintext or tag. Host self-test: tests/ctlcrypto_test.c. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
#include "mldiag.h"
#include "mltrace.h"

#define ALGORITHM_AES_GCM 2   /* PlatformCrypto.h */
#define COPY_MAX 1024         /* larger messages are not verified */

bool __real_PltEncryptMessage(void *ctx, int algorithm, int flags, unsigned char *key, int keyLength,
                              unsigned char *iv, int ivLength, unsigned char *tag, int tagLength,
                              unsigned char *inputData, int inputDataLength,
                              unsigned char *outputData, int *outputDataLength);

/* 1 = the ciphertext decrypts to plain, 0 = it does not */
static int verify_gcm(const unsigned char *key, const unsigned char *iv, int ivLength, const unsigned char *tag,
                      const unsigned char *ct, int ctLen, const unsigned char *plain) {
  unsigned char dec[COPY_MAX];
  int ok = 0, n = 0, n2 = 0;
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  if (!c) return 0;
  if (EVP_DecryptInit_ex(c, EVP_aes_128_gcm(), NULL, NULL, NULL) == 1 &&
      EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, ivLength, NULL) == 1 &&
      EVP_DecryptInit_ex(c, NULL, NULL, key, iv) == 1 &&
      EVP_DecryptUpdate(c, dec, &n, ct, ctLen) == 1 &&
      EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) == 1 &&
      EVP_DecryptFinal_ex(c, dec + n, &n2) == 1)
    ok = n + n2 == ctLen && memcmp(dec, plain, (size_t)ctLen) == 0;
  EVP_CIPHER_CTX_free(c);
  return ok;
}

bool __wrap_PltEncryptMessage(void *ctx, int algorithm, int flags, unsigned char *key, int keyLength,
                              unsigned char *iv, int ivLength, unsigned char *tag, int tagLength,
                              unsigned char *inputData, int inputDataLength,
                              unsigned char *outputData, int *outputDataLength) {
  if (algorithm != ALGORITHM_AES_GCM || !tag)
    return __real_PltEncryptMessage(ctx, algorithm, flags, key, keyLength, iv, ivLength, tag, tagLength,
                                    inputData, inputDataLength, outputData, outputDataLength);
  unsigned char plain[COPY_MAX];   /* the mbedTLS path overwrites [tag, tag+16+len) */
  bool copied = inputDataLength >= 0 && inputDataLength <= COPY_MAX && keyLength == 16 && tagLength == 16;
  if (copied) memcpy(plain, inputData, (size_t)inputDataLength);
  bool r = __real_PltEncryptMessage(ctx, algorithm, flags, key, keyLength, iv, ivLength, tag, tagLength,
                                    inputData, inputDataLength, outputData, outputDataLength);
  int v = -1;
  if (r && copied && outputDataLength && *outputDataLength == inputDataLength)
    v = verify_gcm(key, iv, ivLength, tag, outputData, *outputDataLength, plain);
  unsigned call = mldiag_ctl_encrypt(r, ivLength, v);   /* per session: the trace below covers each session's first 8 */
#ifdef MLT_TRACE
  if (call <= 8 || v == 0) {
    char hex[33] = "";
    for (int i = 0; i < ivLength && i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", iv[i]);
    MLT("ctl gcm #%u ivlen=%d iv=%s in=%d out=%d real=%d verify=%d", call, ivLength, hex, inputDataLength,
        outputDataLength ? *outputDataLength : -1, (int)r, v);
  }
#else
  (void)call;
#endif
  return r;
}
