/*
 * input_evdev.c - native input for the Moonlight client (M3), ported "dry" from
 * moonlight-embedded's evdev input. Reads /dev/input/event* (stable kernel ABI
 * via <linux/input.h>) in a thread and forwards to moonlight-common-c:
 *   keyboard -> LiSendKeyboardEvent, mouse -> LiSendMouseMove/Button/Scroll,
 *   gamepad  -> LiSendMultiControllerEvent.
 *
 * NOTE: UNTESTED on the TV. USB keyboard/mouse/gamepad appear as evdev devices;
 * whether the Tizen TV remote also arrives on evdev (vs CEC/uinput) must be
 * checked on-device - see input_scan() logging.
 *
 * shortcut: keymap covers the common keys only; extend map_vk() as needed.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "Limelight.h"

/* Older kernel headers (Tizen 5.0 / 4.1) lack the BTN_SOUTH/EAST/NORTH/WEST
   aliases; they map to the classic names. */
#ifndef BTN_SOUTH
#define BTN_SOUTH BTN_A
#define BTN_EAST  BTN_B
#define BTN_NORTH BTN_X
#define BTN_WEST  BTN_Y
#endif

#define MAX_DEV 32

static int s_fds[MAX_DEV];
static int s_nfds;
static pthread_t s_thread;
static volatile int s_run;

/* gamepad accumulated state (single controller, number 0) */
static int s_btn;
static short s_lx, s_ly, s_rx, s_ry;
static unsigned char s_lt, s_rt;

/* Linux KEY_* -> Windows virtual-key code (what the host expects). */
static short map_vk(int code) {
  if (code >= KEY_1 && code <= KEY_9) return 0x31 + (code - KEY_1); /* '1'..'9' */
  if (code == KEY_0) return 0x30;
  switch (code) {
    case KEY_A: return 0x41; case KEY_B: return 0x42; case KEY_C: return 0x43;
    case KEY_D: return 0x44; case KEY_E: return 0x45; case KEY_F: return 0x46;
    case KEY_G: return 0x47; case KEY_H: return 0x48; case KEY_I: return 0x49;
    case KEY_J: return 0x4A; case KEY_K: return 0x4B; case KEY_L: return 0x4C;
    case KEY_M: return 0x4D; case KEY_N: return 0x4E; case KEY_O: return 0x4F;
    case KEY_P: return 0x50; case KEY_Q: return 0x51; case KEY_R: return 0x52;
    case KEY_S: return 0x53; case KEY_T: return 0x54; case KEY_U: return 0x55;
    case KEY_V: return 0x56; case KEY_W: return 0x57; case KEY_X: return 0x58;
    case KEY_Y: return 0x59; case KEY_Z: return 0x5A;
    case KEY_ENTER: case KEY_KPENTER: return 0x0D;
    case KEY_ESC: return 0x1B;
    case KEY_BACKSPACE: return 0x08;
    case KEY_TAB: return 0x09;
    case KEY_SPACE: return 0x20;
    case KEY_UP: return 0x26; case KEY_DOWN: return 0x28;
    case KEY_LEFT: return 0x25; case KEY_RIGHT: return 0x27;
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return 0xA0;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: return 0xA2;
    case KEY_LEFTALT: case KEY_RIGHTALT: return 0xA4;
    case KEY_DELETE: return 0x2E; case KEY_HOME: return 0x24; case KEY_END: return 0x23;
    default: return 0;
  }
}

static char s_mods;
static void upd_mod(int code, int down) {
  char m = 0;
  if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) m = MODIFIER_SHIFT;
  else if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) m = MODIFIER_CTRL;
  else if (code == KEY_LEFTALT || code == KEY_RIGHTALT) m = MODIFIER_ALT;
  if (!m) return;
  if (down) s_mods |= m; else s_mods &= ~m;
}

static int gp_flag(int code) {
  switch (code) {
    case BTN_SOUTH: return A_FLAG; case BTN_EAST: return B_FLAG;
    case BTN_NORTH: return X_FLAG; case BTN_WEST: return Y_FLAG;
    case BTN_TL: return LB_FLAG; case BTN_TR: return RB_FLAG;
    case BTN_SELECT: return BACK_FLAG; case BTN_START: return PLAY_FLAG;
    case BTN_THUMBL: return LS_CLK_FLAG; case BTN_THUMBR: return RS_CLK_FLAG;
    case BTN_MODE: return SPECIAL_FLAG;
    default: return 0;
  }
}

static void send_pad(void) {
  LiSendMultiControllerEvent(0, 0x1, s_btn, s_lt, s_rt, s_lx, s_ly, s_rx, s_ry);
}

static void handle(struct input_event *e) {
  if (e->type == EV_KEY) {
    int down = e->value != 0; /* 1 press, 2 autorepeat, 0 release */
    if (e->code == BTN_LEFT) LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    else if (e->code == BTN_RIGHT) LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    else if (e->code == BTN_MIDDLE) LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
    else if (e->code >= BTN_GAMEPAD && e->code <= BTN_THUMBR) {
      int f = gp_flag(e->code);
      if (f) { if (down) s_btn |= f; else s_btn &= ~f; send_pad(); }
    } else {
      short vk = map_vk(e->code);
      if (vk) {
        upd_mod(e->code, down);
        if (e->value != 2) /* ignore autorepeat for key events */
          LiSendKeyboardEvent(vk, down ? KEY_ACTION_DOWN : KEY_ACTION_UP, s_mods);
      }
    }
  } else if (e->type == EV_REL) {
    if (e->code == REL_X) LiSendMouseMoveEvent((short)e->value, 0);
    else if (e->code == REL_Y) LiSendMouseMoveEvent(0, (short)e->value);
    else if (e->code == REL_WHEEL) LiSendScrollEvent((signed char)e->value);
  } else if (e->type == EV_ABS) {
    /* axis range assumed signed 16-bit from the driver; triggers 0..255. */
    switch (e->code) {
      case ABS_X: s_lx = (short)e->value; send_pad(); break;
      case ABS_Y: s_ly = (short)-e->value; send_pad(); break;
      case ABS_RX: s_rx = (short)e->value; send_pad(); break;
      case ABS_RY: s_ry = (short)-e->value; send_pad(); break;
      case ABS_Z: s_lt = (unsigned char)(e->value & 0xff); send_pad(); break;
      case ABS_RZ: s_rt = (unsigned char)(e->value & 0xff); send_pad(); break;
      case ABS_HAT0X:
        s_btn &= ~(LEFT_FLAG | RIGHT_FLAG);
        if (e->value < 0) s_btn |= LEFT_FLAG; else if (e->value > 0) s_btn |= RIGHT_FLAG;
        send_pad(); break;
      case ABS_HAT0Y:
        s_btn &= ~(UP_FLAG | DOWN_FLAG);
        if (e->value < 0) s_btn |= UP_FLAG; else if (e->value > 0) s_btn |= DOWN_FLAG;
        send_pad(); break;
    }
  }
}

static void *reader(void *arg) {
  (void)arg;
  struct pollfd pfd[MAX_DEV];
  for (int i = 0; i < s_nfds; i++) { pfd[i].fd = s_fds[i]; pfd[i].events = POLLIN; }
  while (s_run) {
    int n = poll(pfd, s_nfds, 200);
    if (n <= 0) continue;
    for (int i = 0; i < s_nfds; i++) {
      if (!(pfd[i].revents & POLLIN)) continue;
      struct input_event ev[32];
      ssize_t r = read(pfd[i].fd, ev, sizeof ev);
      for (ssize_t k = 0; k >= 0 && (size_t)(k + 1) * sizeof(ev[0]) <= (size_t)r; k++) handle(&ev[k]);
    }
  }
  return NULL;
}

int input_init(void) {
  DIR *d = opendir("/dev/input");
  if (!d) { fprintf(stderr, "[input] no /dev/input\n"); return -1; }
  struct dirent *de;
  while ((de = readdir(d)) && s_nfds < MAX_DEV) {
    if (strncmp(de->d_name, "event", 5)) continue;
    char path[300]; snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;
    char name[128] = "?"; ioctl(fd, EVIOCGNAME(sizeof name), name);
    fprintf(stderr, "[input] %s: %s\n", path, name);
    s_fds[s_nfds++] = fd;
  }
  closedir(d);
  if (!s_nfds) { fprintf(stderr, "[input] no readable devices (need permissions / root)\n"); return -1; }
  s_run = 1;
  return pthread_create(&s_thread, NULL, reader, NULL);
}

void input_shutdown(void) {
  s_run = 0;
  if (s_thread) pthread_join(s_thread, NULL);
  for (int i = 0; i < s_nfds; i++) close(s_fds[i]);
  s_nfds = 0;
}
