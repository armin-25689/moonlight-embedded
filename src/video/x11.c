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

#include "../input/x11.h"
#ifndef USE_X11
#define USE_X11 1
#endif
#include "video.h"
#include "ffmpeg.h"
#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif

#include "x11.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static Display *display = NULL;
static Window window;

static int display_width;
static int display_height;

static bool startedMuiltiThreads = false;

void x_vaapi_draw(AVFrame* frame, int width, int height) {
  #ifdef HAVE_VAAPI
  return vaapi_queue(frame, window, width, height);
  #endif
}

bool x_test_vaapi_draw(AVFrame* frame, int width, int height) {
  #ifdef HAVE_VAAPI
  return test_vaapi_queue(frame, window, width, height);
  #endif
}

void* x_get_display(const char *device) {
  if (display == NULL) {
    display = (Display *) get_display_from_vaapi(true);
    if (display == NULL)
      display = XOpenDisplay(device);
  }

  return display;
}

void x_close_display() {
  if (display != NULL)
    XCloseDisplay(display);
}

void x_muilti_threads() {
  if (!startedMuiltiThreads) {
    XInitThreads();
    startedMuiltiThreads = true;
  }
}

void x_get_resolution (int *width, int *height) {
  *width = display_width;
  *height = display_height;
}

int x_setup(int width, int height, int drFlags) {

  if (!display) {
    fprintf(stderr, "Error: failed to open X display.\n");
    return -1;
  }

  if (drFlags & DISPLAY_FULLSCREEN) {
    Screen* screen = DefaultScreenOfDisplay(display);
    display_width = WidthOfScreen(screen);
    display_height = HeightOfScreen(screen);
  } else {
    display_width = width;
    display_height = height;
  }

  Window root = DefaultRootWindow(display);
  XSetWindowAttributes winattr = { .event_mask = FocusChangeMask };
  window = XCreateWindow(display, root, 0, 0, display_width, display_height, 0, CopyFromParent, InputOutput, CopyFromParent, CWEventMask, &winattr);
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

EGLSurface x_get_egl_surface(EGLDisplay display, EGLConfig config, void *data) {
  return eglCreateWindowSurface(display, config, window, data);
}

EGLDisplay x_get_egl_display() {
  return eglGetDisplay(display);
}
