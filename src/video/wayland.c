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

#ifdef HAVE_WAYLAND
#include <wayland-client.h>
#include <wayland-egl.h>

#include "video.h"
#include "video_internal.h"
#include "display.h"
#include "xdg-shell-client-protocol.h"
#include "wp-viewporter.h"
#include "wlr-output-management.h"
#include "zwp-pointer-constraints.h"
#include "zwp-relative-pointer.h"
#include "../input/evdev.h"

#include <Limelight.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ACTION_MODIFIERS (MODIFIER_ALT|MODIFIER_CTRL)
#define UNGRAB_WINDOW do { \
    last_x = -1; \
    last_y = -1; \
    if (zwp_pointer_constraints && isGrabing) { \
      zwp_locked_pointer_v1_destroy(zwp_locked_pointer); \
      zwp_locked_pointer = NULL; \
      isGrabing = false; \
    } \
} while(0)
#define RESET_KEY_MONITOR do { \
    wait_key_release = false; \
    keyboard_modifiers = 0; \
} while(0)
#define WHEN_WINDOW_ENTER do { \
    sync_input_state(true && inputing); \
} while(0)
// LiSendMousePositionEvent(display_width, display_height, display_width, display_height);
#define WHEN_WINDOW_LEAVE do { \
    UNGRAB_WINDOW; \
    sync_input_state(false); \
} while(0)
#define MV_CURSOR(nowx, nowy, lastx, lasty, wlpointer, pointersurface, serial) do { \
    wl_pointer_set_cursor(wlpointer, serial, pointersurface, nowx, nowy); \
    LiSendMousePositionEvent(nowx, nowy, display_width, display_height); \
    lastx = nowx; \
    lasty = nowy; \
} while(0)

static struct wl_display *wl_display = NULL;
static struct wl_surface *wlsurface;
static struct wl_egl_window *wl_window = NULL;
static struct wl_compositor *compositor;
static struct wl_registry *registry;
static struct wl_output *wl_output = NULL;
static struct wl_seat *wl_seat = NULL;
static struct wl_pointer *wl_pointer = NULL;
static struct wl_keyboard *wl_keyboard = NULL;
static struct xdg_wm_base *xdg_wm_base;
static struct xdg_toplevel *xdg_toplevel;
static struct xdg_surface *xdg_surface;
static struct wp_viewporter *wp_viewporter = NULL;
static struct wp_viewport *wp_viewport = NULL;
static struct zwlr_output_manager_v1 *wlr_output_manager = NULL;
static struct zwp_pointer_constraints_v1 *zwp_pointer_constraints = NULL;
static struct zwp_locked_pointer_v1 *zwp_locked_pointer = NULL;
static struct zwp_relative_pointer_manager_v1 *zwp_relative_pointer_manager = NULL;
static struct zwp_relative_pointer_v1 *zwp_relative_pointer = NULL;

static const char *quitCode = QUITCODE;

static int offset_x = 0, offset_y = 0;
static int display_width = 0, display_height = 0;
static int output_width = 0, output_height = 0;
static int *window_op_fd_p = NULL;
static int32_t outputScaleFactor = 0;
static uint32_t pointerSerial = 0;
static double scale_factor = 1.0;
static double fractionalScale = 0;
static bool isFullscreen = false;
static bool inWindowP = false;
static bool inWindowK = true;

static bool firstHide = true;
static bool isGrabing = false;
static bool inputing = true;
static bool wait_key_release = false;
static int last_x = -1, last_y = -1;
static int keyboard_modifiers = 0;

static void noop() {};

static void wlr_output_get_scale (void *data,
                             struct zwlr_output_head_v1 *zwlr_output_head_v1,
                             wl_fixed_t scale) {
  fractionalScale = wl_fixed_to_double(scale);
}

static const struct zwlr_output_head_v1_listener wlr_output_head_listener = {
  .name = noop,
  .description = noop,
  .physical_size = noop,
  .mode = noop,
  .enabled = noop,
  .current_mode = noop,
  .position = noop,
  .transform = noop,
  .scale = wlr_output_get_scale,
  .finished = noop,
};

static void add_head_listener (void *data,
                               struct zwlr_output_manager_v1 *zwlr_output_manager_v1,
                               struct zwlr_output_head_v1 *head) {
  zwlr_output_head_v1_add_listener(head, &wlr_output_head_listener, NULL);
}

static const struct zwlr_output_manager_v1_listener wlr_output_manager_listener = {
  .head = add_head_listener,
  .done = noop,
  .finished = noop,
};

static void wl_output_get_mode (void *data, struct wl_output *wl_output, uint32_t flags,
                                int32_t width, int32_t height, int32_t refresh) {
  output_width = width;
  output_height = height;
}

static void wl_output_get_scale (void *data, struct wl_output *wl_output, int32_t factor) {
  outputScaleFactor = factor;
}

static const struct wl_output_listener wl_output_listener = {
  .geometry = noop,
  .mode = wl_output_get_mode,
  .done = noop,
  .scale = wl_output_get_scale,
/*
  for version 4
  .name = noop,
  .description = noop,
*/
};

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
  int modifier = 0;

  switch (key) {
  case 0x38:
  case 0x64:
    modifier = MODIFIER_ALT;
    break;
  case 0x1d:
  case 0x61:
    modifier = MODIFIER_CTRL;
    break;
  }
  if (modifier != 0) {
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
      keyboard_modifiers |= modifier;
    else if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
      keyboard_modifiers &= ~modifier;
  }

  if ((keyboard_modifiers & ACTION_MODIFIERS) == ACTION_MODIFIERS && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    if (modifier != 0) {
      wait_key_release = true;
    }
    else {
      RESET_KEY_MONITOR;
    }
  }
  else if (wait_key_release && keyboard_modifiers == 0 && state == WL_KEYBOARD_KEY_STATE_RELEASED) {
    wait_key_release = false;
    if (inWindowP && inWindowK && inputing) {
      if (zwp_pointer_constraints && isGrabing) {
        UNGRAB_WINDOW;
      }
      else if (zwp_pointer_constraints && zwp_relative_pointer) {
        zwp_locked_pointer = zwp_pointer_constraints_v1_lock_pointer(zwp_pointer_constraints, wlsurface, wl_pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
        if (zwp_locked_pointer) {
          isGrabing = true;
        }
      }
      else {
        printf("WARNING: wayland server not support the zwp_pointer_constraints_v1 interface\n");
      }
    }
  }
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
  //same as pointer_enter
  WHEN_WINDOW_ENTER;
  inWindowK = true;
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface) {
  //same as pointer_leave
  WHEN_WINDOW_LEAVE;
  RESET_KEY_MONITOR;
  inWindowK = false;
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
  .keymap = noop,
  .enter = keyboard_enter,
  .leave = keyboard_leave,
  .key = keyboard_key,
  .modifiers = noop,
  // version 4
  .repeat_info = noop,
};

static void pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *pointer_surface, wl_fixed_t pointer_x, wl_fixed_t pointer_y) {
  // NULL is hidden cursor
  pointerSerial = serial;
  inWindowP = true;
  WHEN_WINDOW_ENTER;
  int sx = wl_fixed_to_int(pointer_x);
  int sy = wl_fixed_to_int(pointer_y);

  if (sx > display_width || sy > display_height)
    return;

  if (firstHide) {
    MV_CURSOR(sx, sy, last_x, last_y, wl_pointer, NULL, serial);
    firstHide = false;
  }
  else {
    if (inputing) {
      MV_CURSOR(sx, sy, last_x, last_y, wl_pointer, NULL, serial);
    }
  }
}

static void pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *wl_surface) {
  WHEN_WINDOW_LEAVE;
  RESET_KEY_MONITOR;
  inWindowP = false; 
}

static void pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
  int motion_x, motion_y, sx, sy;
  if (!isGrabing) {
    sx = wl_fixed_to_int(surface_x);
    sy = wl_fixed_to_int(surface_y);
    motion_x = sx - last_x;
    motion_y = sy - last_y;
    if (abs(motion_x) > 0 || abs(motion_y) > 0) {
      if (last_x >= 0 && last_y >= 0 && inputing) {
        LiSendMouseMoveAsMousePositionEvent(motion_x, motion_y, display_width, display_height);
      }
    }
  
    last_x = sx;
    last_y = sy;
  }
}

static const struct wl_pointer_listener wl_pointer_listener = {
  .enter = pointer_enter,
  .leave = pointer_leave,
  .motion = pointer_motion,
  .button = noop,
  .axis = noop,
  // version 5
  .frame = noop,
  .axis_source = noop,
  .axis_stop = noop,
  .axis_discrete = noop,
/*
  // version 8
  .axis_value120 = noop,
  // version 9
  .axis_relative_direction = noop,
*/
};

static void zwp_relative_motion(void *data, struct zwp_relative_pointer_v1 *zwp_relative_pointer,
                                uint32_t utime_hi, uint32_t utime_lo,
                                wl_fixed_t dx, wl_fixed_t dy,
                                wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
  if (isGrabing && inputing) {
    LiSendMouseMoveEvent(wl_fixed_to_int(dx_unaccel), wl_fixed_to_int(dy_unaccel));
  }
}

static const struct zwp_relative_pointer_v1_listener zwp_relative_pointer_listener = {
  .relative_motion = zwp_relative_motion,
};

static void input_configure(void *data, struct wl_seat *seat, uint32_t capability){
  if (capability & WL_SEAT_CAPABILITY_POINTER) {
    wl_pointer = wl_seat_get_pointer(seat);
    if (wl_pointer) {
      wl_pointer_add_listener(wl_pointer, &wl_pointer_listener, NULL);
      if (zwp_relative_pointer_manager) {
        zwp_relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(zwp_relative_pointer_manager, wl_pointer);
        if (zwp_relative_pointer)
          zwp_relative_pointer_v1_add_listener(zwp_relative_pointer, &zwp_relative_pointer_listener, NULL);
      }
    }
  }
  if (capability & WL_SEAT_CAPABILITY_KEYBOARD) {
    wl_keyboard = wl_seat_get_keyboard(seat);
    if (wl_keyboard)
      wl_keyboard_add_listener(wl_keyboard, &wl_keyboard_listener, NULL);
  }
}

static const struct wl_seat_listener wl_seat_listener = {
  .capabilities = input_configure,
  .name = noop,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
  xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
  .ping = xdg_wm_base_ping,
};

static void registry_handler(void *data,struct wl_registry *registry, uint32_t id,
                             const char *interface,uint32_t version){
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 5);
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    xdg_wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 2);
    xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    wl_output = wl_registry_bind(registry, id, &wl_output_interface, 2);
    wl_output_add_listener(wl_output, &wl_output_listener, NULL);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    wl_seat = wl_registry_bind(registry, id, &wl_seat_interface, 5);
    wl_seat_add_listener(wl_seat, &wl_seat_listener, NULL);
  } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
    wp_viewporter = wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
  } else if (strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
    wlr_output_manager = wl_registry_bind(registry, id, &zwlr_output_manager_v1_interface, 1);
    zwlr_output_manager_v1_add_listener(wlr_output_manager, &wlr_output_manager_listener, NULL);
  } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
    zwp_pointer_constraints = wl_registry_bind(registry, id, &zwp_pointer_constraints_v1_interface, 1);
  } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
    zwp_relative_pointer_manager = wl_registry_bind(registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
  //} else if (strcmp(interface, wl_shm_interface.name) == 0) {
  //  wl_shm_p = wl_registry_bind(registry, id, &wl_shm_interface, 2);
  }
}

static void xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface,
                                         uint32_t serial) {
  xdg_surface_ack_configure(xdg_surface, serial);
  wl_surface_commit(wlsurface);
}

static void registry_remover(void *data, struct wl_registry *registry, uint32_t id) {};

static const struct wl_registry_listener registry_listener= {
  .global = registry_handler,
  .global_remove = registry_remover
};

static void window_close(void *data, struct xdg_toplevel *xdg_toplevel) {
  write(*window_op_fd_p, &quitCode, sizeof(char *));
}

static void window_configure(void *data,
                            struct xdg_toplevel *xdg_toplevel,
                            int32_t width, int32_t height,
                            struct wl_array *states) {
/*
  int *pos;
  wl_array_for_each(pos, states) {
  }
*/
  if (width != 0 && height != 0) {
    wp_viewport_set_destination(wp_viewport, width, height);
    display_width = width;
    display_height = height;
  }
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
  .configure = window_configure,
  .close = window_close,
};

static const struct xdg_surface_listener xdg_surface_listener = {
  .configure = xdg_surface_handle_configure,
};

static int wayland_setup(int width, int height, int drFlags) {

  if (!wl_display) {
    fprintf(stderr, "Error: failed to open WL display.\n");
    return -1;
  }

  registry = wl_display_get_registry(wl_display);
  wl_registry_add_listener(registry, &registry_listener, NULL);

  wl_display_dispatch(wl_display);
  wl_display_roundtrip(wl_display);
 
  isFullscreen = ((drFlags & DISPLAY_FULLSCREEN) == DISPLAY_FULLSCREEN);
  if (!isFullscreen && width > 0 && height > 0) {
    display_width = width;
    display_height = height;
  } else if (output_width > 0 && output_height > 0) {
    display_width = output_width;
    display_height = output_height;
    isFullscreen = true;
  }

  if (compositor == NULL || xdg_wm_base == NULL || wp_viewporter == NULL) {
    fprintf(stderr, "Can't find compositor or xdg_wm_base or wp_viewporter\n");
    return -1;
  }

  wlsurface = wl_compositor_create_surface(compositor);
  if (wlsurface == NULL) {
    fprintf(stderr, "Can't create surface\n");
    return -1;
  }

  wp_viewport = wp_viewporter_get_viewport(wp_viewporter, wlsurface);
  if (wp_viewport == NULL) {
    fprintf(stderr, "Can't create wp_viewport\n");
    return -1;
  }

  if (fractionalScale > 0 ) {
    scale_factor = fractionalScale;
  } else if (outputScaleFactor > 0) {
    scale_factor = outputScaleFactor;
  } else {
    fprintf(stderr, "Can't get scale from wayland server\n");
    return -1;
  }
  display_width = (int)display_width / scale_factor;
  display_height = (int)display_height / scale_factor;
  wp_viewport_set_destination(wp_viewport, display_width, display_height);
  wl_surface_commit(wlsurface);

  xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, wlsurface);
  xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
  if (xdg_surface == NULL || xdg_toplevel == NULL) {
    fprintf(stderr, "Can't create xdg surface or toplevel");
    return -1;
  }
  xdg_toplevel_set_app_id(xdg_toplevel, "moonlight");
  xdg_toplevel_set_title(xdg_toplevel, "moonlight");

  xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
  xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);

  if (isFullscreen)
    xdg_toplevel_set_fullscreen(xdg_toplevel, NULL);

  wl_window = wl_egl_window_create(wlsurface, (int)(display_width * scale_factor), (int)(display_height * scale_factor));
  if (wl_window == NULL) {
    fprintf(stderr, "Can't create wayland window");
    return -1;
  }

  return 0;
}

static void wl_setup_post(void *data) {
  struct _WINDOW_PROPERTIES *wp = data;

  int32_t size = *wp->configure & 0x00000000FFFFFFFF;
  int32_t offset = (*wp->configure & 0xFFFFFFFF00000000) >> 32;
  if (size != 0) {
    //XResizeWindow(display, window, size >> 16, size & 0x0000FFFF);
  }
  if (offset != 0) {
    //XMoveWindow(display, window, offset >> 16, offset & 0x0000FFFF);
  }

  window_op_fd_p = wp->fd_p;
  wl_surface_commit(wlsurface);
  wl_display_roundtrip(wl_display);
}

static void* wl_get_display(const char* *device) {
  if (wl_display == NULL)
    wl_display = wl_display_connect(NULL);

  if (wl_display)
    *device = "/dev/dri/renderD128";
  return wl_display;
}

static void wl_close_display(void *data) {
  struct _WINDOW_PROPERTIES *wp = data;
  *(wp->configure) = (((int64_t)offset_x) << 48) | (((int64_t)offset_y) << 32) | (((int64_t)display_width) << 16) | (int64_t)display_height;

  if (wl_display != NULL) {
    if (wl_output != NULL) {
      wl_output_release(wl_output);
      wl_output = NULL;
    }
    if (wlr_output_manager != NULL) {
      zwlr_output_manager_v1_destroy(wlr_output_manager);
      wlr_output_manager = NULL;
    }
    if (zwp_relative_pointer_manager != NULL) {
      zwp_relative_pointer_manager_v1_destroy(zwp_relative_pointer_manager);
      zwp_relative_pointer_manager = NULL;
      if (zwp_relative_pointer != NULL) {
        zwp_relative_pointer_v1_destroy(zwp_relative_pointer);
        zwp_relative_pointer = NULL;
      }
    }
    if (zwp_pointer_constraints != NULL) {
      if (zwp_locked_pointer) {
        zwp_locked_pointer_v1_destroy(zwp_locked_pointer);
        zwp_locked_pointer = NULL;
      }
      zwp_pointer_constraints_v1_destroy(zwp_pointer_constraints);
      zwp_pointer_constraints = NULL;
    }
    if (wl_pointer != NULL) {
      wl_pointer_release(wl_pointer);
      wl_pointer = NULL;
    }
    if (wl_keyboard != NULL) {
      wl_keyboard_release(wl_keyboard);
      wl_keyboard = NULL;
    }
    if (wl_seat != NULL) {
      wl_seat_release(wl_seat);
      wl_seat = NULL;
    }
    if (wl_window != NULL) {
      xdg_toplevel_destroy(xdg_toplevel);
      xdg_surface_destroy(xdg_surface);
      xdg_wm_base_destroy(xdg_wm_base);
      wp_viewport_destroy(wp_viewport);
      wp_viewporter_destroy(wp_viewporter);
      wl_surface_commit(wlsurface);
      wl_surface_destroy(wlsurface);
      wl_compositor_destroy(compositor);
      wl_registry_destroy(registry);
      wl_egl_window_destroy(wl_window);
    }
    wl_display_disconnect(wl_display);
    wl_display = NULL;
    wl_window = NULL;
  }
}

static int wl_dispatch_event(int width, int height, int index) {
  while(wl_display_prepare_read(wl_display) != 0)
    wl_display_dispatch_pending(wl_display);
  wl_display_flush(wl_display);
  wl_display_read_events(wl_display);
  wl_display_dispatch_pending(wl_display);

  return 0;
}

static void wl_get_resolution(int *width, int *height, bool isfullscreen) {
  if (isfullscreen) {
    *width = output_width;
    *height = output_height;
  }
  else {
    *width = (int)(display_width * scale_factor);
    *height = (int)(display_height * scale_factor);
  }

  return;
}

static void wl_change_cursor(struct WINDOW_OP *op, int flags) {
  if (flags & HIDE_CURSOR) {
    if (op->hide_cursor) {
      if (wl_pointer != NULL && pointerSerial != 0) {
        MV_CURSOR(last_x < 0 ? 0 : last_x, last_y < 0 ? 0 : last_y, last_x, last_y, wl_pointer, NULL, pointerSerial);
      }
    } else {
      // noting
    }
  }
  if (flags & INPUTING) {
    inputing = op->inputing;
    if (!inputing) {
      UNGRAB_WINDOW;
    }
  }

  return;
}

static void* wl_get_window() {
  return wl_window;
}

struct DISPLAY_CALLBACK display_callback_wayland = {
  .name = "wayland",
  .egl_platform = 0x31D8,
  .format = NOT_CARE,
  .hdr_support = false,
  .display_get_display = wl_get_display,
  .display_get_window = wl_get_window,
  .display_close_display = wl_close_display,
  .display_setup = wayland_setup,
  .display_setup_post = wl_setup_post,
  .display_put_to_screen = wl_dispatch_event,
  .display_get_resolution = wl_get_resolution,
  .display_modify_window = wl_change_cursor,
  .display_vsync_loop = NULL,
  .display_exported_buffer_info = NULL,
  .renders = EGL_RENDER,
};
#endif
