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

#include "video_internal.h"

#pragma once

#define QUITCODE "quit"
// compitible for some uint32_t format 
#define NOT_CARE 0
#define NEED_CHANGE_WINDOW_SIZE 1
#define INPUTING 0x01
#define HIDE_CURSOR 0x02

struct WINDOW_OP {
  bool hide_cursor;
  bool inputing;
};

struct _WINDOW_PROPERTIES {
  int fd;
  int *fd_p;
  long long int *configure;
};

struct DISPLAY_CALLBACK {
  char *name;
  // equal to EGL_PLATFORM_XXX_KHR
  int egl_platform;
  int format;
  bool hdr_support;
  void* (*display_get_display) (const char* *device);
  void* (*display_get_window) ();
  void (*display_close_display) (void *data);
  int (*display_setup) (int width, int height, int fps, int drFlags);
  void (*display_setup_post) (void *data);
  int (*display_put_to_screen) (int width, int height, int index);
  void (*display_get_resolution) (int* width, int* height, bool isfullscreen);
  void (*display_modify_window) (struct WINDOW_OP *oprate, int flags);
  int (*display_vsync_loop) (bool *exit, int width, int height, int index);
  void (*display_exported_buffer_info) (struct Source_Buffer_Info *buffer, int *buffersNum, int *planesNum);
  int renders;
};

#ifdef HAVE_X11
extern struct DISPLAY_CALLBACK display_callback_x11;
#endif
#ifdef HAVE_WAYLAND
extern struct DISPLAY_CALLBACK display_callback_wayland;
#endif
#ifdef HAVE_DRM
extern struct DISPLAY_CALLBACK display_callback_drm;
#endif
