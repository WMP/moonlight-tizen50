/* Host self-test for mlclient/ctlcrypto.c (needs OpenSSL). Run via tests/build-esplus-ecore-test.sh. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
#include "mldiag.h"

bool __wrap_PltEncryptMessage(void *ctx, int algorithm, int flags, unsigned char *key, int keyLength,
                              unsigned char *iv, int ivLength, unsigned char *tag, int tagLength,
                              unsigned char *inputData, int inputDataLength,
                              unsigned char *outputData, int *outputDataLength);

static int s_corrupt, s_calls;

/* correct AES-128-GCM; like the mbedTLS path it writes the tag at `tag` and the ciphertext after it */
bool __real_PltEncryptMessage(void *ctx, int algorithm, int flags, unsigned char *key, int keyLength,
                              unsigned char *iv, int ivLength, unsigned char *tag, int tagLength,
                              unsigned char *inputData, int inputDataLength,
                              unsigned char *outputData, int *outputDataLength) {
  (void)ctx; (void)flags; (void)keyLength; (void)tagLength;
  s_calls++;
  if (algorithm != 2) { memcpy(outputData, inputData, (size_t)inputDataLength); *outputDataLength = inputDataLength; return true; }
  unsigned char tmp[4096]; int n = 0, n2 = 0;
  memcpy(tmp, inputData, (size_t)inputDataLength);   /* output overwrites the input area */
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  EVP_EncryptInit_ex(c, EVP_aes_128_gcm(), NULL, NULL, NULL);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, ivLength, NULL);
  EVP_EncryptInit_ex(c, NULL, NULL, key, iv);
  EVP_EncryptUpdate(c, outputData, &n, tmp, inputDataLength);
  EVP_EncryptFinal_ex(c, outputData + n, &n2);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag);
  EVP_CIPHER_CTX_free(c);
  if (s_corrupt) tag[3] ^= 0x40;
  *outputDataLength = n + n2;
  return true;
}

static void enc(int ivlen, int len, int algorithm) {
  unsigned char key[16] = "0123456789abcdef", iv[16] = { 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 'C', 'C' };
  static unsigned char buf[16 + 2048]; unsigned char in[2048];
  memset(in, 0x5a, sizeof in);
  int outlen = 0;
  bool r = __wrap_PltEncryptMessage(NULL, algorithm, 0, key, 16, iv, ivlen, algorithm == 2 ? buf : NULL, algorithm == 2 ? 16 : 0,
                                    in, len, buf + 16, &outlen);
  assert(r && outlen == len);
}

int main(void) {
  char buf[4096];
  mldiag_cfg cfg = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, NULL, NULL, NULL, 0 };
  mldiag_reset(&cfg);
  enc(12, 40, 2); enc(16, 40, 2);
  mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "ctl enc=2 fail=0 iv=16 verify=2/0/0"));

  s_corrupt = 1;
  enc(12, 40, 2);
  mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "ctl enc=3 fail=0 iv=12 verify=2/1/0"));
  s_corrupt = 0;

  enc(12, 1500, 2);   /* above the copy limit: not verified */
  mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "ctl enc=4 fail=0 iv=12 verify=2/1/1"));

  int calls = s_calls;
  enc(16, 32, 1);   /* CBC: pass through, not counted */
  assert(s_calls == calls + 1);
  mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "ctl enc=4 "));
  puts(buf);
  puts("ctlcrypto_test OK");
  return 0;
}
