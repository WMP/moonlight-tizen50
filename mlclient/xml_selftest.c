/*
 * Host self-test for gs_client.c's dependency-free xml_search/xml_status.
 * Build & run on the host (not the TV):
 *   gcc -D_GNU_SOURCE -I<mlt>/moonlight-common-c/src -I<mlt>/libgamestream \
 *       -o /tmp/xmltest mlclient/xml_selftest.c mlclient/gs_client.c \
 *       mlclient/xml_selftest_stubs.c -lcrypto && /tmp/xmltest
 * (xml_selftest_stubs.c stubs the http/gs externs gs_client.c links against.)
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int xml_search(char *, size_t, char *, char **);
int xml_status(char *, size_t);

int main(void) {
  char *r = NULL;
  char a[] = "<root status_code=\"200\"><appversion>7.1.431.0</appversion>"
             "<PairStatus>1</PairStatus><sessionUrl0>rtsp://1.2.3.4:48010</sessionUrl0></root>";
  assert(xml_status(a, strlen(a)) == 0);
  assert(xml_search(a, strlen(a), "appversion", &r) == 0 && !strcmp(r, "7.1.431.0")); free(r); r = NULL;
  assert(xml_search(a, strlen(a), "PairStatus", &r) == 0 && !strcmp(r, "1")); free(r); r = NULL;
  assert(xml_search(a, strlen(a), "sessionUrl0", &r) == 0 && !strcmp(r, "rtsp://1.2.3.4:48010")); free(r); r = NULL;

  /* a prefix tag must not match a longer-named tag */
  char b[] = "<appversionfoo>X</appversionfoo><appversion>9</appversion>";
  assert(xml_search(b, strlen(b), "appversion", &r) == 0 && !strcmp(r, "9")); free(r); r = NULL;

  /* a tag carrying attributes still yields its text */
  char c[] = "<state attr=\"x\">running</state>";
  assert(xml_search(c, strlen(c), "state", &r) == 0 && !strcmp(r, "running")); free(r); r = NULL;

  /* missing tag and non-200 status are reported as errors */
  assert(xml_search(a, strlen(a), "nope", &r) != 0);
  char d[] = "<root status_code=\"401\"></root>";
  assert(xml_status(d, strlen(d)) != 0);

  puts("xml selftest ok");
  return 0;
}
