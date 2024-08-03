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

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include "video.h"
#include "ffmpeg.h"
#include "x11.h"
#include "../input/x11.h"

static Display *display = NULL;
static Window window;

static int display_width, display_height, screen_width, screen_height;

static bool startedMuiltiThreads = false;

void* x_get_display(const char *device) {
  if (display == NULL) {
    display = XOpenDisplay(device);
  }

  return display;
}

void x_close_display() {
  if (display != NULL)
    XCloseDisplay(display);
  display = NULL;
}

void x_muilti_threads() {
  if (!startedMuiltiThreads) {
    XInitThreads();
    startedMuiltiThreads = true;
  }
}

void x_get_resolution (int *width, int *height) {
  *width = screen_width;
  *height = screen_height;
}

int x_setup(int width, int height, int drFlags) {

  if (!display) {
    fprintf(stderr, "Error: failed to open X display.\n");
    return -1;
  }

  Screen* screen = DefaultScreenOfDisplay(display);
  screen_width = WidthOfScreen(screen);
  screen_height = HeightOfScreen(screen);
  if (drFlags & DISPLAY_FULLSCREEN) {
    display_width = screen_width;
    display_height = screen_height;
  } else {
    display_width = width;
    display_height = height;
  }

  Window root = DefaultRootWindow(display);
  XSetWindowAttributes winattr = { .event_mask = FocusChangeMask | EnterWindowMask | LeaveWindowMask};
  window = XCreateWindow(display, root, 0, 0, display_width, display_height, 0, CopyFromParent, InputOutput, CopyFromParent, CWEventMask, &winattr);
  XSelectInput(display, window, StructureNotifyMask);
  XMapWindow(display, window);
  XStoreName(display, window, "Moonlight");

  if (drFlags & DISPLAY_FULLSCREEN) {
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);

    XEvent xev = {0};
    xev.type = ClientMessage;
    xev.xclient.window = window;
    xev.xclient.message_type = wm_state;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = 1;
    xev.xclient.data.l[1] = fullscreen;
    xev.xclient.data.l[2] = 0;

    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);
  }
  XFlush(display);

  x11_input_init(display, window);

  return 0;
}

void* x_get_window() {
  return &window;
}
