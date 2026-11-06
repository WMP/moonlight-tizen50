/*
 * engine_pad.c - gamepad input for the web UI (see mlengine.h). Split from engine_ui.c so a host test can link it.
 */
#include <stdint.h>

#include "Limelight.h"
#include "mldiag.h"
#include "mltrace.h"

int ml_engine_running(void);

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int g_pad_traced;   /* state lines traced since the last arrival: the UI sends ~100/s */

int ml_engine_gamepad_arrival(int index, int active_mask, int type, unsigned int supported_buttons,
                              int capabilities) {
  if (index < 0 || index > 15 || !ml_engine_running()) { mldiag_pad(1, -1); return -1; }
  g_pad_traced = 0;
  int rc = LiSendControllerArrivalEvent((uint8_t)index, (uint16_t)active_mask, (uint8_t)type,
                                        supported_buttons, (uint16_t)capabilities);
  mldiag_pad(1, rc);
  MLT("ml_engine_gamepad_arrival idx=%d mask=0x%x type=%d buttons=0x%x caps=0x%x -> %d", index,
      active_mask, type, supported_buttons, capabilities, rc);
  return rc;
}

int ml_engine_gamepad_state(int index, int active_mask, int buttons, int left_trigger,
                            int right_trigger, int ls_x, int ls_y, int rs_x, int rs_y) {
  if (index < 0 || index > 15 || !ml_engine_running()) { mldiag_pad(0, -1); return -1; }
  int rc = LiSendMultiControllerEvent((short)index, (short)active_mask, buttons,
                                      (unsigned char)clampi(left_trigger, 0, 255),
                                      (unsigned char)clampi(right_trigger, 0, 255),
                                      (short)clampi(ls_x, -32768, 32767), (short)clampi(ls_y, -32768, 32767),
                                      (short)clampi(rs_x, -32768, 32767), (short)clampi(rs_y, -32768, 32767));
  mldiag_pad(0, rc);
  if (rc != 0 || g_pad_traced < 5) {
    g_pad_traced++;
    MLT("ml_engine_gamepad_state idx=%d mask=0x%x btn=0x%x -> %d", index, active_mask, buttons, rc);
  }
  return rc;
}
