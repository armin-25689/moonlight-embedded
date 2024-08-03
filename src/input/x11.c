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

#include "x11.h"
#include "evdev.h"

#include "../loop.h"

#include <Limelight.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <stdbool.h>
#include <stdlib.h>

static Display *display;
static Window window;
static int displayFd = -1;
static bool isMapedWindow = false;

static Atom wm_deletemessage;

static const char data[1] = {0};
static Cursor cursor;
static bool grabbed = True;

static int x11_handler(int fd, void *data) {
  XEvent event;

  while (XPending(display)) {
    XNextEvent(display, &event);
    switch (event.type) {
    case MapNotify:
      if (!isMapedWindow) {
        grab_window(true);
        isMapedWindow = true;
      }
      break;
    case DestroyNotify:
      grab_window(false);
      break;
    case EnterNotify:
    case LeaveNotify:
    case FocusIn:
    case FocusOut:
      if (event.type == FocusIn || event.type == EnterNotify) {
        grabbed = true;
        fake_grab_window(true);
      } else {
        grabbed = false;
        fake_grab_window(false);
      }
      XDefineCursor(display, window, grabbed ? cursor : 0);
      break;
    case ClientMessage:
      if (event.xclient.data.l[0] == wm_deletemessage)
        return LOOP_RETURN;

      break;
    }
  }

  return LOOP_OK;
}

void x11_input_init(Display* x11_display, Window x11_window) {
  display = x11_display;
  window = x11_window;

  wm_deletemessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(display, window, &wm_deletemessage, 1);

  /* make a blank cursor */
  XColor dummy;
  Pixmap blank = XCreateBitmapFromData(display, window, data, 1, 1);
  cursor = XCreatePixmapCursor(display, blank, blank, &dummy, &dummy, 0, 0);
  XFreePixmap(display, blank);
  XDefineCursor(display, window, cursor);

  displayFd = ConnectionNumber(display);
  if (displayFd > -1)
    loop_add_fd(displayFd, &x11_handler, EPOLLIN | EPOLLERR | EPOLLHUP);

  isMapedWindow = false;
}

void x11_input_remove () {
  if (displayFd > -1) {
    loop_remove_fd(displayFd);
    displayFd = -1;
  }
}
