/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Iwan Timmer
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

#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <stdbool.h>
#include <stdlib.h>

#include "x11.h"
#include "evdev.h"
#include "keyboard.h"

#include "../loop.h"

#include <Limelight.h>

#define ACTION_MODIFIERS (MODIFIER_ALT|MODIFIER_CTRL)
#define RESET_KEY_MONITOR do { \
    wait_key_release = false; \
    keyboard_modifiers = 0; \
} while(0)
#define RESET_INPUT do { \
    grabbed = false; \
    XUngrabPointer(display, CurrentTime); \
    XUngrabKeyboard(display, CurrentTime); \
    evdev_switch_mouse_mode(EVDEV_HANDLE_BY_WINDOW); \
} while(0)

static Display *display;
static Window window;
static int displayFd = -1;
static Atom wm_deletemessage;

int x_display_width = 0;
int x_display_height = 0;

static int keyboard_modifiers;
static int last_x = -1, last_y = -1;
static int semi_width, semi_height;
static const char data[1] = {0};
static Cursor cursor;
static bool grabbed = False;
static bool in_window = True;
static bool inputing = True;
static bool wait_key_release = false;
static bool keep_display_cursor = false;

static int x11_handler(int fd, void *data) {
  XEvent event;
  int motion_x, motion_y;

  while (XPending(display)) {
    XNextEvent(display, &event);
    switch (event.type) {
    case ConfigureNotify:
      x_display_width = ((XConfigureEvent)event.xconfigure).width;
      x_display_height = ((XConfigureEvent)event.xconfigure).height;
      semi_width = (int)x_display_width / 2;
      semi_height = (int)x_display_height / 2;
      break;
    case DestroyNotify:
      grab_window(E_UNGRAB_WINDOW);
      break;
    case EnterNotify:
    case LeaveNotify:
    case FocusIn:
    case FocusOut:
      if (event.type == FocusIn || event.type == EnterNotify) {
        in_window = true;
        sync_input_state(true);
        if (inputing)
          LiSendMousePositionEvent(last_x < 0 ? 0 : last_x, last_y < 0 ? 0 : last_y, x_display_width, x_display_height);
        if (grabbed)
          evdev_switch_mouse_mode(EVDEV_HANDLE_BY_EVDEV);
      } else {
        in_window = false;
        sync_input_state(false);
        RESET_KEY_MONITOR;
      }
      XDefineCursor(display, window, (!keep_display_cursor && in_window) ? cursor : 0);
      break;
    case ClientMessage:
      if (event.xclient.data.l[0] == wm_deletemessage)
        return LOOP_RETURN;
      break;
    case KeyPress:
    case KeyRelease:
      if (event.xkey.keycode >= 8 && event.xkey.keycode < (sizeof(keyCodes)/sizeof(keyCodes[0]) + 8)) {
        int modifier = 0;
        switch (event.xkey.keycode) {
        case 0x40:
        case 0x6c:
          modifier = MODIFIER_ALT;
          break;
        case 0x25:
        case 0x69:
          modifier = MODIFIER_CTRL;
          break;
        // let others key map to shift,for check whether have key pressed but not alt+ctrl
        default:
          modifier = MODIFIER_SHIFT;
          break;
        }
        if (modifier != 0) {
          if (event.type == KeyPress)
            keyboard_modifiers |= modifier;
          else
            keyboard_modifiers &= ~modifier;
        }

        if ((keyboard_modifiers & ACTION_MODIFIERS) == ACTION_MODIFIERS && event.type == KeyPress) {
          if (modifier != 0 && inputing) {
            wait_key_release = true;
          }
          else {
            RESET_KEY_MONITOR;
            break;
          }
        }
        else if (wait_key_release && keyboard_modifiers == 0 && event.type == KeyRelease) {
          wait_key_release = false;
          if (grabbed) {
            RESET_INPUT;
          }
          else {
            if (XGrabPointer(display, window, true, 0, GrabModeAsync, GrabModeAsync, window, None, CurrentTime) == GrabSuccess) {
              evdev_switch_mouse_mode(EVDEV_HANDLE_BY_EVDEV);
              XGrabKeyboard(display, window, true, GrabModeAsync, GrabModeAsync, CurrentTime);
              grabbed = true;
            }
          }
        }
      }
      break;
    case MotionNotify:
      motion_x = event.xmotion.x - last_x;
      motion_y = event.xmotion.y - last_y;
      if (abs(motion_x) > 0 || abs(motion_y) > 0) {
        if (last_x >= 0 && last_y >= 0 && inputing) {
          if (!grabbed)
            LiSendMouseMoveAsMousePositionEvent(motion_x, motion_y, x_display_width, x_display_height);
/*
          // handled by evdev instead
          if (grabbed)
*/
        }

        if (grabbed)
          XWarpPointer(display, None, window, 0, 0, 0, 0, semi_width, semi_height);
      }

      last_x = grabbed ? semi_width : event.xmotion.x;
      last_y = grabbed ? semi_height : event.xmotion.y;
      break;
    }
  }

  return LOOP_OK;
}

void x11_input_init(Display* x11_display, Window x11_window) {
  display = x11_display;
  window = x11_window;
  semi_width = x_display_width > 0 ? ((int)x_display_width / 2) : 640;
  semi_height = x_display_height > 0 ? ((int)x_display_height / 2) : 360;

  wm_deletemessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
  Atom prop_list[1] = { wm_deletemessage };
  XSetWMProtocols(display, window, prop_list, 1);

  /* make a blank cursor */
  XColor dummy;
  Pixmap blank = XCreateBitmapFromData(display, window, data, 1, 1);
  cursor = XCreatePixmapCursor(display, blank, blank, &dummy, &dummy, 0, 0);
  XFreePixmap(display, blank);
  XDefineCursor(display, window, cursor);

  displayFd = ConnectionNumber(display);
  if (displayFd > -1)
    loop_add_fd(displayFd, &x11_handler, EPOLLIN | EPOLLERR | EPOLLHUP);
}

void x11_input_remove () {
  if (displayFd > -1) {
    loop_remove_fd(displayFd);
    displayFd = -1;
  }
}


void x11_change_input_stat (bool isinput) {
  inputing = isinput;
  if (!inputing) {
    if (grabbed) {
      RESET_INPUT;
    }
    RESET_KEY_MONITOR;
  }
}

void x11_keep_display_cursor (bool isdisplay) {
  keep_display_cursor = isdisplay;
  XDefineCursor(display, window, isdisplay ? 0 : cursor);
}
