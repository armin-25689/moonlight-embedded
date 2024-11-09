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

#define QUITCODE "quit"
// compitible for some uint32_t format 
#define NOT_CARE 0

struct DISPLAY_CALLBACK {
  char *name;
  // equal to EGL_PLATFORM_XXX_KHR
  int egl_platform;
  int format;
  bool hdr_support;
  void* (*display_get_display) (const char* *device);
  void* (*display_get_window) ();
  void (*display_close_display) ();
  int (*display_setup) (int width, int height, int drFlags);
  void (*display_setup_post) (void *data);
  int (*display_put_to_screen) (int width, int height, int index);
  void (*display_get_resolution) (int* width, int* height);
  void (*display_change_cursor) (const char *oprate);
  int (*display_vsync_loop) (bool *exit, int *index, void(*loop_pre)(void), void(*loop_post)(void));
  void (*display_exported_buffer_info) (struct Source_Buffer_Info *buffer, int *buffersNum, int *planesNum);
  int renders;
};

#ifdef HAVE_X11
extern struct DISPLAY_CALLBACK display_callback_x11;
#endif
#ifdef HAVE_WAYLAND
extern struct DISPLAY_CALLBACK display_callback_wayland;
#endif
#ifdef HAVE_GBM
extern struct DISPLAY_CALLBACK display_callback_gbm;
#endif
