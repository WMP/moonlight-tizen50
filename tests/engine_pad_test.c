/* Host self-test for mlclient/engine_pad.c (stubs for Limelight and ml_engine_running). Run via tests/build-esplus-ecore-test.sh. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "Limelight.h"
#include "mldiag.h"

int ml_engine_running(void);
int ml_engine_gamepad_arrival(int index, int active_mask, int type, unsigned int supported_buttons, int capabilities);
int ml_engine_gamepad_state(int index, int active_mask, int buttons, int left_trigger, int right_trigger,
                            int ls_x, int ls_y, int rs_x, int rs_y);

static int g_running = 1, g_rc, g_calls;
static struct { int idx, mask, type, caps; unsigned btn; } A;
static struct { int idx, mask, btn, lt, rt, lsx, lsy, rsx, rsy; } S;

int ml_engine_running(void) { return g_running; }
int LiSendControllerArrivalEvent(uint8_t i, uint16_t m, uint8_t t, uint32_t b, uint16_t c) {
  g_calls++; A.idx = i; A.mask = m; A.type = t; A.btn = b; A.caps = c; return g_rc;
}
int LiSendMultiControllerEvent(short i, short m, int b, unsigned char lt, unsigned char rt, short lsx, short lsy, short rsx, short rsy) {
  g_calls++; S.idx = i; S.mask = m; S.btn = b; S.lt = lt; S.rt = rt; S.lsx = lsx; S.lsy = lsy; S.rsx = rsx; S.rsy = rsy; return g_rc;
}

static const mldiag_cfg CFG = { 1920, 1080, 60, 20000, 1392, 1, 1, 0, 0x30002, 1, 0, "a", "b", NULL, 0, NULL };

int main(void) {
  char buf[4096];
  mldiag_reset(&CFG);

  /* bad index / not running: -1, no Li* call */
  assert(ml_engine_gamepad_state(-1, 1, 0, 0, 0, 0, 0, 0, 0) == -1);
  assert(ml_engine_gamepad_state(16, 1, 0, 0, 0, 0, 0, 0, 0) == -1);
  assert(ml_engine_gamepad_arrival(-1, 1, 1, 0, 0) == -1);
  assert(ml_engine_gamepad_arrival(16, 1, 1, 0, 0) == -1);
  g_running = 0;
  assert(ml_engine_gamepad_state(0, 1, 0, 0, 0, 0, 0, 0, 0) == -1);
  assert(ml_engine_gamepad_arrival(0, 1, 1, 0, 0) == -1);
  g_running = 1;
  assert(g_calls == 0);

  /* clamps */
  assert(ml_engine_gamepad_state(2, 3, 0x1000, 300, -5, 40000, -40000, 32767, -32768) == 0);
  assert(g_calls == 1 && S.idx == 2 && S.mask == 3 && S.btn == 0x1000);
  assert(S.lt == 255 && S.rt == 0);
  assert(S.lsx == 32767 && S.lsy == -32768 && S.rsx == 32767 && S.rsy == -32768);
  assert(ml_engine_gamepad_state(0, 1, 0, 128, 7, 100, -100, 0, 0) == 0);
  assert(S.lt == 128 && S.rt == 7 && S.lsx == 100 && S.lsy == -100);

  /* arrival passes through */
  assert(ml_engine_gamepad_arrival(1, 5, 2, 0xF0F0u, 0x3) == 0);
  assert(g_calls == 3 && A.idx == 1 && A.mask == 5 && A.type == 2 && A.btn == 0xF0F0u && A.caps == 3);

  /* Li* failure is returned and counted */
  g_rc = -7;
  assert(ml_engine_gamepad_state(0, 1, 0, 0, 0, 0, 0, 0, 0) == -7);

  /* counters: 4 arrival calls (3 early -1), 6 state calls (3 early -1, 1 Li failure) */
  mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(buf, sizeof buf);
  assert(strstr(buf, " | pad arr=4 st=6 fail=7/-7 | cc "));
  puts("engine_pad_test OK");
  return 0;
}
