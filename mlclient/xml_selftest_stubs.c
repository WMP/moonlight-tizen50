/* Stubs for the externs gs_client.c references, so xml_selftest.c can link on the
 * host without curl/openssl-ssl or the real pairing code. */
#include <stddef.h>
#include "http.h"
PHTTP_DATA http_create_data(void) { static HTTP_DATA d; return &d; }
int http_request(const char *a, const char *b, PHTTP_DATA c) { (void)a; (void)b; (void)c; return 0; }
void http_free_data(PHTTP_DATA d) { (void)d; }
char *g_UniqueId = (char *)"0";
int gs_pair(int a, const char *b, unsigned short c, const char *d, char **e) {
  (void)a; (void)b; (void)c; (void)d; (void)e; return 0;
}
const char *LiGetLaunchUrlQueryParameters(void) { return ""; }
