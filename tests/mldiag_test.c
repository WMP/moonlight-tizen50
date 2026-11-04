/* Host self-test for mlclient/mldiag.c. Run via tests/build-esplus-ecore-test.sh, or:
   gcc -std=gnu11 -Wall -Wextra -Werror -Imlclient -o /tmp/mldiag_test tests/mldiag_test.c mlclient/mldiag.c -lpthread && /tmp/mldiag_test */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mldiag.h"

static const mldiag_cfg CFG = { 1920, 1080, 60, 20000, 1392, 1, 1, 0, 0x30002, 1, 0, "7.1.431.-1", "3.23.0.74" };

static void fail(int code) { mldiag_fail(code, -1, NULL, 0, 2, 1, 0, 0); }

int main(void) {
  char buf[4096];
  mldiag_reset(&CFG);
  assert(mldiag_last(buf, sizeof buf) == 0 && buf[0] == 0);

  /* counters + first_pkt_ms */
  mldiag_cc_line("Received first video packet after 12 ms\n");
  mldiag_cc_line("Unrecoverable frame 5\n");
  mldiag_cc_line("Network dropped 3 frames\n");
  mldiag_cc_line("Waiting for IDR frame\n");
  mldiag_cc_line("Depacketizer detected corrupt frame\n");
  mldiag_cc_line("Recovery error\n");
  mldiag_cc_line("Requesting IDR frame on behalf of video decoder\n");
  mldiag_cc_line("Sending RFI request\n");
  mldiag_cc_line("Recovered 2 packets\n");
  mldiag_cc_line("Video decode unit queue overflow\n");
  mldiag_video_setup(1, 1920, 1080, 60, 0);
  mldiag_video_du(0); mldiag_video_du(1);
  mldiag_submit(1, 0); mldiag_submit(0, 0); mldiag_submit(0, 3);
  mldiag_player("open", 1); mldiag_player("win", 0); mldiag_player("prep", 1); mldiag_player("pdone", 1); mldiag_player("start", 1);
  mldiag_fail(-101, -1, NULL, 0, 2, 1, 7, 9);
  assert(mldiag_last(buf, sizeof buf) == -101);
  assert(strncmp(buf, "ML-ERROR code=-101 name=NO_VIDEO_FRAME stage=- t=", 49) == 0);
  assert(strstr(buf, "cfg 1920x1080@60 br=20000 pkt=1392 fmts=0x1 cs=1 cr=0 audio=0x30002 scm=0x1 enc=0x0 app=7.1.431.-1 gfe=3.23.0.74 hostflags=0x0 rtt=2/1"));
  assert(strstr(buf, "first_pkt_ms=12"));
  assert(strstr(buf, "du=2 idr=1"));
  assert(strstr(buf, "open=1 win=0 prep=1 pdone=1 start=1 vsub=1 asub=2 subfail=1/3 err=7/9"));
  assert(strstr(buf, "cc unrec=1 drop=1 waitidr=1 badlen=0 decrypt=0 corrupt=2 unkft=0 idrreq=2 fec=1 qovf=1 droplim=0"));
  assert(!strchr(buf, '\n'));
  puts(buf);

  /* name mapping for an unknown code; stage name */
  mldiag_reset(&CFG);
  mldiag_fail(-7, 3, "RTSP handshake", 0, 0, 0, 0, 0);
  assert(mldiag_last(buf, sizeof buf) == -7);
  assert(strstr(buf, "name=OTHER stage=RTSP handshake"));

  /* first 4 / last 8 ring with 20 lines */
  mldiag_reset(&CFG);
  for (int i = 0; i < 20; i++) { char l[32]; snprintf(l, sizeof l, "line%02d\n", i); mldiag_cc_line(l); }
  fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "cc-first: line00 ; line01 ; line02 ; line03 | cc-last: line12 ; line13 ; line14 ; line15 ; line16 ; line17 ; line18 ; line19"));

  /* fewer than 4 lines */
  mldiag_reset(&CFG);
  mldiag_cc_line("only"); fail(-101); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, "cc-first: only | cc-last: only"));

  /* very long cc line: truncated to 160, whole buffer bounded */
  mldiag_reset(&CFG);
  char big[5000]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
  for (int i = 0; i < 20; i++) mldiag_cc_line(big);
  fail(-101);
  assert(mldiag_last(buf, sizeof buf) == -101);
  assert(strlen(buf) < 2048);
  char x161[162], x160[162]; memset(x161, 'x', 161); x161[161] = 0; memcpy(x160, x161, 162); x160[160] = 0;
  assert(strstr(buf, x160) && !strstr(buf, x161));   /* each cc line cut to 160 chars */

  /* small len, NULL buf */
  char small[10]; memset(small, 'z', sizeof small);
  assert(mldiag_last(small, sizeof small) == -101);
  assert(strlen(small) == 9 && strncmp(small, "ML-ERROR ", 9) == 0);
  assert(mldiag_last(NULL, 100) == -101);
  assert(mldiag_last(buf, 0) == -101);

  /* reset clears */
  mldiag_reset(&CFG);
  assert(mldiag_last(buf, sizeof buf) == 0 && buf[0] == 0);
  puts("mldiag_test OK");
  return 0;
}
