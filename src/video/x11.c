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
#include <stdlib.h>
#include <string.h>

#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include "display.h"
#include "video.h"
#include "video_internal.h"
#include "render.h"
#include "ffmpeg.h"
#include "ffmpeg_vaapi_x11.h"
#include "../input/x11.h"

static Display *display = NULL;
static Window window;

static int display_width, display_height, screen_width, screen_height;
static int frame_width, frame_height;

static bool startedMuiltiThreads = false;

/*
static void x_multi_threads() {
  if (!startedMuiltiThreads) {
    XInitThreads();
    startedMuiltiThreads = true;
  }
}
*/

static void* x_get_display(const char* *device) {
/*
  x_multi_threads();
*/
  if (display == NULL) {
    display = XOpenDisplay(*device);
  }

  if (display)
    *device = getenv("DISPLAY");

  return display;
}

static void x_close_display() {
  if (startedMuiltiThreads) {
    XFreeThreads();
    startedMuiltiThreads = false;
  }
  if (display != NULL)
    XCloseDisplay(display);
  display = NULL;
}

static void x_get_resolution (int *width, int *height) {
  *width = screen_width;
  *height = screen_height;
}

static int x_setup(int width, int height, int drFlags) {

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

static void* x_get_window() {
  return &window;
}

static void x_setup_post(void *data) {};
static int x_put_to_screen(int w, int h, int i) { return 0; };
static void x_change_cursor(const char *op) {};

static int x_render_init(struct Render_Init_Info *paras) {
  frame_width = paras->frame_width;
  frame_height = paras->frame_height;
  // direct render not support yuv444
  if (paras->is_yuv444)
    return -1;
  return 0;
}
static void x_render_destroy() {};
static int x_render_create(struct Render_Init_Info *paras) { return 0; };
static int x_draw(union Render_Image image) { 
  vaapi_queue(image.frame_data, window, display_width, display_height, frame_width, frame_height);
  return 0;
}

struct DISPLAY_CALLBACK display_callback_x11 = {
  .name = "x11",
  .egl_platform = 0x31D5,
  .format = NOT_CARE,
  .hdr_support = false,
  .display_get_display = x_get_display,
  .display_get_window = x_get_window,
  .display_close_display = x_close_display,
  .display_setup = x_setup,
  .display_setup_post = x_setup_post,
  .display_put_to_screen = x_put_to_screen,
  .display_get_resolution = x_get_resolution,
  .display_change_cursor = x_change_cursor,
  .display_vsync_loop = NULL,
  .display_exported_buffer_info = NULL,
  .renders = (EGL_RENDER | X11_RENDER),
};

struct RENDER_CALLBACK x11_render = {
  .name = "x11",
  .display_name = "x11",
  .is_hardaccel_support = true,
  .render_type = X11_RENDER,
  .decoder_type = VAAPI,
  .data = NULL,
  .render_create = x_render_create,
  .render_init = x_render_init,
  .render_sync_config = NULL,
  .render_draw = x_draw,
  .render_destroy = x_render_destroy,
};
