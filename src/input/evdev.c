/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "evdev.h"

#include "keyboard.h"

#include "../loop.h"

#include "libevdev/libevdev.h"
#include <Limelight.h>

#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#ifdef __linux__
#include <endian.h>
#else
#include <sys/endian.h>
#endif
#include <math.h>

#define MAX_MT_TOUCH_FINGER 5
#define ONE_FINGER_EVENT 0x01
#define TWO_FINGER_EVENT 0x02
#define THREE_FINGER_EVENT 0x04
#define FOUR_FINGER_EVENT 0x08
#define FIVE_FINGER_EVENT 0x10
#define ZERO_FINGER_EVENT 0x20
// reserve 0x40 for repeat event
#define MOVING_MT_EVENT 0x100
#define NO_SCROLL_MT_EVENT 0x200
#define NEED_SPECIAL_FINGER_EVENT 0x1000
#define SPECIAL_FINGER_EVENT 0x2000
#define LEFT_MT_EVENT 0x4000
#define RIGHT_MT_EVENT 0x8000
#define FINGER_EVENT_MASK 0xFF
#define CLEAN_MT_EVENT_MASK 0xF000

static int keyboardpipefd = -1;
static const evwcode quitstate = QUITCODE;
static const evwcode grabcode = GRABCODE;
static const evwcode ungrabcode = UNGRABCODE;
static const evwcode fakegrabcode = FAKEGRABCODE;

static bool not_handle_mouse_motion = false;
static bool not_handle_mouse_motion_var = false;
static bool waitingToSwitchGrabOnModifierUp = false;
static bool isGrabKeyRelease = false;
static bool isUseKbdmux = false;
static bool fakeGrab = false;
static bool fakeGrabKey = false;
static bool isInputing = true;
static bool sdlgp = false;
static bool swapXYAB = false;
static bool gameMode = false;
// for check gamepad num,evdev_check_input function
static int gpNumForCheck = 0;
static enum grabWindowRequest isGrabed = E_UNGRAB_WINDOW;

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define int16_to_le(val) val
#else
#define int16_to_le(val) ((((val) >> 8) & 0x00FF) | (((val) << 8) & 0xFF00))
#endif

struct input_abs_parms {
  int min, max;
  int flat;
  int avg;
  int range, diff;
};

struct input_device {
  struct libevdev *dev;
  bool is_keyboard;
  bool is_mouse;
  bool is_touchscreen;
  int rotate;
  struct mapping* map;
  int key_map[KEY_CNT];
  int abs_map[ABS_CNT];
  int hats_state[3][2];
  int fd;
  char *path;
  char modifiers;
  #ifdef __linux__
  __s32 mouseDeltaX, mouseDeltaY, mouseVScroll, mouseHScroll;
  __s32 touchDownX, touchDownY, touchX, touchY;
  #else
  int32_t mouseDeltaX, mouseDeltaY, mouseVScroll, mouseHScroll;
  int32_t touchDownX, touchDownY, touchX, touchY;
  #endif
  struct {
    int32_t distanceX[MAX_MT_TOUCH_FINGER];
    int32_t distanceY[MAX_MT_TOUCH_FINGER];
    int32_t slotValueX[MAX_MT_TOUCH_FINGER];
    int32_t slotValueY[MAX_MT_TOUCH_FINGER];
    int32_t lastX[MAX_MT_TOUCH_FINGER];
    int32_t lastY[MAX_MT_TOUCH_FINGER];
    struct timeval touchDownTime[MAX_MT_TOUCH_FINGER];
    uint32_t up_event;
    uint32_t down_event;
    uint32_t pre_up_event;
    uint32_t pre_down_event;
    uint32_t end_event;
    uint32_t mtSlot;
    uint32_t upEventSlot;
    uint32_t downEventSlot;
    uint32_t specialEventSlot;
    uint32_t fingerMaxNum;
    uint32_t fingersNum;
    bool isMoving;
    bool needWait;
    uint32_t mtEvent;
    uint32_t mtEventLast;
    int interval;
    int rightIndex; // for gamemode
    int leftIndex; // for gamemode
  } mt_info; // precision touchpda
  int mtPalm;
  int mtXMax;
  int mtYMax;
  uint32_t mtLastEvent;
  struct timeval touchUpTime;
  struct timeval touchDownTime;
  struct timeval btnDownTime;
  short controllerId;
  int haptic_effect_id;
  int buttonFlags;
  unsigned char leftTrigger, rightTrigger;
  short leftStickX, leftStickY;
  short rightStickX, rightStickY;
  bool gamepadModified;
  bool mouseEmulation;
  pthread_t meThread;
  struct input_abs_parms xParms, yParms, rxParms, ryParms, zParms, rzParms;
  struct input_abs_parms leftParms, rightParms, upParms, downParms;
};

#define HAT_UP 1
#define HAT_RIGHT 2
#define HAT_DOWN 4
#define HAT_LEFT 8
static const int hat_constants[3][3] = {{HAT_UP | HAT_LEFT, HAT_UP, HAT_UP | HAT_RIGHT}, {HAT_LEFT, 0, HAT_RIGHT}, {HAT_LEFT | HAT_DOWN, HAT_DOWN, HAT_DOWN | HAT_RIGHT}};

#define set_hat(flags, flag, hat, hat_flag) flags = (hat & hat_flag) == hat_flag ? flags | flag : flags & ~flag

#define TOUCH_UP -1
#define TOUCH_CLICK_RADIUS 10
#define TOUCH_CLICK_DELAY 100000 // microseconds
#define TOUCH_RCLICK_TIME 750 // milliseconds

// How long the Start button must be pressed to toggle mouse emulation
#define MOUSE_EMULATION_LONG_PRESS_TIME 750
// How long between polling the gamepad to send virtual mouse input
#define MOUSE_EMULATION_POLLING_INTERVAL 50000
// Determines how fast the mouse will move each interval
#define MOUSE_EMULATION_MOTION_MULTIPLIER 3
// Determines the maximum motion amount before allowing movement
#define MOUSE_EMULATION_DEADZONE 2

// Limited by number of bits in activeGamepadMask
#define MAX_GAMEPADS 16

LIST_HEAD(head_of_list, List_Node);
static struct head_of_list first_node;
static struct head_of_list *head_device = &first_node;
static bool verboseMe = false;
static int numDevices = 0;
static int assignedControllerIds = 0;

static short* currentKey;
static short* currentHat;
static short* currentHatDir;
static short* currentAbs;
static bool* currentReverse;

static bool grabbingDevices = false;
static bool mouseEmulationEnabled;

static bool waitingToExitOnModifiersUp = false;

int evdev_gamepads = 0;

#define ACTION_MODIFIERS (MODIFIER_SHIFT|MODIFIER_ALT|MODIFIER_CTRL)
#define QUIT_KEY KEY_Q
#define GRAB_KEY KEY_Z
#define FAKE_GRAB_KEY KEY_M
#define GAME_MODE_KEY KEY_G
#define QUIT_BUTTONS (PLAY_FLAG|BACK_FLAG|LB_FLAG|RB_FLAG)

static bool (*handler) (struct input_event*, struct input_device*);
static int evdev_handle(int fd, void *data);
static int mt_evdev_handle(int fd, void *data, int interval, uint32_t event, int slot);

static int evdev_get_map(int* map, int length, int value) {
  for (int i = 0; i < length; i++) {
    if (value == map[i])
      return i;
  }
  return -1;
}

static void fake_grab_window() {
  fakeGrab = true;
  sync_input_state(true);
#if defined(HAVE_X11) || defined(HAVE_WAYLAND) || defined(HAVE_DRM)
  if (keyboardpipefd > -1) {
    write(keyboardpipefd, &fakegrabcode, sizeof(fakegrabcode));
  }
#endif
}

static short keystatlist[0xFF];
static void keyrelease(int keycode) {
  keystatlist[keycode] = 0;
}
static void keypress(int keycode) {
  keystatlist[keycode] = 1;
}
static void freeallkey () {
  for (int i=0;i<0xFF;i++) {
    if (keystatlist[i] == 1) {
      keystatlist[i] = 0;
      LiSendKeyboardEvent(0x80 << 8 | keyCodes[i], KEY_ACTION_UP, 0);
    }
  }
}

static bool evdev_init_parms(struct input_device *dev, struct input_abs_parms *parms, int code) {
  int abs = evdev_get_map(dev->abs_map, ABS_MAX, code);

  if (abs >= 0) {
    parms->flat = libevdev_get_abs_flat(dev->dev, abs);
    parms->min = libevdev_get_abs_minimum(dev->dev, abs);
    parms->max = libevdev_get_abs_maximum(dev->dev, abs);
    if (parms->flat == 0 && parms->min == 0 && parms->max == 0)
      return false;

    parms->avg = (parms->min+parms->max)/2;
    parms->range = parms->max - parms->avg;
    parms->diff = parms->max - parms->min;
  }
  return true;
}

static void evdev_remove_device(struct input_device *dis_device, const char *path, int opt) {
  // opt is 1 means remove all device
  struct List_Node *nodePtr = NULL;
  LIST_FOREACH(nodePtr, head_device, node) {
    struct input_device *device = (struct input_device*)nodePtr->data;
    if(((device) == dis_device && dis_device != NULL) || opt == 1 || (path != NULL && strcmp(device->path, path) == 0)) {  
      numDevices--;
      if (verboseMe || opt != 1)
        printf("Input device removed: %s (player %d)\n", libevdev_get_name(device->dev), device->controllerId + 1);

      // remove from loop first
      loop_remove_fd(device->fd);

      // drain all event
      struct input_event ev;
      while (libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) >= 0);

      // clear device
      if (device->controllerId >= 0) {
        gpNumForCheck--;
        evdev_gamepads--;
        assignedControllerIds &= ~(1 << device->controllerId);
        LiSendMultiControllerEvent(device->controllerId, assignedControllerIds, 0, 0, 0, 0, 0, 0, 0);
      }
      if (device->mouseEmulation) {
        device->mouseEmulation = false;
        pthread_join(device->meThread, NULL);
      }

      free(device->path);
      libevdev_free(device->dev);
      close(device->fd);

      // remove device
      LIST_REMOVE(nodePtr, node);
      free(nodePtr->data);
      free(nodePtr);

      if (opt != 1)
        break;
    }
  }
}

static void evdev_remove_all(void) {
  evdev_remove_device(NULL, NULL, 1);
}

static void evdev_remove(struct input_device *device) {
  evdev_remove_device(device, NULL, 0);
}

void evdev_remove_from_path(const char* path) {
  evdev_remove_device(NULL, path, 0);
}

static short evdev_convert_value(struct input_event *ev, struct input_device *dev, struct input_abs_parms *parms, bool reverse) {
  if (parms->max == 0 && parms->min == 0) {
    fprintf(stderr, "Axis not found: %d\n", ev->code);
    return 0;
  }

  if (abs(ev->value - parms->avg) < parms->flat)
    return 0;
  else if (ev->value > parms->max)
    return reverse?SHRT_MIN:SHRT_MAX;
  else if (ev->value < parms->min)
    return reverse?SHRT_MAX:SHRT_MIN;
  else if (reverse)
    return (long long)(parms->max - (ev->value<parms->avg?parms->flat*2:0) - ev->value) * (SHRT_MAX-SHRT_MIN) / (parms->max-parms->min-parms->flat*2) + SHRT_MIN;
  else
    return (long long)(ev->value - (ev->value>parms->avg?parms->flat*2:0) - parms->min) * (SHRT_MAX-SHRT_MIN) / (parms->max-parms->min-parms->flat*2) + SHRT_MIN;
}

static unsigned char evdev_convert_value_byte(struct input_event *ev, struct input_device *dev, struct input_abs_parms *parms, char halfaxis) {
  if (parms->max == 0 && parms->min == 0) {
    fprintf(stderr, "Axis not found: %d\n", ev->code);
    return 0;
  }

  if (halfaxis == 0) {
    if (abs(ev->value-parms->min)<parms->flat)
      return 0;
    else if (ev->value>parms->max)
      return UCHAR_MAX;
    else
      return (ev->value - parms->flat - parms->min) * UCHAR_MAX / (parms->diff - parms->flat);
  } else {
    short val = evdev_convert_value(ev, dev, parms, false);
    if (halfaxis == '-' && val < 0)
      return -(int)val * UCHAR_MAX / (SHRT_MAX-SHRT_MIN);
    else if (halfaxis == '+' && val > 0)
      return (int)val * UCHAR_MAX / (SHRT_MAX-SHRT_MIN);
    else
      return 0;
  }
}

void *HandleMouseEmulation(void* param)
{
  struct input_device* dev = (struct input_device*) param;

  while (dev->mouseEmulation) {
    usleep(MOUSE_EMULATION_POLLING_INTERVAL);

    short rawX;
    short rawY;

    // Determine which analog stick is currently receiving the strongest input
    if ((uint32_t)abs(dev->leftStickX) + abs(dev->leftStickY) > (uint32_t)abs(dev->rightStickX) + abs(dev->rightStickY)) {
      rawX = dev->leftStickX;
      rawY = dev->leftStickY;
    } else {
      rawX = dev->rightStickX;
      rawY = dev->rightStickY;
    }

    float deltaX;
    float deltaY;

    // Produce a base vector for mouse movement with increased speed as we deviate further from center
    deltaX = pow((float)rawX / 32767.0f * MOUSE_EMULATION_MOTION_MULTIPLIER, 3);
    deltaY = pow((float)rawY / 32767.0f * MOUSE_EMULATION_MOTION_MULTIPLIER, 3);

    // Enforce deadzones
    deltaX = fabs(deltaX) > MOUSE_EMULATION_DEADZONE ? deltaX - MOUSE_EMULATION_DEADZONE : 0;
    deltaY = fabs(deltaY) > MOUSE_EMULATION_DEADZONE ? deltaY - MOUSE_EMULATION_DEADZONE : 0;

    if (deltaX != 0 || deltaY != 0)
      LiSendMouseMoveEvent(deltaX, -deltaY);
  }

  return NULL;
}

#define SET_BTN_FLAG(x, y) supportedButtonFlags |= (x >= 0) ? y : 0

static void send_controller_arrival(struct input_device *dev) {
  unsigned char type = LI_CTYPE_UNKNOWN;
  unsigned int supportedButtonFlags = 0;
  unsigned short capabilities = 0;
  // for check gamepad num
  gpNumForCheck++;
  evdev_gamepads++;

  switch (libevdev_get_id_vendor(dev->dev)) {
  case 0x045e: // Microsoft
    type = LI_CTYPE_XBOX;
    break;
  case 0x054c: // Sony
    type = LI_CTYPE_PS;
    break;
  case 0x057e: // Nintendo
    type = LI_CTYPE_NINTENDO;
    break;
  }

  const char* name = libevdev_get_name(dev->dev);
  if (name && type == LI_CTYPE_UNKNOWN) {

    // Try to guess based on the name
    if (strstr(name, "Xbox") || strstr(name, "X-Box") || strstr(name, "XBox") || strstr(name, "XBOX")) {
      type = LI_CTYPE_XBOX;
    }
  }

  SET_BTN_FLAG(dev->map->btn_a, A_FLAG);
  SET_BTN_FLAG(dev->map->btn_b, B_FLAG);
  SET_BTN_FLAG(dev->map->btn_x, X_FLAG);
  SET_BTN_FLAG(dev->map->btn_y, Y_FLAG);
  SET_BTN_FLAG(dev->map->btn_back, BACK_FLAG);
  SET_BTN_FLAG(dev->map->btn_start, PLAY_FLAG);
  SET_BTN_FLAG(dev->map->btn_guide, SPECIAL_FLAG);
  SET_BTN_FLAG(dev->map->btn_leftstick, LS_CLK_FLAG);
  SET_BTN_FLAG(dev->map->btn_rightstick, RS_CLK_FLAG);
  SET_BTN_FLAG(dev->map->btn_leftshoulder, LB_FLAG);
  SET_BTN_FLAG(dev->map->btn_rightshoulder, RB_FLAG);
  SET_BTN_FLAG(dev->map->btn_misc1, MISC_FLAG);
  SET_BTN_FLAG(dev->map->btn_paddle1, PADDLE1_FLAG);
  SET_BTN_FLAG(dev->map->btn_paddle2, PADDLE2_FLAG);
  SET_BTN_FLAG(dev->map->btn_paddle3, PADDLE3_FLAG);
  SET_BTN_FLAG(dev->map->btn_paddle4, PADDLE4_FLAG);
  SET_BTN_FLAG(dev->map->btn_touchpad, TOUCHPAD_FLAG);

  if (dev->map->abs_lefttrigger >= 0 && dev->map->abs_righttrigger >= 0)
    capabilities |= LI_CCAP_ANALOG_TRIGGERS;

  // TODO: Probe for this properly
  capabilities |= LI_CCAP_RUMBLE;

  LiSendControllerArrivalEvent(dev->controllerId, assignedControllerIds, type,
                               supportedButtonFlags, capabilities);
}

static bool evdev_mt_touchpad_handle_event(struct input_event *ev, struct input_device *dev) {
  bool needSpecialEvent = false;

  if (!isInputing)
    return true;
  switch (ev->type) {
  case EV_SYN:

    if (dev->mt_info.mtEventLast != dev->mt_info.mtEvent) {
      dev->mt_info.isMoving = false;
      dev->mt_info.needWait = true;
      dev->mt_info.interval = (dev->mt_info.mtEventLast < dev->mt_info.mtEvent ? 1 : 3) * dev->mt_info.fingersNum + 1;
      if (dev->mtLastEvent > ZERO_FINGER_EVENT ||
          ((dev->mt_info.mtEventLast < dev->mt_info.mtEvent) &&
           ((dev->mtLastEvent & ~ONE_FINGER_EVENT) & dev->mt_info.mtEvent))) {
        dev->mt_info.end_event = ZERO_FINGER_EVENT;
      }
      if ((dev->mtLastEvent & (RIGHT_MT_EVENT) &&
           dev->mt_info.mtEventLast < dev->mt_info.mtEvent &&
           dev->mt_info.rightIndex != dev->mt_info.mtSlot) ||
           ((dev->mtLastEvent & (LEFT_MT_EVENT | RIGHT_MT_EVENT)) == 0)) {
        dev->mt_info.pre_up_event = dev->mt_info.mtEvent; 
      }
      if (dev->mt_info.pre_down_event) {
        dev->mt_info.down_event |= dev->mt_info.pre_down_event;
        dev->mt_info.pre_down_event = 0;
        dev->mt_info.end_event = ZERO_FINGER_EVENT;
      }
      // for event that not moved,such as click
      if (dev->mt_info.mtEvent > dev->mt_info.end_event) {
        dev->mt_info.end_event = dev->mt_info.mtEvent;
      }
      dev->mtLastEvent |= ((dev->mt_info.down_event | dev->mt_info.end_event) & FINGER_EVENT_MASK);
      // for last event that all finger leave touchpad
      if (dev->mt_info.mtEvent == 0) {
        dev->mt_info.interval = 0;
        dev->mt_info.down_event |= dev->mt_info.end_event;
        dev->mtLastEvent &= (CLEAN_MT_EVENT_MASK | ONE_FINGER_EVENT);
      }
      dev->mt_info.mtEventLast = dev->mt_info.mtEvent;
    }

    if (dev->mt_info.interval > 0) {
      dev->mt_info.interval--;
      if (dev->mt_info.interval < 1) {
        if (dev->mt_info.pre_up_event) {
          dev->mt_info.up_event = dev->mt_info.pre_up_event;
          dev->mt_info.pre_up_event = 0;
        }
      }
    }

    if (dev->mt_info.isMoving) {
      int nowDistanceX = 0;
      int nowDistanceY = 0;
      for (int i = 0; i < dev->mt_info.fingerMaxNum; i++) {
        if (dev->mt_info.slotValueX[i] != 0 && dev->mt_info.slotValueY[i] != 0) {
          int tempX = dev->mt_info.slotValueX[i] - dev->mt_info.lastX[i];
          int tempY = dev->mt_info.slotValueY[i] - dev->mt_info.lastY[i];
          if ((abs(tempX) + abs(tempY)) > (abs(nowDistanceX) + abs(nowDistanceY))) {
            nowDistanceX = tempX;
            nowDistanceY = tempY;
          }
          dev->mt_info.lastX[i] = dev->mt_info.slotValueX[i];
          dev->mt_info.lastY[i] = dev->mt_info.slotValueY[i];
          if (dev->mt_info.distanceX[i] < dev->mtXMax)
            dev->mt_info.distanceX[i] += abs(tempX);
          if (dev->mt_info.distanceY[i] < dev->mtYMax)
            dev->mt_info.distanceY[i] += abs(tempY);
        }
      }
      if ((dev->mt_info.mtEvent & THREE_FINGER_EVENT) ||
          (dev->mtLastEvent & (LEFT_MT_EVENT | RIGHT_MT_EVENT | NO_SCROLL_MT_EVENT))) {
        dev->mouseDeltaX += nowDistanceX;
        dev->mouseDeltaY += nowDistanceY;
      }
      else if (dev->mt_info.mtEvent & TWO_FINGER_EVENT) {
        int absX = abs(nowDistanceX);
        int absY = abs(nowDistanceY);
        float specialBase = 10.0;
        bool needX = (absX > absY);
        int multiZ = needX ? absX : absY;
        int multi = multiZ / specialBase;
        multi = multi < 1 ? 1 : multi;
        float restrain = 0;
        if (absX > (0.8 * absY) && absX < (1.2 * absY))
          restrain = multi;
        // why -nowDistanceX? Because for same scroll direction
        LiSendHighResHScrollEvent((short)(-nowDistanceX * (needX ? multi : restrain)));
        LiSendHighResScrollEvent((short)(nowDistanceY * (needX ? restrain : multi)));
      }
      else if (dev->mt_info.mtEvent & ONE_FINGER_EVENT) {
        dev->mouseDeltaX += nowDistanceX;
        dev->mouseDeltaY += nowDistanceY;
      }
    }

    if (dev->mt_info.needWait) {
      int nowdeltax = 0;
      int nowdeltay = 0;
      for (int i = 0; i < dev->mt_info.fingerMaxNum; i++) {
        if (dev->mt_info.slotValueX[i] != 0 && dev->mt_info.slotValueY[i] != 0) {
          int tempX = dev->mt_info.slotValueX[i] - dev->mt_info.lastX[i];
          int tempY = dev->mt_info.slotValueY[i] - dev->mt_info.lastY[i];
          if ((abs(tempX) + abs(tempY)) > (abs(nowdeltax) + abs(nowdeltay))) {
            nowdeltax = tempX;
            nowdeltay = tempY;
          }
          dev->mt_info.lastX[i] = dev->mt_info.slotValueX[i];
          dev->mt_info.lastY[i] = dev->mt_info.slotValueY[i];
          dev->mt_info.distanceX[i] += abs(tempX);
          dev->mt_info.distanceY[i] += abs(tempY);
          if (dev->mt_info.distanceX[i] >= dev->mtPalm || dev->mt_info.distanceY[i] >= dev->mtPalm) {
            dev->mt_info.isMoving = true;
            dev->mt_info.needWait = false;
          }
        }
      }
    }

    // event release
    // no moving event
    if (dev->mt_info.down_event) {
      struct timeval eventTime;
      eventTime.tv_sec = ev->input_event_sec;
      eventTime.tv_usec = ev->input_event_usec;
      struct timeval elapsedTime;
      timersub(&eventTime, &dev->mt_info.touchDownTime[dev->mt_info.downEventSlot], &elapsedTime);
      int jtime = elapsedTime.tv_sec * 1000000 + elapsedTime.tv_usec;

      // restrict time and distance
      if (dev->mt_info.distanceX[dev->mt_info.downEventSlot] < (dev->mtPalm * 2) &&
          dev->mt_info.distanceY[dev->mt_info.downEventSlot] < (dev->mtPalm * 2) &&
          jtime <= (TOUCH_CLICK_DELAY * 4)) {
        // down.2 three
        switch (dev->mt_info.down_event & FINGER_EVENT_MASK) {
        case THREE_FINGER_EVENT:
          LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_MIDDLE);
          usleep(TOUCH_CLICK_DELAY);
          LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
          break;
        // down.3 two
        case TWO_FINGER_EVENT:
          if (dev->mtLastEvent & (NO_SCROLL_MT_EVENT | RIGHT_MT_EVENT)) {
            break;
          }
          LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
          usleep(TOUCH_CLICK_DELAY);
          LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
          break;
        // down.4 one
        case ONE_FINGER_EVENT:
          if (dev->mtLastEvent & LEFT_MT_EVENT) {
            break;
          }
          if (dev->mt_info.mtEvent != 0)
            dev->mtLastEvent |= NO_SCROLL_MT_EVENT;
          dev->mtLastEvent |= LEFT_MT_EVENT;
          needSpecialEvent = true;
          dev->touchUpTime.tv_sec = ev->input_event_sec;
          dev->touchUpTime.tv_usec = ev->input_event_usec;
          break;
        }
      }
      dev->mt_info.down_event = 0;
    }

    switch (dev->mt_info.up_event) {
      // event cancel
    case THREE_FINGER_EVENT:
      if (dev->mtLastEvent & (LEFT_MT_EVENT)) {
        dev->mt_info.up_event = 0;
        break;
      }
      if (dev->mt_info.isMoving) {
        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
        dev->mtLastEvent |= LEFT_MT_EVENT;
        dev->mt_info.leftIndex = dev->mt_info.upEventSlot;
        if ((dev->mtLastEvent & RIGHT_MT_EVENT) == 0) {
          dev->mt_info.rightIndex = -1;
        }
        dev->mt_info.up_event = 0;
      }
      break;
    case TWO_FINGER_EVENT:
      dev->mt_info.up_event = 0;
      break;
    case ONE_FINGER_EVENT:
      dev->mt_info.up_event = 0;
      break;
    default:
      dev->mt_info.up_event = 0;
      break;
    }

    if (not_handle_mouse_motion_var && fakeGrab) {
      dev->mouseDeltaX = 0;
      dev->mouseDeltaY = 0;
    }
    if (dev->mouseDeltaX != 0 || dev->mouseDeltaY != 0) {
      switch (dev->rotate) {
      case 90:
        LiSendMouseMoveEvent(dev->mouseDeltaY, -dev->mouseDeltaX);
        break;
      case 180:
        LiSendMouseMoveEvent(-dev->mouseDeltaX, -dev->mouseDeltaY);
        break;
      case 270:
        LiSendMouseMoveEvent(-dev->mouseDeltaY, dev->mouseDeltaX);
        break;
      default:
        LiSendMouseMoveEvent(dev->mouseDeltaX, dev->mouseDeltaY);
        break;
      }
      dev->mouseDeltaX = 0;
      dev->mouseDeltaY = 0;
    }

    if (dev->mt_info.mtEventLast == 0) {
      memset(&dev->mt_info, 0, sizeof(dev->mt_info));
    }

    if (needSpecialEvent) {
      needSpecialEvent = false;
      LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
      int res = mt_evdev_handle(dev->fd, dev, 6, NEED_SPECIAL_FINGER_EVENT, dev->mt_info.upEventSlot);
      // res < 0 means exit loop;res = 0 means special event has handled,return function directly
      if (res == 0) {
        return true;
      }
      LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
      if (res == -1) {
        return true;
      }
      dev->mt_info.leftIndex = -1;
      dev->mtLastEvent &= ~LEFT_MT_EVENT;
      if (res < 0) {
        return false;
      }
    }

    break;
  case EV_KEY:
    if (ev->code > KEY_MAX)
      return true;
    switch (ev->code) {
    case BTN_TOUCH:
      // event start or end
      if (ev->value == 1) {
        dev->mt_info.fingerMaxNum = 1;
        dev->touchDownTime.tv_sec = ev->input_event_sec;
        dev->touchDownTime.tv_usec = ev->input_event_usec;
        dev->touchDownX = dev->mt_info.slotValueX[0];
        dev->touchDownY = dev->mt_info.slotValueY[0];
        if (!(dev->mtLastEvent & (LEFT_MT_EVENT))) {
          dev->mt_info.leftIndex = -1;
        }
        if (!(dev->mtLastEvent & (RIGHT_MT_EVENT))) {
          dev->mt_info.rightIndex = -1;
        }
      }
      break;
    case BTN_TOOL_FINGER:
      if (ev->value == 1) {
        dev->mt_info.mtEvent |= ONE_FINGER_EVENT;
      }
      else {
        dev->mt_info.mtEvent &= ~ONE_FINGER_EVENT;
      }
      break;
    case BTN_TOOL_DOUBLETAP:
      if (ev->value == 1) {
        dev->mt_info.mtEvent |= TWO_FINGER_EVENT;
      }
      else {
        dev->mt_info.mtEvent &= ~TWO_FINGER_EVENT;
      }
      break;
    case BTN_TOOL_TRIPLETAP:
      if (ev->value == 1) {
        dev->mt_info.mtEvent |= THREE_FINGER_EVENT;
      }
      else {
        dev->mt_info.mtEvent &= ~THREE_FINGER_EVENT;
      }
      break;
    case BTN_TOOL_QUADTAP:
      if (ev->value == 1) {
        dev->mt_info.mtEvent |= FOUR_FINGER_EVENT;
      }
      else {
        dev->mt_info.mtEvent &= ~FOUR_FINGER_EVENT;
      }
      break;
    case BTN_TOOL_QUINTTAP:
      if (ev->value == 1) {
        dev->mt_info.mtEvent |= FIVE_FINGER_EVENT;
      }
      else {
        dev->mt_info.mtEvent &= ~FIVE_FINGER_EVENT;
      }
      break;
    // mouse action
    case BTN_LEFT:
      // return if value == 0,may be means error occur
      if (dev->mt_info.slotValueX[dev->mt_info.mtSlot] == 0 || dev->mt_info.slotValueY[dev->mt_info.mtSlot] == 0) {
        break;
      }
      // set end_event none
      if (!ev->value && dev->mtLastEvent & LEFT_MT_EVENT) {
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        dev->mtLastEvent &= ~LEFT_MT_EVENT;
      }
      else if (!ev->value && dev->mtLastEvent & RIGHT_MT_EVENT) {
        LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
        dev->mtLastEvent &= ~RIGHT_MT_EVENT;
      }
      else if (!(dev->mtLastEvent & RIGHT_MT_EVENT) && ev->value && dev->mt_info.slotValueX[dev->mt_info.upEventSlot] > (dev->mtXMax * 0.5) && dev->mt_info.slotValueY[dev->mt_info.upEventSlot] > (dev->mtYMax * 0.8)) {
        dev->mt_info.end_event = ZERO_FINGER_EVENT;
        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
        dev->mtLastEvent |= (ev->value ? RIGHT_MT_EVENT : 0);
      }
      else if (!(dev->mtLastEvent & LEFT_MT_EVENT) && ev->value) {
        dev->mt_info.end_event = ZERO_FINGER_EVENT;
        LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
        dev->mtLastEvent |= (ev->value ? LEFT_MT_EVENT : 0);
      }
      break;
    case BTN_RIGHT:
      dev->mt_info.end_event = ZERO_FINGER_EVENT;
      LiSendMouseButtonEvent(ev->value ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
      break;
    case BTN_MIDDLE:
      dev->mt_info.end_event = ZERO_FINGER_EVENT;
      LiSendMouseButtonEvent(ev->value ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
      break;
    }
    break;
  case EV_ABS:
    if (ev->code > ABS_MAX)
      return true;
    switch (ev->code) {
    case ABS_MT_TRACKING_ID:
      if (ev->value == -1) {
        // clean var
        dev->mt_info.downEventSlot = dev->mt_info.mtSlot;
        dev->mt_info.slotValueX[dev->mt_info.mtSlot] = 0;
        dev->mt_info.slotValueY[dev->mt_info.mtSlot] = 0;
        dev->mt_info.fingersNum--;

        // handle click event when gameMode
        if (gameMode && !(dev->mtLastEvent & (SPECIAL_FINGER_EVENT | LEFT_MT_EVENT))) {
          if (dev->mt_info.fingersNum == 1 && dev->mt_info.rightIndex != dev->mt_info.mtSlot) {
            struct timeval eventTime;
            eventTime.tv_sec = ev->input_event_sec;
            eventTime.tv_usec = ev->input_event_usec;
            struct timeval timeval;
            timersub(&eventTime, &dev->touchDownTime, &timeval);
            int ktime = timeval.tv_sec * 1000000 + timeval.tv_usec;
            if (ktime > (TOUCH_CLICK_DELAY * 4)) {
              dev->mt_info.down_event |= ONE_FINGER_EVENT;
            }
          }
        }

        // handle press event cancel
        if ((dev->mtLastEvent & RIGHT_MT_EVENT) && dev->mt_info.rightIndex == dev->mt_info.mtSlot) {
          LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
          dev->mtLastEvent &= ~RIGHT_MT_EVENT;
          dev->mt_info.rightIndex = -1;
        }
        if ((dev->mtLastEvent & NEED_SPECIAL_FINGER_EVENT) || ((dev->mtLastEvent & LEFT_MT_EVENT) && (dev->mt_info.leftIndex == dev->mt_info.mtSlot || dev->mt_info.fingersNum == 2))) {
          LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
          dev->mtLastEvent &= ~LEFT_MT_EVENT;
          dev->mt_info.leftIndex = -1;
        }

        // release keys when all fingers leave
        if (dev->mt_info.fingersNum == 0) {
          if (dev->mtLastEvent & LEFT_MT_EVENT) {
            dev->mtLastEvent &= ~LEFT_MT_EVENT;
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
          }
          if (dev->mtLastEvent & RIGHT_MT_EVENT) {
            dev->mtLastEvent &= ~RIGHT_MT_EVENT;
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
          }
        }

        // release special event
        if (!(dev->mtLastEvent & LEFT_MT_EVENT) && (dev->mtLastEvent & SPECIAL_FINGER_EVENT)) {
          dev->mtLastEvent &= ~SPECIAL_FINGER_EVENT;
          struct timeval eventTime;
          eventTime.tv_sec = ev->input_event_sec;
          eventTime.tv_usec = ev->input_event_usec;
          struct timeval elapsedTime;
          timersub(&eventTime, &dev->mt_info.touchDownTime[dev->mt_info.specialEventSlot], &elapsedTime);
          int jtime = elapsedTime.tv_sec * 1000000 + elapsedTime.tv_usec;
          if (dev->mt_info.distanceX[dev->mt_info.specialEventSlot] < (dev->mtPalm * 2) &&
              dev->mt_info.distanceY[dev->mt_info.specialEventSlot] < (dev->mtPalm * 2) &&
              jtime <= (TOUCH_CLICK_DELAY * 2)) {
            usleep(TOUCH_CLICK_DELAY / 10);
            LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
            usleep(TOUCH_CLICK_DELAY / 10);
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
          }
        }
      }
      else {
        // record touch time
        dev->mt_info.distanceX[dev->mt_info.mtSlot] = 0;
        dev->mt_info.distanceY[dev->mt_info.mtSlot] = 0;
        dev->mt_info.lastX[dev->mt_info.mtSlot] = dev->mt_info.slotValueX[dev->mt_info.mtSlot];
        dev->mt_info.lastY[dev->mt_info.mtSlot] = dev->mt_info.slotValueY[dev->mt_info.mtSlot];
        dev->mt_info.touchDownTime[dev->mt_info.mtSlot].tv_sec = ev->input_event_sec;
        dev->mt_info.touchDownTime[dev->mt_info.mtSlot].tv_usec = ev->input_event_usec;
        // recording touch event every time when event started
        dev->mt_info.upEventSlot = dev->mt_info.mtSlot;
        dev->mt_info.fingersNum++;

        // handle special event when needed and occur
        if (dev->mtLastEvent & NEED_SPECIAL_FINGER_EVENT) {
          dev->mt_info.leftIndex = dev->mt_info.mtSlot;
          dev->mt_info.specialEventSlot = dev->mt_info.mtSlot;
          dev->mtLastEvent |= SPECIAL_FINGER_EVENT;
          if ((dev->mtLastEvent & RIGHT_MT_EVENT) == 0) {
            dev->mt_info.rightIndex = -1;
          }

          // break loop, for not excute gameMode test
          break;
        }
      }

      if (gameMode) {
        if (ev->value != -1) {
          if (dev->mt_info.slotValueX[dev->mt_info.mtSlot] == 0 || dev->mt_info.slotValueY[dev->mt_info.mtSlot] == 0) {
            break;
          }
          // down
          //if (dev->mt_info.slotValueX[dev->mt_info.mtSlot] > (dev->mtXMax * 0.5) && dev->mt_info.slotValueY[dev->mt_info.mtSlot] > (dev->mtYMax * 0.78)) {
          // up
          if (dev->mt_info.slotValueX[dev->mt_info.mtSlot] > (dev->mtXMax * 0.5) && dev->mt_info.slotValueY[dev->mt_info.mtSlot] < (dev->mtYMax * 0.22)) {
            if ((dev->mtLastEvent & RIGHT_MT_EVENT) == 0) {
              dev->mt_info.rightIndex = dev->mt_info.mtSlot;
              LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
              dev->mtLastEvent |= RIGHT_MT_EVENT;
              if ((dev->mtLastEvent & LEFT_MT_EVENT) == 0) {
                dev->mt_info.leftIndex = -1;
              }
            }
          }
          //else if (dev->mt_info.slotValueX[dev->mt_info.mtSlot] <= (dev->mtXMax * 0.5) && dev->mt_info.slotValueY[dev->mt_info.mtSlot] > (dev->mtYMax * 0.78)) {
          else if ((dev->mt_info.slotValueX[dev->mt_info.mtSlot] <= (dev->mtXMax * 0.5) && dev->mt_info.slotValueY[dev->mt_info.mtSlot] < (dev->mtYMax * 0.22))) {
            if ((dev->mtLastEvent & LEFT_MT_EVENT) == 0) {
              dev->mt_info.leftIndex = dev->mt_info.mtSlot;
              LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
              dev->mtLastEvent |= LEFT_MT_EVENT;
              if ((dev->mtLastEvent & RIGHT_MT_EVENT) == 0) {
                dev->mt_info.rightIndex = -1;
              }
            }
          }
        }
      }
      break;
    case ABS_MT_SLOT:
      if (dev->mt_info.fingerMaxNum < (ev->value + 1)) {
        dev->mt_info.fingerMaxNum = (ev->value + 1);
      }
      dev->mt_info.mtSlot = ev->value;
      break;
    case ABS_MT_POSITION_X:
      if (abs(ev->value) > dev->mtXMax)
        break;
      dev->mt_info.slotValueX[dev->mt_info.mtSlot] = ev->value;
      break;
    case ABS_MT_POSITION_Y:
      if (abs(ev->value) > dev->mtYMax)
        break;
      dev->mt_info.slotValueY[dev->mt_info.mtSlot] = ev->value;
      break;
    }
    break;
  }
  return true;
}

static int evdev_check_input() {
  int res = 0;
  struct List_Node *nodePtr = NULL;

  LIST_FOREACH(nodePtr, head_device, node) {
    struct input_device *device = (struct input_device*)nodePtr->data;
    if (device->is_mouse || device->is_keyboard)
      res++;
  }
  if (gpNumForCheck > 0)
    res++;

  return res;
}

static bool evdev_handle_event(struct input_event *ev, struct input_device *dev) {
  bool gamepadModified = false;

  if (dev->mtPalm > 0) {
    return evdev_mt_touchpad_handle_event(ev, dev);
  }

  switch (ev->type) {
  case EV_SYN:
    if (not_handle_mouse_motion_var && fakeGrab) {
      dev->mouseDeltaX = 0;
      dev->mouseDeltaY = 0;
    }
    if (dev->mouseDeltaX != 0 || dev->mouseDeltaY != 0) {
      switch (dev->rotate) {
      case 90:
        LiSendMouseMoveEvent(dev->mouseDeltaY, -dev->mouseDeltaX);
        break;
      case 180:
        LiSendMouseMoveEvent(-dev->mouseDeltaX, -dev->mouseDeltaY);
        break;
      case 270:
        LiSendMouseMoveEvent(-dev->mouseDeltaY, dev->mouseDeltaX);
        break;
      default:
        LiSendMouseMoveEvent(dev->mouseDeltaX, dev->mouseDeltaY);
        break;
      }
      dev->mouseDeltaX = 0;
      dev->mouseDeltaY = 0;
    }
    if (dev->mouseVScroll != 0) {
      LiSendScrollEvent(dev->mouseVScroll);
      dev->mouseVScroll = 0;
    }
    if (dev->mouseHScroll != 0) {
      LiSendHScrollEvent(dev->mouseHScroll);
      dev->mouseHScroll = 0;
    } 
    if (dev->gamepadModified) {
      if (dev->controllerId < 0) {
        for (int i = 0; i < MAX_GAMEPADS; i++) {
          if ((assignedControllerIds & (1 << i)) == 0) {
            assignedControllerIds |= (1 << i);
            dev->controllerId = i;
            printf("Assigned %s as player %d\n", libevdev_get_name(dev->dev), i+1);
            break;
          }
        }
        //Use id 0 when too many gamepads are connected
        if (dev->controllerId < 0)
          dev->controllerId = 0;

        // Send controller arrival event to the host
        send_controller_arrival(dev);
      }
      // Send event only if mouse emulation is disabled.
      if (dev->mouseEmulation == false)
        LiSendMultiControllerEvent(dev->controllerId, assignedControllerIds, dev->buttonFlags, dev->leftTrigger, dev->rightTrigger, dev->leftStickX, dev->leftStickY, dev->rightStickX, dev->rightStickY);
      dev->gamepadModified = false;
    }
    break;
  case EV_KEY:
    if (ev->code > KEY_MAX)
      return true;
    if (ev->code < sizeof(keyCodes)/sizeof(keyCodes[0])) {
      char modifier = 0;
      switch (ev->code) {
      case KEY_LEFTSHIFT:
      case KEY_RIGHTSHIFT:
        modifier = MODIFIER_SHIFT;
        break;
      case KEY_LEFTALT:
      case KEY_RIGHTALT:
        modifier = MODIFIER_ALT;
        break;
      case KEY_LEFTCTRL:
      case KEY_RIGHTCTRL:
        modifier = MODIFIER_CTRL;
        break;
      case KEY_LEFTMETA:
      case KEY_RIGHTMETA:
        modifier = MODIFIER_META;
        break;
      }
      if (modifier != 0) {
        if (ev->value)
          dev->modifiers |= modifier;
        else
          dev->modifiers &= ~modifier;
      }

      // After the quit key combo is pressed, quit once all keys are raised
      if ((dev->modifiers & ACTION_MODIFIERS) == ACTION_MODIFIERS && ev->value != 0) {
        if (ev->code == QUIT_KEY) {
          waitingToExitOnModifiersUp = true;
          return true;
        } else if (ev->code == GRAB_KEY || ev->code == FAKE_GRAB_KEY) {
          if (ev->code == FAKE_GRAB_KEY) {
            fakeGrabKey = true;
          }
          waitingToSwitchGrabOnModifierUp = true;
          return true;
        } else if (ev->code == GAME_MODE_KEY) {
          gameMode = !gameMode;
          return true;
        }
      }
      else if (((dev->modifiers == MODIFIER_ALT) ||
                (dev->modifiers == (MODIFIER_ALT | MODIFIER_CTRL)))
               && ev->value != 0) {
        evwcode passkey;
        switch (ev->code) {
        case KEY_F1:
        case KEY_F2:
        case KEY_F3:
        case KEY_F4:
        case KEY_F5:
        case KEY_F6:
        case KEY_F7:
        case KEY_F8:
        case KEY_F9:
        case KEY_F10:
        case KEY_F11:
        case KEY_F12:
          if (!fakeGrab) break;
          passkey = ev->code - 58;
          if (ev->code == KEY_F11) passkey = 11;
          if (ev->code == KEY_F12) passkey = 12;

          if (dev->modifiers == (MODIFIER_ALT | MODIFIER_CTRL))
            passkey |= FROMDISPLAY;

          write(keyboardpipefd, &passkey, sizeof(passkey));
          break;
        }
      }
      if (waitingToSwitchGrabOnModifierUp) {
        if ((ev->code == GRAB_KEY && ev->value == 0) || 
            (ev->code == FAKE_GRAB_KEY && ev->value == 0)) {
          isGrabKeyRelease = true;
          if (dev->modifiers != 0)
            return true;
        }
        if (dev->modifiers == 0 && isGrabKeyRelease) {
          waitingToSwitchGrabOnModifierUp = false;
          isGrabKeyRelease = false;
          freeallkey();
          if (fakeGrabKey && fakeGrab && isInputing) {
            grab_window(E_GRAB_WINDOW);
          }
          else if (fakeGrabKey && !fakeGrab) {
            fake_grab_window();
          }
          else {
            grab_window(isInputing ? E_UNGRAB_WINDOW : E_GRAB_WINDOW);
          }
          fakeGrabKey = false;
          return true;
        }
        return true;
      } else if (waitingToExitOnModifiersUp && dev->modifiers == 0) {
        freeallkey();
#if defined(HAVE_X11) || defined(HAVE_WAYLAND) || defined(HAVE_DRM)
        if (keyboardpipefd > -1) {
          write(keyboardpipefd, &quitstate, sizeof(quitstate));
        }
#endif
        grab_window(E_UNGRAB_WINDOW);
        return false;
      }

      if (!isInputing)
        break;
      if (ev->value)
        keypress(ev->code);
      else
        keyrelease(ev->code);
      short code = 0x80 << 8 | keyCodes[ev->code];
      LiSendKeyboardEvent(code, ev->value?KEY_ACTION_DOWN:KEY_ACTION_UP, dev->modifiers);

    } else {
      if (!isInputing)
        break;
      int mouseCode = 0;
      int gamepadCode = 0;
      int index = dev->key_map[ev->code];

      switch (ev->code) {
      case BTN_LEFT:
        mouseCode = BUTTON_LEFT;
        break;
      case BTN_MIDDLE:
        mouseCode = BUTTON_MIDDLE;
        break;
      case BTN_RIGHT:
        mouseCode = BUTTON_RIGHT;
        break;
      case BTN_SIDE:
        mouseCode = BUTTON_X1;
        break;
      case BTN_EXTRA:
        mouseCode = BUTTON_X2;
        break;
      case BTN_TOUCH:
        if (!dev->is_touchscreen)
          break;
        if (ev->value == 1) {
          dev->touchDownTime.tv_sec = ev->input_event_sec;
          dev->touchDownTime.tv_usec = ev->input_event_usec;
        } else {
          if (dev->touchDownX != TOUCH_UP && dev->touchDownY != TOUCH_UP) {
            int deltaX = dev->touchX - dev->touchDownX;
            int deltaY = dev->touchY - dev->touchDownY;
            if (deltaX * deltaX + deltaY * deltaY < TOUCH_CLICK_RADIUS * TOUCH_CLICK_RADIUS) {
              struct timeval eventTime;
              eventTime.tv_sec = ev->input_event_sec;
              eventTime.tv_usec = ev->input_event_usec;
              struct timeval elapsedTime;
              timersub(&eventTime, &dev->touchDownTime, &elapsedTime);
              int holdTimeMs = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
              int button = holdTimeMs >= TOUCH_RCLICK_TIME ? BUTTON_RIGHT : BUTTON_LEFT;
              LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, button);
              usleep(TOUCH_CLICK_DELAY);
              LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
            }
          }
          dev->touchDownX = TOUCH_UP;
          dev->touchDownY = TOUCH_UP;
        }
        break;
      default:
        gamepadModified = true;
        if (dev->map == NULL)
          break;
        else if (index == dev->map->btn_a) {
          if (!swapXYAB)
            gamepadCode = A_FLAG;
          else
            gamepadCode = B_FLAG;
        }
        else if (index == dev->map->btn_x) {
          if (!swapXYAB)
            gamepadCode = X_FLAG;
          else
            gamepadCode = Y_FLAG;
        }
        else if (index == dev->map->btn_y) {
          if (!swapXYAB)
            gamepadCode = Y_FLAG;
          else
            gamepadCode = X_FLAG;
        }
        else if (index == dev->map->btn_b) {
          if (!swapXYAB)
            gamepadCode = B_FLAG;
          else
            gamepadCode = A_FLAG;
        }
        else if (index == dev->map->btn_dpup)
          gamepadCode = UP_FLAG;
        else if (index == dev->map->btn_dpdown)
          gamepadCode = DOWN_FLAG;
        else if (index == dev->map->btn_dpright)
          gamepadCode = RIGHT_FLAG;
        else if (index == dev->map->btn_dpleft)
          gamepadCode = LEFT_FLAG;
        else if (index == dev->map->btn_leftstick)
          gamepadCode = LS_CLK_FLAG;
        else if (index == dev->map->btn_rightstick)
          gamepadCode = RS_CLK_FLAG;
        else if (index == dev->map->btn_leftshoulder)
          gamepadCode = LB_FLAG;
        else if (index == dev->map->btn_rightshoulder)
          gamepadCode = RB_FLAG;
        else if (index == dev->map->btn_start)
          gamepadCode = PLAY_FLAG;
        else if (index == dev->map->btn_back)
          gamepadCode = BACK_FLAG;
        else if (index == dev->map->btn_guide)
          gamepadCode = SPECIAL_FLAG;
        else if (index == dev->map->btn_misc1)
          gamepadCode = MISC_FLAG;
        else if (index == dev->map->btn_paddle1)
          gamepadCode = PADDLE1_FLAG;
        else if (index == dev->map->btn_paddle2)
          gamepadCode = PADDLE2_FLAG;
        else if (index == dev->map->btn_paddle3)
          gamepadCode = PADDLE3_FLAG;
        else if (index == dev->map->btn_paddle4)
          gamepadCode = PADDLE4_FLAG;
        else if (index == dev->map->btn_touchpad)
          gamepadCode = TOUCHPAD_FLAG;
      }

      if (mouseCode != 0) {
        LiSendMouseButtonEvent(ev->value?BUTTON_ACTION_PRESS:BUTTON_ACTION_RELEASE, mouseCode);
        gamepadModified = false;
      } else if (gamepadCode != 0) {
        if (ev->value) {
          dev->buttonFlags |= gamepadCode;
          dev->btnDownTime.tv_sec = ev->input_event_sec;
          dev->btnDownTime.tv_usec = ev->input_event_usec;
        } else
          dev->buttonFlags &= ~gamepadCode;

        if (mouseEmulationEnabled && gamepadCode == PLAY_FLAG && ev->value == 0) {
          struct timeval eventTime;
          eventTime.tv_sec = ev->input_event_sec;
          eventTime.tv_usec = ev->input_event_usec;
          struct timeval elapsedTime;
          timersub(&eventTime, &dev->btnDownTime, &elapsedTime);
          int holdTimeMs = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
          if (holdTimeMs >= MOUSE_EMULATION_LONG_PRESS_TIME) {
            if (dev->mouseEmulation) {
              dev->mouseEmulation = false;
              pthread_join(dev->meThread, NULL);
              dev->meThread = 0;
              printf("Mouse emulation disabled for controller %d.\n", dev->controllerId);
            } else {
              dev->mouseEmulation = true;
              pthread_create(&dev->meThread, NULL, HandleMouseEmulation, dev);
              printf("Mouse emulation enabled for controller %d.\n", dev->controllerId);
            }
            // clear gamepad state.
            LiSendMultiControllerEvent(dev->controllerId, assignedControllerIds, 0, 0, 0, 0, 0, 0, 0);
          }
        } else if (dev->mouseEmulation) {
          char action = ev->value ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE;
          switch (gamepadCode) {
            case A_FLAG:
              LiSendMouseButtonEvent(action, BUTTON_LEFT);
              break;
            case B_FLAG:
              LiSendMouseButtonEvent(action, BUTTON_RIGHT);
              break;
            case X_FLAG:
              LiSendMouseButtonEvent(action, BUTTON_MIDDLE);
              break;
            case LB_FLAG:
              LiSendMouseButtonEvent(action, BUTTON_X1);
              break;
            case RB_FLAG:
              LiSendMouseButtonEvent(action, BUTTON_X2);
              break;
          }
        }
      } else if (dev->map != NULL && index == dev->map->btn_lefttrigger)
        dev->leftTrigger = ev->value ? UCHAR_MAX : 0;
      else if (dev->map != NULL && index == dev->map->btn_righttrigger)
        dev->rightTrigger = ev->value ? UCHAR_MAX : 0;
      else {
        if (dev->map != NULL)
          fprintf(stderr, "Unmapped button: %d\n", ev->code);

        gamepadModified = false;
      }
    }
    break;
  case EV_REL:
    switch (ev->code) {
      case REL_X:
        dev->mouseDeltaX = ev->value;
        break;
      case REL_Y:
        dev->mouseDeltaY = ev->value;
        break;
      case REL_HWHEEL:
        dev->mouseHScroll = ev->value;
        break;
      case REL_WHEEL:
        dev->mouseVScroll = ev->value;
        break;
    }
    break;
  case EV_ABS:
    if (ev->code > ABS_MAX)
      return true;
    if (dev->is_touchscreen) {
      switch (ev->code) {
      case ABS_X:
        if (dev->touchDownX == TOUCH_UP) {
          dev->touchDownX = ev->value;
          dev->touchX = ev->value;
        } else {
          dev->mouseDeltaX += (ev->value - dev->touchX);
          dev->touchX = ev->value;
        }
        break;
      case ABS_Y:
        if (dev->touchDownY == TOUCH_UP) {
          dev->touchDownY = ev->value;
          dev->touchY = ev->value;
        } else {
          dev->mouseDeltaY += (ev->value - dev->touchY);
          dev->touchY = ev->value;
        }
        break;
      }
      break;
    }

    if (dev->map == NULL)
      break;

    gamepadModified = true;
    int index = dev->abs_map[ev->code];
    int hat_index = (ev->code - ABS_HAT0X) / 2;
    int hat_dir_index = (ev->code - ABS_HAT0X) % 2;

    switch (ev->code) {
    case ABS_HAT0X:
    case ABS_HAT0Y:
    case ABS_HAT1X:
    case ABS_HAT1Y:
    case ABS_HAT2X:
    case ABS_HAT2Y:
    case ABS_HAT3X:
    case ABS_HAT3Y:
      dev->hats_state[hat_index][hat_dir_index] = ev->value < 0 ? -1 : (ev->value == 0 ? 0 : 1);
      int hat_state = hat_constants[dev->hats_state[hat_index][1] + 1][dev->hats_state[hat_index][0] + 1];
      if (hat_index == dev->map->hat_dpup)
        set_hat(dev->buttonFlags, UP_FLAG, hat_state, dev->map->hat_dir_dpup);
      if (hat_index == dev->map->hat_dpdown)
        set_hat(dev->buttonFlags, DOWN_FLAG, hat_state, dev->map->hat_dir_dpdown);
      if (hat_index == dev->map->hat_dpright)
        set_hat(dev->buttonFlags, RIGHT_FLAG, hat_state, dev->map->hat_dir_dpright);
      if (hat_index == dev->map->hat_dpleft)
        set_hat(dev->buttonFlags, LEFT_FLAG, hat_state, dev->map->hat_dir_dpleft);
      break;
    default:
      if (index == dev->map->abs_leftx)
        dev->leftStickX = evdev_convert_value(ev, dev, &dev->xParms, dev->map->reverse_leftx);
      else if (index == dev->map->abs_lefty)
        dev->leftStickY = evdev_convert_value(ev, dev, &dev->yParms, !dev->map->reverse_lefty);
      else if (index == dev->map->abs_rightx)
        dev->rightStickX = evdev_convert_value(ev, dev, &dev->rxParms, dev->map->reverse_rightx);
      else if (index == dev->map->abs_righty)
        dev->rightStickY = evdev_convert_value(ev, dev, &dev->ryParms, !dev->map->reverse_righty);
      else
        gamepadModified = false;

      if (index == dev->map->abs_lefttrigger) {
        dev->leftTrigger = evdev_convert_value_byte(ev, dev, &dev->zParms, dev->map->halfaxis_lefttrigger);
        gamepadModified = true;
      }
      if (index == dev->map->abs_righttrigger) {
        dev->rightTrigger = evdev_convert_value_byte(ev, dev, &dev->rzParms, dev->map->halfaxis_righttrigger);
        gamepadModified = true;
      }

      if (index == dev->map->abs_dpright) {
        if (evdev_convert_value_byte(ev, dev, &dev->rightParms, dev->map->halfaxis_dpright) > 127)
          dev->buttonFlags |= RIGHT_FLAG;
        else
          dev->buttonFlags &= ~RIGHT_FLAG;

        gamepadModified = true;
      }
      if (index == dev->map->abs_dpleft) {
        if (evdev_convert_value_byte(ev, dev, &dev->leftParms, dev->map->halfaxis_dpleft) > 127)
          dev->buttonFlags |= LEFT_FLAG;
        else
          dev->buttonFlags &= ~LEFT_FLAG;

        gamepadModified = true;
      }
      if (index == dev->map->abs_dpup) {
        if (evdev_convert_value_byte(ev, dev, &dev->upParms, dev->map->halfaxis_dpup) > 127)
          dev->buttonFlags |= UP_FLAG;
        else
          dev->buttonFlags &= ~UP_FLAG;

        gamepadModified = true;
      }
      if (index == dev->map->abs_dpdown) {
        if (evdev_convert_value_byte(ev, dev, &dev->downParms, dev->map->halfaxis_dpdown) > 127)
          dev->buttonFlags |= DOWN_FLAG;
        else
          dev->buttonFlags &= ~DOWN_FLAG;

        gamepadModified = true;
      }
    }
  }

  if (gamepadModified && (dev->buttonFlags & QUIT_BUTTONS) == QUIT_BUTTONS) {
#if defined(HAVE_X11) || defined(HAVE_WAYLAND) || defined(HAVE_DRM)
    if (keyboardpipefd > -1) {
      write(keyboardpipefd, &quitstate, sizeof(quitstate));
    }
#endif
    LiSendMultiControllerEvent(dev->controllerId, assignedControllerIds, 0, 0, 0, 0, 0, 0, 0);
    return false;
  }

  dev->gamepadModified |= gamepadModified;
  return true;
}

static bool evdev_handle_mapping_event(struct input_event *ev, struct input_device *dev) {
  int index, hat_index;
  switch (ev->type) {
  case EV_KEY:
    index = dev->key_map[ev->code];
    if (currentKey != NULL) {
      if (ev->value)
        *currentKey = index;
      else if (*currentKey != -1 && index == *currentKey)
        return false;
    }
    break;
  case EV_ABS:
    hat_index = (ev->code - ABS_HAT0X) / 2;
    if (hat_index >= 0 && hat_index < 4) {
      dev->hats_state[hat_index][0] = 0;
      dev->hats_state[hat_index][1] = 0;
      int hat_dir_index = (ev->code - ABS_HAT0X) % 2;
      dev->hats_state[hat_index][hat_dir_index] = ev->value < 0 ? -1 : (ev->value == 0 ? 0 : 1);
    }
    if (currentAbs != NULL) {
      struct input_abs_parms parms;
      evdev_init_parms(dev, &parms, dev->abs_map[ev->code]);

      if (ev->value > parms.avg + parms.range/2) {
        *currentAbs = dev->abs_map[ev->code];
        *currentReverse = false;
      } else if (ev->value < parms.avg - parms.range/2) {
        *currentAbs = dev->abs_map[ev->code];
        *currentReverse = true;
      } else if (dev->abs_map[ev->code] == *currentAbs) {
        return false;
      }
    } else if (currentHat != NULL) {
      if (hat_index >= 0 && hat_index < 4) {
        *currentHat = hat_index;
        *currentHatDir = hat_constants[dev->hats_state[hat_index][1] + 1][dev->hats_state[hat_index][0] + 1];
        return false;
      }
    }
    break;
  }
  return true;
}

static void evdev_drain(void) {
  // opt 0 is not remove deivce, -1 is remove all device
  struct List_Node *nodePtr = NULL;
  LIST_FOREACH(nodePtr, head_device, node) {
    struct input_event ev;
    while (libevdev_next_event(((struct input_device *)nodePtr->data)->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) >= 0);
  }
}

static int mt_evdev_handle(int fd, void *data, int interval, uint32_t event, int slot) {
#define EXIT_RES -2
#define REMOVE_RES -1
#define SUCCESS_RES 0
#define FAILED_RES 1
  static int failed_times = 0;
  int res;

  struct input_device *device = (struct input_device *)data;
  int rc;
  int times = 0;
  device->mtLastEvent |= event;
  while (times < interval) {
    struct input_event ev = {0};
    rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    switch (rc) {
    case LIBEVDEV_READ_STATUS_SUCCESS:
      if (!handler(&ev, device)) {
        res = EXIT_RES;
        goto direct_exit;
      }
      switch (ev.type) {
      case EV_SYN:
        times++;
        break;
      case EV_ABS:
        if (ev.code == ABS_MT_TRACKING_ID) {
          res = SUCCESS_RES;
          goto exit;
        }
        break;
      default:
        break;
      }
      break;
    case -EAGAIN:
      if (failed_times > 12) {
        res = FAILED_RES;
        goto exit;
      }
      failed_times++;
      usleep(TOUCH_CLICK_DELAY / 10);
      return mt_evdev_handle(fd, data, 6, event, slot);
    case LIBEVDEV_READ_STATUS_SYNC:
      fprintf(stderr, "Error:%s cannot keep up\n", libevdev_get_name(device->dev));
      res = FAILED_RES;
      goto exit;
    case -ENODEV:
      evdev_remove(device);
      // return success for not excute continue code
      res = REMOVE_RES;
      goto direct_exit;
    default:
      if (rc < 0) {
        fprintf(stderr, "Error occur from evdev device: %s, remove %s\n", strerror(-rc), device->path);
        evdev_remove(device);
        // return success for not excute continue code
        res = REMOVE_RES;
        if (evdev_check_input() <= 0) {
          fprintf(stderr, "There is no avaliable input device! Exit now.\n");
          res = EXIT_RES;
        }
        goto direct_exit;
      }
      break;
    }
  }

  res = FAILED_RES;

exit:
  device->mtLastEvent &= ~event;
  failed_times = 0;

direct_exit:
  return res;
#undef EXIT_RES
#undef REMOVE_RES
#undef SUCCESS_RES
#undef FAILED_RES
}

static int evdev_handle(int fd, void *data) {
  struct input_device *device = (struct input_device *)data;
  int rc;
  struct input_event ev;
  while ((rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) >= 0) {
    if (rc == LIBEVDEV_READ_STATUS_SYNC)
      fprintf(stderr, "Error:%s cannot keep up\n", libevdev_get_name(device->dev));
    else if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
      if (!isInputing && ev.type != EV_KEY)
        break;
      if (!handler(&ev, device)) {
        return LOOP_RETURN;
      }
    }
  }
  if (rc == -ENODEV) {
    evdev_remove(device);
  } else if (rc != -EAGAIN && rc < 0) {
    fprintf(stderr, "Error occur from evdev device: %s, remove %s\n", strerror(-rc), device->path);
    evdev_remove(device);
    if (evdev_check_input() <= 0) {
      fprintf(stderr, "There is no avaliable input device! Exit now.\n");
      return LOOP_RETURN;
    }
  }
  return LOOP_OK;
}

void evdev_init_vars(bool isfakegrab, bool issdlgp, bool isswapxyab, bool isinputadded) {
  fakeGrab = isfakegrab;
  sdlgp = issdlgp;
  swapXYAB = isswapxyab;
  if (swapXYAB)

  if (isinputadded)
    return;

  const char* tryFirstInput = "/dev/input/event0";
  const char* trySecondInput = "/dev/input/event1";

  int fdFirst = open(tryFirstInput, O_RDWR|O_NONBLOCK);
  int fdSecond = open(trySecondInput, O_RDWR|O_NONBLOCK);
  if (fdFirst <= 0 && fdSecond <= 0) {
    //Suppose use kbdmux because of default behavior
    isUseKbdmux = true;
    return;
  }

  struct libevdev *evdevFirst = libevdev_new();
  libevdev_set_fd(evdevFirst, fdFirst);
  const char* nameFirst = libevdev_get_name(evdevFirst);
  struct libevdev *evdevSecond = libevdev_new();
  libevdev_set_fd(evdevSecond, fdSecond);
  const char* nameSecond = libevdev_get_name(evdevSecond);

  libevdev_free(evdevFirst);
  libevdev_free(evdevSecond);
  close(fdFirst);
  close(fdSecond);

  if (strcmp(nameFirst, "System keyboard multiplexer") == 0 ||
      strcmp(nameSecond, "System keyboard multiplexer") == 0) {
    isUseKbdmux = true;
    return;
  }

  return;
}

void evdev_create(const char* device, struct mapping* mappings, bool verbose, int rotate) {
  int fd = open(device, O_RDWR|O_NONBLOCK);
  if (fd <= 0) {
    fprintf(stderr, "Failed to open device %s\n", device);
    fflush(stderr);
    return;
  }

  struct libevdev *evdev = libevdev_new();
  libevdev_set_fd(evdev, fd);
  const char* name = libevdev_get_name(evdev);

  int16_t guid[8] = {0};
  guid[0] = int16_to_le(libevdev_get_id_bustype(evdev));
  int16_t vendor = libevdev_get_id_vendor(evdev);
  int16_t product = libevdev_get_id_product(evdev);
  if (vendor && product) {
    guid[2] = int16_to_le(vendor);
    guid[4] = int16_to_le(product);
    guid[6] = int16_to_le(libevdev_get_id_version(evdev));
  } else
    strncpy((char*) &guid[2], name, 11);

  char str_guid[33];
  char* buf = str_guid;
  for (int i = 0; i < 16; i++)
    buf += sprintf(buf, "%02x", ((unsigned char*) guid)[i]);

  struct mapping* default_mapping = NULL;
  struct mapping* xwc_mapping = NULL;
  while (mappings != NULL) {
    if (strncmp(str_guid, mappings->guid, 32) == 0) {
      if (verbose)
        printf("Detected %s (%s) on %s as %s\n", name, str_guid, device, mappings->name);

      break;
    } else if (strncmp("default", mappings->guid, 32) == 0)
      default_mapping = mappings;
    else if (strncmp("xwc", mappings->guid, 32) == 0)
      xwc_mapping = mappings;

    mappings = mappings->next;
  }

  if (mappings == NULL && strstr(name, "Xbox 360 Wireless Receiver") != NULL)
    mappings = xwc_mapping;

  bool is_keyboard = libevdev_has_event_code(evdev, EV_KEY, KEY_Q);
  bool is_mouse = libevdev_has_event_type(evdev, EV_REL) || 
                  libevdev_has_event_code(evdev, EV_KEY, BTN_LEFT);
  bool is_touchscreen = !is_mouse && libevdev_has_event_code(evdev, EV_KEY, BTN_TOUCH);

  // This classification logic comes from SDL
  bool is_accelerometer =
    ((libevdev_has_event_code(evdev, EV_ABS, ABS_X) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_Y) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_Z)) ||
     (libevdev_has_event_code(evdev, EV_ABS, ABS_RX) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_RY) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_RZ))) &&
    !libevdev_has_event_type(evdev, EV_KEY);
  bool is_gamepad =
    ((libevdev_has_event_code(evdev, EV_ABS, ABS_X) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_Y)) ||
     (libevdev_has_event_code(evdev, EV_ABS, ABS_HAT0X) &&
      libevdev_has_event_code(evdev, EV_ABS, ABS_HAT0Y))) &&
    (libevdev_has_event_code(evdev, EV_KEY, BTN_TRIGGER) ||
     libevdev_has_event_code(evdev, EV_KEY, BTN_A) ||
     libevdev_has_event_code(evdev, EV_KEY, BTN_1) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_RX) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_RY) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_RZ) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_THROTTLE) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_RUDDER) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_WHEEL) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_GAS) ||
     libevdev_has_event_code(evdev, EV_ABS, ABS_BRAKE));
  bool is_acpibutton =
    strcmp(name, "Sleep Button") == 0 ||
    strcmp(name, "Power Button") == 0;
  // Just use System keyboard multiplexer for FreeBSD,see kbdcontrol(1) and kbdmux(4)
  // Trying to grab kbdmux0 and keyboard it's self at the same time results in
  // the keyboard becoming unresponsive on FreeBSD.
  bool is_likekeyboard =
    is_keyboard && isUseKbdmux && strcmp(name, "System keyboard multiplexer") != 0;
/*
    (is_keyboard && guid[0] <= 3) ||
    strcmp(name, "AT keyboard") == 0;
*/

  // In some cases,acpibutton can be mistaken for a keyboard and freeze the keyboard when tring grab.
  if (is_acpibutton) {
    if (verbose)
      printf("Skip acpibutton: %s\n", name);
    libevdev_free(evdev);
    close(fd);
    return;
  }
  // In some cases,Do not grab likekeyboard for avoiding keyboard unresponsive
  if (is_likekeyboard) {
    if (verbose)
      printf("Do NOT grab like-keyboard: %s,version: %d,bustype: %d\n", name, guid[6], guid[0]);
    is_keyboard = false;
  }

  if (is_accelerometer) {
    if (verbose)
      printf("Ignoring accelerometer: %s\n", name);
    libevdev_free(evdev);
    close(fd);
    return;
  }

  if (is_gamepad) {

    if (sdlgp) {
      if (verbose)
        printf("Ignoring gamepad by evdev,instead by using sdl: %s\n", name);
      libevdev_free(evdev);
      close(fd);
      return;
    }

    if (mappings == NULL) {
      fprintf(stderr, "No mapping available for %s (%s) on %s\n", name, str_guid, device);
      fprintf(stderr, "Please use 'moonlight map -input %s >> ~/.config/moonlight/gamecontrollerdb.txt' for %s to create mapping\n", device, name);
      mappings = default_mapping;
    }
  } else {
    if (verbose)
      printf("Not mapping %s as a gamepad\n", name);
    mappings = NULL;
  }

  struct input_device *dev = malloc(sizeof(struct input_device));
  struct List_Node *nodePtr = malloc(sizeof(struct List_Node));

  if (dev == NULL || nodePtr == NULL) {
    if (dev)
      free(dev);
    if (nodePtr)
      free(nodePtr);
    fprintf(stderr, "Not enough memory\n");
    exit(EXIT_FAILURE);
  }

  if (numDevices == 0) {
    verboseMe = verbose;
    // generate device list
    LIST_INIT(head_device);
  }

  numDevices++;

  memset(nodePtr, 0, sizeof(struct List_Node));
  nodePtr->data = (void *) dev;
  LIST_INSERT_HEAD(head_device, nodePtr, node);

  memset(dev, 0, sizeof(struct input_device));
  dev->fd = fd;
  dev->path = malloc(1 + strlen(device) * sizeof(char));
  memcpy(dev->path, device, 1 + strlen(device) * sizeof(char));
  dev->dev = evdev;
  dev->map = mappings;
  /* Set unused evdev indices to -2 to avoid aliasing with the default -1 in our mappings */
  memset(&dev->key_map, -2, sizeof(dev->key_map));
  memset(&dev->abs_map, -2, sizeof(dev->abs_map));
  dev->is_keyboard = is_keyboard;
  dev->is_mouse = is_mouse;
  dev->is_touchscreen = is_touchscreen;
  dev->rotate = rotate;
  dev->touchDownX = TOUCH_UP;
  dev->touchDownY = TOUCH_UP;
  if (!is_gamepad && is_mouse &&
      (libevdev_has_event_code(evdev, EV_ABS, ABS_MT_SLOT) && 
       libevdev_has_event_code(evdev, EV_KEY, BTN_TOOL_DOUBLETAP) && 
       libevdev_has_event_code(evdev, EV_KEY, BTN_TOUCH) && 
       libevdev_has_event_code(evdev, EV_ABS, ABS_X))) {
    const struct input_absinfo *info = libevdev_get_abs_info(evdev, ABS_MT_SLOT);
    if (info && info->maximum < 10 && info->maximum > 0) {
      dev->mtPalm = (int)libevdev_get_abs_resolution(evdev, ABS_X) * 0.8;
      dev->mtXMax = libevdev_get_abs_maximum(evdev, ABS_X);
      dev->mtYMax = libevdev_get_abs_maximum(evdev, ABS_Y);
      if (dev->mtPalm <= 0 || dev->mtXMax <= 0 || dev->mtYMax <= 0) {
        dev->mtPalm = 0;
        dev->mtXMax = 0;
        dev->mtYMax = 0;
      }
    }
  }


  int nbuttons = 0;
  /* Count joystick buttons first like SDL does */
  for (int i = BTN_JOYSTICK; i < KEY_MAX; ++i) {
    if (libevdev_has_event_code(dev->dev, EV_KEY, i))
      dev->key_map[i] = nbuttons++;
  }
  for (int i = 0; i < BTN_JOYSTICK; ++i) {
    if (libevdev_has_event_code(dev->dev, EV_KEY, i))
      dev->key_map[i] = nbuttons++;
  }

  int naxes = 0;
  for (int i = 0; i < ABS_MAX; ++i) {
    /* Skip hats */
    if (i == ABS_HAT0X)
      i = ABS_HAT3Y;
    else if (libevdev_has_event_code(dev->dev, EV_ABS, i)) {
      dev->abs_map[i] = naxes++;
    }
  }

  dev->controllerId = -1;
  dev->haptic_effect_id = -1;

  if (dev->map != NULL) {
    bool valid = evdev_init_parms(dev, &(dev->xParms), dev->map->abs_leftx);
    valid &= evdev_init_parms(dev, &(dev->yParms), dev->map->abs_lefty);
    valid &= evdev_init_parms(dev, &(dev->zParms), dev->map->abs_lefttrigger);
    valid &= evdev_init_parms(dev, &(dev->rxParms), dev->map->abs_rightx);
    valid &= evdev_init_parms(dev, &(dev->ryParms), dev->map->abs_righty);
    valid &= evdev_init_parms(dev, &(dev->rzParms), dev->map->abs_righttrigger);
    valid &= evdev_init_parms(dev, &(dev->leftParms), dev->map->abs_dpleft);
    valid &= evdev_init_parms(dev, &(dev->rightParms), dev->map->abs_dpright);
    valid &= evdev_init_parms(dev, &(dev->upParms), dev->map->abs_dpup);
    valid &= evdev_init_parms(dev, &(dev->downParms), dev->map->abs_dpdown);
    if (!valid)
      fprintf(stderr, "Mapping for %s (%s) on %s is incorrect\n", name, str_guid, device);
  }

  if (grabbingDevices && !fakeGrab && (is_keyboard || is_mouse || is_touchscreen)) {
    if (libevdev_grab(dev->dev, LIBEVDEV_GRAB) < 0) {
      fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);
    }
  }

  loop_add_fd1(dev->fd, &evdev_handle, EPOLLIN, (void *)(dev));
}

static void evdev_map_key(char* keyName, short* key) {
  printf("Press %s\n", keyName);
  currentKey = key;
  currentHat = NULL;
  currentAbs = NULL;
  *key = -1;
  loop_main();

  usleep(250000);
  evdev_drain();
}

static void evdev_map_abs(char* keyName, short* abs, bool* reverse) {
  printf("Move %s\n", keyName);
  currentKey = NULL;
  currentHat = NULL;
  currentAbs = abs;
  currentReverse = reverse;
  *abs = -1;
  loop_main();

  usleep(250000);
  evdev_drain();
}

static void evdev_map_hatkey(char* keyName, short* hat, short* hat_dir, short* key) {
  printf("Press %s\n", keyName);
  currentKey = key;
  currentHat = hat;
  currentHatDir = hat_dir;
  currentAbs = NULL;
  *key = -1;
  *hat = -1;
  *hat_dir = -1;
  *currentReverse = false;
  loop_main();

  usleep(250000);
  evdev_drain();
}

static void evdev_map_abskey(char* keyName, short* abs, short* key, bool* reverse) {
  printf("Press %s\n", keyName);
  currentKey = key;
  currentHat = NULL;
  currentAbs = abs;
  currentReverse = reverse;
  *key = -1;
  *abs = -1;
  *currentReverse = false;
  loop_main();

  usleep(250000);
  evdev_drain();
}

void evdev_map(char* device) {
  int fd = open(device, O_RDONLY|O_NONBLOCK);
  struct libevdev *evdev = libevdev_new();
  libevdev_set_fd(evdev, fd);
  const char* name = libevdev_get_name(evdev);
  isInputing = true;

  int16_t guid[8] = {0};
  guid[0] = int16_to_le(libevdev_get_id_bustype(evdev));
  guid[2] = int16_to_le(libevdev_get_id_vendor(evdev));
  guid[4] = int16_to_le(libevdev_get_id_product(evdev));
  guid[6] = int16_to_le(libevdev_get_id_version(evdev));
  char str_guid[33];
  char* buf = str_guid;
  for (int i = 0; i < 16; i++)
    buf += sprintf(buf, "%02x", ((unsigned char*) guid)[i]);

  struct mapping map = {0};
  strncpy(map.name, name, sizeof(map.name) - 1);
  strncpy(map.guid, str_guid, sizeof(map.guid) - 1);

  libevdev_free(evdev);
  close(fd);

  if (ioctl(((struct input_device *)(LIST_FIRST(head_device)->data))->fd, EVIOCGRAB, 1) < 0)
    fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);

  handler = evdev_handle_mapping_event;

  evdev_map_abs("Left Stick Right", &(map.abs_leftx), &(map.reverse_leftx));
  evdev_map_abs("Left Stick Up", &(map.abs_lefty), &(map.reverse_lefty));
  evdev_map_key("Left Stick Button", &(map.btn_leftstick));

  evdev_map_abs("Right Stick Right", &(map.abs_rightx), &(map.reverse_rightx));
  evdev_map_abs("Right Stick Up", &(map.abs_righty), &(map.reverse_righty));
  evdev_map_key("Right Stick Button", &(map.btn_rightstick));

  evdev_map_hatkey("D-Pad(Hat) Right", &(map.hat_dpright), &(map.hat_dir_dpright), &(map.btn_dpright));
  evdev_map_hatkey("D-Pad(Hat) Left", &(map.hat_dpleft), &(map.hat_dir_dpleft), &(map.btn_dpleft));
  evdev_map_hatkey("D-Pad(Hat) Up", &(map.hat_dpup), &(map.hat_dir_dpup), &(map.btn_dpup));
  evdev_map_hatkey("D-Pad(Hat) Down", &(map.hat_dpdown), &(map.hat_dir_dpdown), &(map.btn_dpdown));

  evdev_map_key("Button X (1)", &(map.btn_x));
  evdev_map_key("Button A (2)", &(map.btn_a));
  evdev_map_key("Button B (3)", &(map.btn_b));
  evdev_map_key("Button Y (4)", &(map.btn_y));
  evdev_map_key("Back(Select) Button", &(map.btn_back));
  evdev_map_key("Start Button", &(map.btn_start));
  evdev_map_key("Special(Home) Button", &(map.btn_guide));

  evdev_map_key("Left Bumper(Shoulder)", &(map.btn_leftshoulder));
  evdev_map_key("Right Bumper(Shoulder)", &(map.btn_rightshoulder));

  bool ignored;
  evdev_map_abskey("Left Trigger", &(map.abs_lefttrigger), &(map.btn_lefttrigger), &ignored);
  evdev_map_abskey("Right Trigger", &(map.abs_righttrigger), &(map.btn_righttrigger), &ignored);

  if (ioctl(((struct input_device *)(LIST_FIRST(head_device)->data))->fd, EVIOCGRAB, 0) < 0)
    fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);

  loop_destroy();
  mapping_print(&map);
}

void evdev_start() {
  // After grabbing, the only way to quit via the keyboard
  // is via the special key combo that the input handling
  // code looks for. For this reason, we wait to grab until
  // we're ready to take input events. Ctrl+C works up until
  // this point.
  if (!fakeGrab)
    grab_window(E_GRAB_WINDOW);

  // Handle input events until the quit combo is pressed
}

void evdev_stop() {
  grab_window(E_UNGRAB_WINDOW);
  evdev_remove_all();
}

void evdev_init(bool mouse_emulation_enabled) {
  handler = evdev_handle_event;
  mouseEmulationEnabled = mouse_emulation_enabled;
}

static struct input_device* evdev_get_input_device(unsigned short controller_id) {
  struct List_Node *nodePtr = NULL;
  LIST_FOREACH(nodePtr, head_device, node) {
    if(((struct input_device*)nodePtr->data)->controllerId == controller_id) {
      return nodePtr->data;
    }
  }

  return NULL;
}

void evdev_rumble(unsigned short controller_id, unsigned short low_freq_motor, unsigned short high_freq_motor) {
  struct input_device* device = evdev_get_input_device(controller_id);
  if (!device)
    return;

  if (device->haptic_effect_id >= 0) {
    ioctl(device->fd, EVIOCRMFF, device->haptic_effect_id);
    device->haptic_effect_id = -1;
  }

  if (low_freq_motor == 0 && high_freq_motor == 0)
    return;

  struct ff_effect effect = {0};
  effect.type = FF_RUMBLE;
  effect.id = -1;
  effect.replay.length = USHRT_MAX;
  effect.u.rumble.strong_magnitude = low_freq_motor;
  effect.u.rumble.weak_magnitude = high_freq_motor;
  if (ioctl(device->fd, EVIOCSFF, &effect) == -1)
    return;

  struct input_event event = {0};
  event.type = EV_FF;
  event.code = effect.id;
  event.value = 1;
  write(device->fd, (const void*) &event, sizeof(event));
  device->haptic_effect_id = effect.id;
}

void sync_input_state(bool isinputing) {
  if (!fakeGrab)
    return;
  freeallkey();
  evdev_drain();

  if (not_handle_mouse_motion)
    not_handle_mouse_motion_var = true;

  if (isGrabed == E_GRAB_WINDOW) {
    struct List_Node *nodePtr = NULL;
    LIST_FOREACH(nodePtr, head_device, node) {
      struct input_device *device = (struct input_device *)nodePtr->data;
      if (device->is_keyboard || device->is_mouse || device->is_touchscreen) {
        if (libevdev_grab(device->dev, LIBEVDEV_UNGRAB) < 0)
          fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);
      }
    }
  }
  grabbingDevices = false;
  isGrabed = E_UNGRAB_WINDOW;

  isInputing = isinputing;
}

void grab_window(enum grabWindowRequest grabstat) {
  enum libevdev_grab_mode grabmode;

  evdev_drain();

  if (grabstat != E_GRAB_WINDOW) {
    grabmode = LIBEVDEV_UNGRAB;
    grabbingDevices = false;
  } else {
    // Any new input devices detected after this point will be grabbed immediately
    grabbingDevices = true;
    grabmode = LIBEVDEV_GRAB;
  }

  if (grabstat != isGrabed) {
    struct List_Node *nodePtr = NULL;
    LIST_FOREACH(nodePtr, head_device, node) {
      struct input_device *device = (struct input_device *)nodePtr->data;
      if (device->is_keyboard || device->is_mouse || device->is_touchscreen) {
        if (libevdev_grab(device->dev, grabmode) < 0)
          fprintf(stderr, "EVIOCGRAB failed with error %d\n", errno);
      }
    }
  }

  bool lastInputStat = isInputing;
  fakeGrab = false;
  not_handle_mouse_motion_var = false;
  isInputing = grabstat == E_GRAB_WINDOW ? true : false;
  isGrabed =  grabstat == E_GRAB_WINDOW ? E_GRAB_WINDOW : E_UNGRAB_WINDOW;

  if (lastInputStat != isInputing) {
#if defined(HAVE_X11) || defined(HAVE_WAYLAND) || defined(HAVE_DRM)
    if (keyboardpipefd > -1) {
      write(keyboardpipefd, grabstat != E_GRAB_WINDOW ? &ungrabcode : &grabcode, sizeof(grabcode));
    }
#endif
  }
}

void evdev_trans_op_fd(int pipefd) {
  keyboardpipefd = pipefd;
}

void evdev_switch_mouse_mode(bool handled_by_window) {
  not_handle_mouse_motion_var = handled_by_window;
}

void evdev_pass_mouse_mode(bool handled_by_window) {
  not_handle_mouse_motion = handled_by_window;
  evdev_switch_mouse_mode(handled_by_window);
}

#ifdef HAVE_SDL

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_gamepad.h>

#define SDL_NOTHING 0
#define SDL_QUIT_APPLICATION 1
#define QUIT_BUTTONS (PLAY_FLAG|BACK_FLAG|LB_FLAG|RB_FLAG)

extern int sdl_gamepads;
int sdl_gamepads = 0;

static SDL_Thread *thread = NULL;
static bool x11_sdl_start = false;

static const int SDL_TO_LI_BUTTON_MAP[] = {
  A_FLAG, B_FLAG, X_FLAG, Y_FLAG,
  BACK_FLAG, SPECIAL_FLAG, PLAY_FLAG,
  LS_CLK_FLAG, RS_CLK_FLAG,
  LB_FLAG, RB_FLAG,
  UP_FLAG, DOWN_FLAG, LEFT_FLAG, RIGHT_FLAG,
  MISC_FLAG,
  PADDLE1_FLAG, PADDLE2_FLAG, PADDLE3_FLAG, PADDLE4_FLAG,
  TOUCHPAD_FLAG,
};

typedef struct _GAMEPAD_STATE {
  unsigned char leftTrigger, rightTrigger;
  short leftStickX, leftStickY;
  short rightStickX, rightStickY;
  int buttons;
  SDL_JoystickID sdl_id;
  SDL_Gamepad* controller;
  short id;
  bool initialized;
} GAMEPAD_STATE, *PGAMEPAD_STATE;

static GAMEPAD_STATE gamepads[MAX_GAMEPADS];

static int activeGamepadMask = 0;

static void send_controller_arrival_sdl(PGAMEPAD_STATE state) {
  unsigned int supportedButtonFlags = 0;
  unsigned short capabilities = 0;
  unsigned char type = LI_CTYPE_UNKNOWN;
  // for check gamepad num
  gpNumForCheck++;
  SDL_PropertiesID props = SDL_GetGamepadProperties(state->controller);

  for (int i = 0; i < SDL_arraysize(SDL_TO_LI_BUTTON_MAP); i++) {
    if (SDL_GamepadHasButton(state->controller, (SDL_GamepadButton)i)) {
        supportedButtonFlags |= SDL_TO_LI_BUTTON_MAP[i];
    }
  }

  if (SDL_GamepadHasAxis(state->controller, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) ||
      SDL_GamepadHasAxis(state->controller, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
    capabilities |= LI_CCAP_ANALOG_TRIGGERS;
  if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
    capabilities |= LI_CCAP_RUMBLE;
  if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false))
    capabilities |= LI_CCAP_TRIGGER_RUMBLE;
  if (SDL_GetNumGamepadTouchpads(state->controller) > 0)
    capabilities |= LI_CCAP_TOUCHPAD;
  if (SDL_GamepadHasSensor(state->controller, SDL_SENSOR_ACCEL))
    capabilities |= LI_CCAP_ACCEL;
  if (SDL_GamepadHasSensor(state->controller, SDL_SENSOR_GYRO))
    capabilities |= LI_CCAP_GYRO;
  if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false))
    capabilities |= LI_CCAP_RGB_LED;

  switch (SDL_GetGamepadType(state->controller)) {
  case SDL_GAMEPAD_TYPE_XBOX360:
  case SDL_GAMEPAD_TYPE_XBOXONE:
    type = LI_CTYPE_XBOX;
    break;
  case SDL_GAMEPAD_TYPE_PS3:
  case SDL_GAMEPAD_TYPE_PS4:
  case SDL_GAMEPAD_TYPE_PS5:
    type = LI_CTYPE_PS;
    break;
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    type = LI_CTYPE_NINTENDO;
    break;
  }

  LiSendControllerArrivalEvent(state->id, activeGamepadMask, type, supportedButtonFlags, capabilities);
}

static PGAMEPAD_STATE get_gamepad(SDL_JoystickID sdl_id, bool add) {
  // See if a gamepad already exists
  for (int i = 0;i<MAX_GAMEPADS;i++) {
    if (gamepads[i].initialized && gamepads[i].sdl_id == sdl_id)
      return &gamepads[i];
  }

  if (!add)
    return NULL;

  for (int i = 0;i<MAX_GAMEPADS;i++) {
    if (!gamepads[i].initialized) {
      gamepads[i].sdl_id = sdl_id;
      gamepads[i].id = i;
      gamepads[i].initialized = true;

      activeGamepadMask |= (1 << i);

      return &gamepads[i];
    }
  }

  return &gamepads[0];
}

static int x11_sdlinput_handle_event(SDL_Event* event);
static int add_gamepad(int joystick_id) {
  int ret = 0;
  SDL_Gamepad* controller = SDL_OpenGamepad(joystick_id);
  if (!controller) {
    fprintf(stderr, "Could not open gamecontroller %i: %s\n", joystick_id, SDL_GetError());
    return ret;
  }

  // Check if we have already set up a state for this gamepad
  PGAMEPAD_STATE state = get_gamepad(joystick_id, false);
  if (state) {
    // This was probably a gamepad added during initialization, so we've already
    // got state set up. However, we still need to inform the host about it, since
    // we couldn't do that during initialization (since we weren't connected yet).
    send_controller_arrival_sdl(state);

    SDL_CloseGamepad(controller);
    return ret;
  }

  // Create a new gamepad state
  state = get_gamepad(joystick_id, true);
  state->controller = controller;

  // Send the controller arrival event to the host
  send_controller_arrival_sdl(state);

  sdl_gamepads++;
  return sdl_gamepads;
}

static void remove_gamepad(SDL_JoystickID sdl_id) {
  for (int i = 0;i<MAX_GAMEPADS;i++) {
    if (gamepads[i].initialized && gamepads[i].sdl_id == sdl_id) {

      SDL_CloseGamepad(gamepads[i].controller);

      // This will cause disconnection of the virtual controller on the host PC
      activeGamepadMask &= ~(1 << i);
      LiSendMultiControllerEvent(i, activeGamepadMask, 0, 0, 0, 0, 0, 0, 0);

      memset(&gamepads[i], 0, sizeof(*gamepads));
      sdl_gamepads--;
      // for check gamepad num
      gpNumForCheck--;
      break;
    }
  }
}

static void sdlinput_init(char* mappings) {
  memset(gamepads, 0, sizeof(gamepads));

  SDL_Init(SDL_INIT_GAMEPAD);

  SDL_AddGamepadMappingsFromFile(mappings);
}

static int x11_sdlinput_handle_event(SDL_Event* event) {
  unsigned char touchEventType;
  PGAMEPAD_STATE gamepad;
  switch (event->type) {
  case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    gamepad = get_gamepad(event->gaxis.which, false);
    if (!gamepad)
      return SDL_NOTHING;
    switch (event->gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      gamepad->leftStickX = event->gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      gamepad->leftStickY = -SDL_max(event->gaxis.value, (short)-32767);
      break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      gamepad->rightStickX = event->gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      gamepad->rightStickY = -SDL_max(event->gaxis.value, (short)-32767);
      break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      gamepad->leftTrigger = (unsigned char)(event->gaxis.value * 255UL / 32767);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      gamepad->rightTrigger = (unsigned char)(event->gaxis.value * 255UL / 32767);
      break;
    default:
      return SDL_NOTHING;
    }
    LiSendMultiControllerEvent(gamepad->id, activeGamepadMask, gamepad->buttons, gamepad->leftTrigger, gamepad->rightTrigger, gamepad->leftStickX, gamepad->leftStickY, gamepad->rightStickX, gamepad->rightStickY);
    break;
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
  case SDL_EVENT_GAMEPAD_BUTTON_UP:
    gamepad = get_gamepad(event->gbutton.which, false);
    if (!gamepad)
      return SDL_NOTHING;
    if (event->gbutton.button >= SDL_arraysize(SDL_TO_LI_BUTTON_MAP))
      return SDL_NOTHING;

    int now_buttons = SDL_TO_LI_BUTTON_MAP[event->gbutton.button];
    if (swapXYAB) {
      switch (now_buttons) {
      case A_FLAG:
        now_buttons = B_FLAG;
        break;
      case B_FLAG:
        now_buttons = A_FLAG;
        break;
      case X_FLAG:
        now_buttons = Y_FLAG;
        break;
      case Y_FLAG:
        now_buttons = X_FLAG;
        break;
      }
    }
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
      gamepad->buttons |= now_buttons;
    else
      gamepad->buttons &= ~now_buttons;

    if ((gamepad->buttons & QUIT_BUTTONS) == QUIT_BUTTONS)
      return SDL_QUIT_APPLICATION;

    LiSendMultiControllerEvent(gamepad->id, activeGamepadMask, gamepad->buttons, gamepad->leftTrigger, gamepad->rightTrigger, gamepad->leftStickX, gamepad->leftStickY, gamepad->rightStickX, gamepad->rightStickY);
    break;
  case SDL_EVENT_GAMEPAD_ADDED:
    add_gamepad(event->gdevice.which);
    break;
  case SDL_EVENT_GAMEPAD_REMOVED:
    remove_gamepad(event->gdevice.which);
    break;
  case SDL_EVENT_SENSOR_UPDATE:
    gamepad = get_gamepad(event->gsensor.which, false);
    if (!gamepad)
      return SDL_NOTHING;
    switch (event->gsensor.sensor) {
    case SDL_SENSOR_ACCEL:
      LiSendControllerMotionEvent(gamepad->id, LI_MOTION_TYPE_ACCEL, event->gsensor.data[0], event->gsensor.data[1], event->gsensor.data[2]);
      break;
    case SDL_SENSOR_GYRO:
      // Convert rad/s to deg/s
      LiSendControllerMotionEvent(gamepad->id, LI_MOTION_TYPE_GYRO,
                                  event->gsensor.data[0] * 57.2957795f,
                                  event->gsensor.data[1] * 57.2957795f,
                                  event->gsensor.data[2] * 57.2957795f);
      break;
    }
    break;
  case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
  case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
  case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    gamepad = get_gamepad(event->gtouchpad.which, false);
    if (!gamepad)
      return SDL_NOTHING;
    switch (event->type) {
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
      touchEventType = LI_TOUCH_EVENT_DOWN;
      break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
      touchEventType = LI_TOUCH_EVENT_UP;
      break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
      touchEventType = LI_TOUCH_EVENT_MOVE;
      break;
    default:
      return SDL_NOTHING;
    }
    LiSendControllerTouchEvent(gamepad->id, touchEventType, event->gtouchpad.finger,
                               event->gtouchpad.x, event->gtouchpad.y, event->gtouchpad.pressure);
    break;
  }

  return SDL_NOTHING;
}

static void x11_sdl_stop () {
  for (int i=0;i<MAX_GAMEPADS;i++) {
    if (gamepads[i].initialized) {
      remove_gamepad(gamepads[i].sdl_id);
    }
  }

  SDL_Quit();
}

static int x11_sdl_event_handle(void *pointer) {
  SDL_Event event;

  while (!done && SDL_WaitEvent(&event)) {
    switch (x11_sdlinput_handle_event(&event)) {
    case SDL_QUIT_APPLICATION:
#if defined(HAVE_X11) || defined(HAVE_WAYLAND) || defined(HAVE_DRM)
      if (keyboardpipefd > -1) {
        write(keyboardpipefd, &quitstate, sizeof(quitstate));
      }
#endif
      done = true;
      break;
    default:
      if (event.type == SDL_EVENT_QUIT)
        done = true;
      break;
    }
  }

  return 0;
}

int x11_sdl_init (char* mappings) {
  sdl_gamepads = 0;
  int count = 0;
  sdlinput_init(mappings);

  // Add game controllers here to ensure an accurate count
  // goes to the host when starting a new session.
  SDL_JoystickID *glist = SDL_GetGamepads(&count);
  for (int i = 0; i < count; ++i) {
    if (add_gamepad(glist[i]) < 1) {
      char guidStr[33];
      SDL_GUID gguid = SDL_GetJoystickGUIDForID(glist[i]);
      SDL_GUIDToString(gguid, guidStr, sizeof(guidStr));
      const char* name = SDL_GetJoystickNameForID(glist[i]);
      fprintf(stderr, "No mapping available for %s (%s).Use 'x11/antimicrox' or others to create mapping to ~/.config/moonlight/gamecontrollerdb.txt\n", name, guidStr);
    }
  }

  thread = SDL_CreateThread(x11_sdl_event_handle, "sdl_event_handle", NULL);
  if (thread == NULL) {
    fprintf(stderr, "Can't create sdl poll event thread.\n");
    SDL_Quit();
    return -1;
  }

  x11_sdl_start = true;

  return 0;
}

void sdlinput_rumble(unsigned short controller_id, unsigned short low_freq_motor, unsigned short high_freq_motor) {
  if (controller_id >= MAX_GAMEPADS)
    return;

  PGAMEPAD_STATE state = &gamepads[controller_id];

  if (!state->initialized)
    return;

  SDL_RumbleGamepad(state->controller, low_freq_motor, high_freq_motor, 30000);
}

void sdlinput_rumble_triggers(unsigned short controller_id, unsigned short left_trigger, unsigned short right_trigger) {
  PGAMEPAD_STATE state = &gamepads[controller_id];

  if (!state->initialized)
    return;

  SDL_RumbleGamepadTriggers(state->controller, left_trigger, right_trigger, 30000);
}

void sdlinput_set_motion_event_state(unsigned short controller_id, unsigned char motion_type, unsigned short report_rate_hz) {
  PGAMEPAD_STATE state = &gamepads[controller_id];

  if (!state->initialized)
    return;

  switch (motion_type) {
  case LI_MOTION_TYPE_ACCEL:
    SDL_SetGamepadSensorEnabled(state->controller, SDL_SENSOR_ACCEL, report_rate_hz ? true : false);
    break;
  case LI_MOTION_TYPE_GYRO:
    SDL_SetGamepadSensorEnabled(state->controller, SDL_SENSOR_GYRO, report_rate_hz ? true : false);
    break;
  }
}

void sdlinput_set_controller_led(unsigned short controller_id, unsigned char r, unsigned char g, unsigned char b) {
  PGAMEPAD_STATE state = &gamepads[controller_id];

  if (!state->initialized)
    return;

  SDL_SetGamepadLED(state->controller, r, g, b);
}

void x11_sdl_clear() {
  if (!x11_sdl_start) return;
  x11_sdl_start = false;
  SDL_Event event;
  event.type = SDL_EVENT_QUIT;
  SDL_PushEvent(&event);
  SDL_WaitThread(thread, NULL);
  x11_sdl_stop();
  return;
}
#endif /* HAVE_SDL */
