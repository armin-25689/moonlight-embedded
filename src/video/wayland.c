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
#include "wp-colormanagement.h"
#include "wp-representation.h"
#include "wp-linuxdmabuf.h"
#include "wp-viewporter.h"
#include "wp-presentation-time.h"
#include "wp-fractional-scale.h"
#include "zwp-pointer-constraints.h"
#include "zwp-relative-pointer.h"
#include "../input/evdev.h"

#include "render.h"
#include "drm.h"
#include "ffmpeg.h"
#include "gbm.h"

#include <Limelight.h>

#include <libavutil/pixfmt.h>

#include <sys/mman.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
static struct wp_fractional_scale_manager_v1 *wp_fracscale = NULL;
static struct zwp_pointer_constraints_v1 *zwp_pointer_constraints = NULL;
static struct zwp_locked_pointer_v1 *zwp_locked_pointer = NULL;
static struct zwp_relative_pointer_manager_v1 *zwp_relative_pointer_manager = NULL;
static struct zwp_relative_pointer_v1 *zwp_relative_pointer = NULL;
// for render
#define COMMIT_TIME 2000000
struct _frame_callback_object {
  AVFrame *frame;
  struct Render_Image *image;
  struct wl_buffer *buffer;
};
struct _wl_render {
  struct wp_presentation *wp_presentation;
  struct wp_color_representation_manager_v1 *wp_color_representation;
  struct wp_color_representation_surface_v1 *wp_representation_surface;
  struct wp_color_manager_v1 *wp_color_manager;
  struct wp_color_management_surface_v1 *wp_color_surface;
  struct wp_color_management_output_v1 *wp_color_output;
  struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf;
  struct zwp_linux_dmabuf_feedback_v1 *feedback;
  struct _drm_buf drm_buf[MAX_FB_NUM];
  void *gbm_device;
  struct _frame_callback_object frame_callback_object[MAX_FB_NUM];
  struct {
    bool output_primaries_bt2020;
    bool set_luminances;
    bool set_primaries;
    bool bt2020;
    bool support;
  } hdr_support;
  struct {
    uint64_t refresh;
    uint64_t fps_ntime;
    struct timespec time_ns;
    uint64_t seq;
    bool done;
  } presentation;
  uint64_t size[MAX_PLANE_NUM];
  uint32_t *supported_format;
  int supported_format_count;
  int drm_fd;
  int dst_fmt;
  int plane_num;
  int lastrange;
  int lastcolorspace;
  int (*wl_set_hdr_metadata) (int index);
} static wl_render_base = {0};
struct _dm_table {
  uint32_t format;
  uint32_t unuse;
  uint64_t modifier;
};
static int wl_commit_loop(bool *exit, int width, int height, int index);
// render

static int offset_x = 0, offset_y = 0;
static int display_width = 0, display_height = 0, frame_width = 0, frame_height = 0;
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
static const char *render_device = "/dev/dri/renderD128";

static void noop() {};
static int noop_int() { return 0; };

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

static void wl_get_color_features (void *data, struct wp_color_manager_v1 *wp_mana, uint32_t features) {
  switch (features) {
  case WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES:
    wl_render_base.hdr_support.set_luminances = true;
    break;
  case WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES:
    wl_render_base.hdr_support.set_primaries = true;
    break;
  }

  return;
}

static void wl_get_primaries_support (void *data, struct wp_color_manager_v1 *wp_mana, uint32_t primaries) {
  switch (primaries) {
  case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020:
    wl_render_base.hdr_support.bt2020 = true;
    break;
  }
}

static const struct wp_color_manager_v1_listener wp_color_manager_listener = {
  .supported_intent = noop,
  .supported_feature = wl_get_color_features,
  .supported_tf_named = noop,
  .supported_primaries_named = wl_get_primaries_support,
  .done = noop,
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
  } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
    zwp_pointer_constraints = wl_registry_bind(registry, id, &zwp_pointer_constraints_v1_interface, 1);
  } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
    zwp_relative_pointer_manager = wl_registry_bind(registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
  } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
    wp_fracscale = wl_registry_bind(registry, id, &wp_fractional_scale_manager_v1_interface, 1);
  } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
    wl_render_base.zwp_linux_dmabuf = wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 4);
  } else if (strcmp(interface, wp_color_manager_v1_interface.name) == 0) {
    wl_render_base.wp_color_manager = wl_registry_bind(registry, id, &wp_color_manager_v1_interface, 1);
    wp_color_manager_v1_add_listener(wl_render_base.wp_color_manager, &wp_color_manager_listener, NULL);
  } else if (strcmp(interface, wp_color_representation_manager_v1_interface.name) == 0) {
    wl_render_base.wp_color_representation = wl_registry_bind(registry, id, &wp_color_representation_manager_v1_interface, 1);
  } else if (strcmp(interface, wp_presentation_interface.name) == 0) {
    wl_render_base.wp_presentation = wl_registry_bind(registry, id, &wp_presentation_interface, 2);
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
  evwcode quitCode = QUITCODE;
  write(*window_op_fd_p, &quitCode, sizeof(quitCode));
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

static void get_surface_scale (void *data, struct wp_fractional_scale_v1 *wp_fractional_scale_v1,
                              uint32_t scale) {
  fractionalScale = scale / 120.0;
  wp_fractional_scale_v1_destroy(wp_fractional_scale_v1);
  display_width = display_width * scale_factor / fractionalScale;
  display_height = display_height * scale_factor / fractionalScale;
  wp_viewport_set_destination(wp_viewport, display_width, display_height);
  scale_factor = fractionalScale;
  return;
}

static const struct wp_fractional_scale_v1_listener wp_fracscale_listener = {
  .preferred_scale = get_surface_scale,
};

static int wayland_setup(int width, int height, int fps, int drFlags) {

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
  frame_width = width;
  frame_height = height;
  uint64_t fps_time = 1000000000 / fps;
  wl_render_base.presentation.fps_ntime = fps_time;

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

  if (wp_fracscale) {
    struct wp_fractional_scale_v1 *wp_fscale = wp_fractional_scale_manager_v1_get_fractional_scale(wp_fracscale, wlsurface);
    if (wp_fscale) {
      wp_fractional_scale_v1_add_listener(wp_fscale, &wp_fracscale_listener, NULL);
      wl_surface_commit(wlsurface);
      wl_display_dispatch(wl_display);
      wl_display_roundtrip(wl_display);
    }
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

  if (drFlags & WAYLAND_RENDER) {
    display_callback_wayland.display_vsync_loop = &wl_commit_loop;
    wl_render_base.hdr_support.support = wl_render_base.hdr_support.set_luminances && wl_render_base.hdr_support.set_primaries && wl_render_base.hdr_support.bt2020;
    display_callback_wayland.hdr_support = wl_render_base.hdr_support.support;
  } else {
    if (wantHdr)
      printf("WARNING: NO HDR support!\n");
    wl_window = wl_egl_window_create(wlsurface, (int)(display_width * scale_factor), (int)(display_height * scale_factor));
    if (wl_window == NULL) {
      fprintf(stderr, "Can't create wayland window");
      return -1;
    }
    display_callback_wayland.hdr_support = false;
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
    *device = render_device;
  return wl_display;
}

static void wl_close_display(void *data) {
  struct _WINDOW_PROPERTIES *wp = data;
  *(wp->configure) = (((int64_t)offset_x) << 48) | (((int64_t)offset_y) << 32) | (((int64_t)display_width) << 16) | (int64_t)display_height;

  if (wl_display != NULL) {
    if (wl_render_base.wp_presentation)
      wp_presentation_destroy(wl_render_base.wp_presentation);
    if (wl_render_base.wp_color_surface)
      wp_color_management_surface_v1_destroy(wl_render_base.wp_color_surface);
    if (wl_render_base.wp_color_output)
      wp_color_management_output_v1_destroy(wl_render_base.wp_color_output);
    if (wl_render_base.wp_representation_surface)
      wp_color_representation_surface_v1_destroy(wl_render_base.wp_representation_surface);
    if (wl_render_base.wp_color_manager)
      wp_color_manager_v1_destroy(wl_render_base.wp_color_manager);
    if (wl_render_base.wp_color_representation)
      wp_color_representation_manager_v1_destroy(wl_render_base.wp_color_representation);
    if (wl_render_base.feedback)
      zwp_linux_dmabuf_feedback_v1_destroy(wl_render_base.feedback);
    if (wl_render_base.zwp_linux_dmabuf)
      zwp_linux_dmabuf_v1_destroy(wl_render_base.zwp_linux_dmabuf);

    if (wp_fracscale) {
      wp_fractional_scale_manager_v1_destroy(wp_fracscale);
      wp_fracscale = NULL;
    }
    if (wl_output != NULL) {
      wl_output_release(wl_output);
      wl_output = NULL;
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
  .hdr_support = true,
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
  .renders = EGL_RENDER | WAYLAND_RENDER,
};

#if defined(HAVE_DRM)
static int set_hdr_static(int index) {

  static bool hdr_active = false;
  bool last_stat = hdr_active;
  SS_HDR_METADATA sunshineHdrMetadata = {0};
  if (!LiGetHdrMetadata(&sunshineHdrMetadata)) {
    hdr_active = false;
  }
  else {
    hdr_active = true;
  }

  if (wl_render_base.lastcolorspace >= 0) {
    int colorspace = ffmpeg_get_frame_colorspace(wl_render_base.frame_callback_object[index].image->sframe.frame);
    if (colorspace != wl_render_base.lastcolorspace) {
      uint32_t color_space = colorspace == COLORSPACE_REC_2020 ? WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020 : (colorspace == COLORSPACE_REC_709 ? WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709 : WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601);
      wp_color_representation_surface_v1_set_coefficients_and_range(wl_render_base.wp_representation_surface, color_space, wl_render_base.lastrange);
      wl_render_base.lastcolorspace = colorspace;
    }
  }

  if (last_stat == hdr_active)
    return 0;

  if (hdr_active == false) {
    wp_color_management_surface_v1_unset_image_description(wl_render_base.wp_color_surface);
    return 0;
  }

  struct wp_image_description_creator_params_v1 *creator = wp_color_manager_v1_create_parametric_creator(wl_render_base.wp_color_manager);
  if (!creator) {
    fprintf(stderr, "Cannot create params creator. \n");
    return -1;
  }

  wp_image_description_creator_params_v1_set_primaries_named(creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
  wp_image_description_creator_params_v1_set_tf_named(creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
  wp_image_description_creator_params_v1_set_mastering_luminance(creator, sunshineHdrMetadata.minDisplayLuminance, sunshineHdrMetadata.maxDisplayLuminance);
  wp_image_description_creator_params_v1_set_mastering_display_primaries(creator, sunshineHdrMetadata.displayPrimaries[0].x, sunshineHdrMetadata.displayPrimaries[0].y, sunshineHdrMetadata.displayPrimaries[1].x, sunshineHdrMetadata.displayPrimaries[1].y, sunshineHdrMetadata.displayPrimaries[2].x, sunshineHdrMetadata.displayPrimaries[2].y, sunshineHdrMetadata.whitePoint.x, sunshineHdrMetadata.whitePoint.y);
  wp_image_description_creator_params_v1_set_max_cll(creator, sunshineHdrMetadata.maxContentLightLevel);
  wp_image_description_creator_params_v1_set_max_fall(creator, sunshineHdrMetadata.maxFrameAverageLightLevel);

  struct wp_image_description_v1 *descriptor = wp_image_description_creator_params_v1_create(creator);
  if (!descriptor) {
    fprintf(stderr, "wl: cannot set hdr metadata.\n");
    return -1;
  }

  wp_color_management_surface_v1_set_image_description(wl_render_base.wp_color_surface, descriptor, 0);

  wp_image_description_v1_destroy(descriptor);

  return 0;
}

static int wl_render_create(struct Render_Init_Info *paras) { 
  wl_render_base.drm_fd = -1;

  wl_render_base.gbm_device = gbm_get_display(&wl_render_base.drm_fd);
  if (wl_render_base.drm_fd < 0) {
    fprintf(stderr, "Could not open render device: %s.\n", render_device);
    return -1;
  }

  return 0;
}

static inline int commit_surface(int index) {
  struct wl_buffer *buffer = wl_render_base.frame_callback_object[index].buffer;
  if (buffer == NULL) {
    fprintf(stderr, "Invalid buffer.\n");
    return -1;
  }

  wl_render_base.wl_set_hdr_metadata(index);

  wl_surface_attach(wlsurface, buffer, 0, 0);
  wl_surface_damage_buffer(wlsurface, 0, 0, frame_width, frame_height);

  wl_surface_commit(wlsurface);

  return index;
}

static void get_drm_format (void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback, int32_t fd, uint32_t size) {
  void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map == MAP_FAILED) return;
  struct _dm_table *drm_fmt = map;
  int count = size / sizeof(*drm_fmt);
  if (wl_render_base.supported_format) free(wl_render_base.supported_format);
  wl_render_base.supported_format = calloc(count, sizeof(uint32_t));
  for (int i = 0; i < count; i++) {
    wl_render_base.supported_format[i] = drm_fmt[i].format;
  }
  wl_render_base.supported_format_count = count;

  munmap(map, size);
  close(fd);
}

static const struct zwp_linux_dmabuf_feedback_v1_listener feedback_listener = {
  .done = noop,
  .format_table = get_drm_format,
  .main_device = noop,
  .tranche_done = noop,
  .tranche_target_device = noop,
  .tranche_formats = noop,
  .tranche_flags = noop,
};

static void get_primaries_named(void *data, struct wp_image_description_info_v1 *info, uint32_t named) {
  if (named == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) wl_render_base.hdr_support.output_primaries_bt2020 = true;
}

static const struct wp_image_description_info_v1_listener output_description_info_listener = {
  .done = noop, .icc_file = noop, .primaries = noop,
  .primaries_named = get_primaries_named, .tf_power = noop, .tf_named = noop, .luminances = noop,
  .target_primaries = noop, .target_luminance = noop, .target_max_cll = noop, .target_max_fall = noop,
};

static void surface_presentation (void *data, struct wp_presentation_feedback *wp_presentation_feedback,
                                  uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec, uint32_t refresh,
                                  uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
  uint64_t seq = ((uint64_t)seq_hi << 32) | seq_lo;
  uint64_t now_tv_sec = ((uint64_t) tv_sec_hi << 32) | tv_sec_lo;
  if ((wl_render_base.presentation.seq - seq) == 1) {
    wl_render_base.presentation.fps_ntime = (tv_nsec - wl_render_base.presentation.time_ns.tv_nsec) + ((now_tv_sec - wl_render_base.presentation.time_ns.tv_sec) * 1000000000LL);
    wl_render_base.presentation.done = true;
  }
  wl_render_base.presentation.refresh = refresh;
  wl_render_base.presentation.time_ns.tv_sec =  now_tv_sec;
  wl_render_base.presentation.time_ns.tv_nsec =  tv_nsec;
  wl_render_base.presentation.seq  = seq;
  wp_presentation_feedback_destroy(wp_presentation_feedback);
  return;
}

static void surface_discarded (void *data, struct wp_presentation_feedback *wp_feedback) {
  wp_presentation_feedback_destroy(wp_feedback);
  return;
}

static const struct wp_presentation_feedback_listener presentation_feedback = {
  .presented = surface_presentation,
  .discarded = surface_discarded,
  .sync_output = noop,
};

static void inline wait_to_commit() {
  struct timespec now;

  if (wl_render_base.presentation.done) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t interval = (now.tv_sec - wl_render_base.presentation.time_ns.tv_sec) * 1000000000LL + (now.tv_nsec - wl_render_base.presentation.time_ns.tv_nsec);
    uint64_t rem = interval % wl_render_base.presentation.fps_ntime;
    // 2ms to left
    uint64_t commit = wl_render_base.presentation.refresh - COMMIT_TIME;
    uint64_t wait_time = commit < rem ? (wl_render_base.presentation.fps_ntime - rem + commit) : commit - rem;
    wl_render_base.presentation.done = false;

    now.tv_sec = 0;
    now.tv_nsec = wait_time;
  }
  else {
    now.tv_sec = 0;
    now.tv_nsec = wl_render_base.presentation.fps_ntime;
  }

  nanosleep(&now, NULL);

  return;
}

static int wl_commit_loop(bool *exit, int width, int height, int index) {
  static uint32_t time = 0;
  struct wp_presentation_feedback *pr = NULL;

  time++;

  int ret = commit_surface(index);
  if (ret < 0)
    return -1;

  wl_dispatch_event(width, height, index);
  wait_to_commit();

  switch (time) {
  case 120:
    time = 0;
    break;
  case 1:
  case 2:
  case 3:
  case 4:
    pr = wp_presentation_feedback(wl_render_base.wp_presentation, wlsurface);
    wp_presentation_feedback_add_listener(pr, &presentation_feedback, NULL);
    break;
  }

  return ret;
}

static int wl_render_init(struct Render_Init_Info *paras) { 
  if (wl_render_base.wp_color_representation == NULL || wl_render_base.zwp_linux_dmabuf == NULL || wl_render_base.wp_presentation == NULL) {
    fprintf(stderr, "wl: color_representation/linux_dmabuf/presentation could not supported.\n");
    return -1;
  }
  wl_render_base.wp_representation_surface = wp_color_representation_manager_v1_get_surface(wl_render_base.wp_color_representation, wlsurface);
  if (wl_render_base.wp_representation_surface == NULL) {
    fprintf(stderr, "wl: color_surface/representation_surface can not supported.\n");
    return -1;
  }

  if (wl_render_base.wp_color_manager) {
    wl_render_base.wp_color_surface = wp_color_manager_v1_get_surface(wl_render_base.wp_color_manager, wlsurface);
  }
  if (!wl_render_base.wp_color_surface || !display_callback_wayland.hdr_support) {
    if (useHdr) {
      fprintf(stderr, "wl: cannot get color manager surface ,please remove -hdr options.\n");
      display_callback_wayland.hdr_support = false;
      return -1;
    }
  }
  if (useHdr) {
    wl_render_base.wp_color_output = wp_color_manager_v1_get_output(wl_render_base.wp_color_manager, wl_output);
    struct wp_image_description_v1 *output_description = wp_color_management_output_v1_get_image_description(wl_render_base.wp_color_output);
    struct wp_image_description_info_v1 *output_description_info = wp_image_description_v1_get_information(output_description);
    wp_image_description_info_v1_add_listener(output_description_info, &output_description_info_listener, NULL);
    wp_image_description_v1_destroy(output_description);
    wl_render_base.wl_set_hdr_metadata = &set_hdr_static;
  } else
    wl_render_base.wl_set_hdr_metadata = &noop_int;

  wl_render_base.feedback = zwp_linux_dmabuf_v1_get_surface_feedback(wl_render_base.zwp_linux_dmabuf, wlsurface);
  zwp_linux_dmabuf_feedback_v1_add_listener(wl_render_base.feedback, &feedback_listener, NULL);
  wl_surface_commit(wlsurface);
  wl_display_roundtrip(wl_display);

  if (useHdr && !wl_render_base.hdr_support.output_primaries_bt2020)
    fprintf(stderr, "WARNNING: wayland output is not BT2020 primaries! \n");

  return 0; 
}

static void wl_render_destroy() {

  if (wl_render_base.supported_format)
    free(wl_render_base.supported_format);

  for (int i = 0; i < MAX_FB_NUM; i++) {
    if (wl_render_base.frame_callback_object[i].buffer) {
      wl_buffer_destroy(wl_render_base.frame_callback_object[i].buffer);
      wl_render_base.frame_callback_object[i].buffer = NULL;
    }
  }

  if (wl_render_base.drm_fd >= 0 )
    gbm_close_display (wl_render_base.drm_fd, wl_render_base.drm_buf, MAX_FB_NUM, wl_render_base.gbm_device, NULL);

  memset(&wl_render_base, 0, sizeof(wl_render_base));
  wl_render_base.drm_fd = -1;
}

static inline struct wl_buffer *wl_import_dmabuf(struct _drm_buf *drm_buf) {
  struct wl_buffer *buffer = NULL;

  struct zwp_linux_buffer_params_v1 *creator = zwp_linux_dmabuf_v1_create_params(wl_render_base.zwp_linux_dmabuf);
  if (!creator) {
    fprintf(stderr, "Create linux dmabuf creator failed.\n");
    return NULL;
  }
  for (int i = 0; i < wl_render_base.plane_num; i++) {
    zwp_linux_buffer_params_v1_add(creator, drm_buf->fd[i], i, drm_buf->offset[i], drm_buf->pitch[i], drm_buf->modifiers[i] >> 32, drm_buf->modifiers[i] & 0xFFFFFFFF);
  }

  buffer = zwp_linux_buffer_params_v1_create_immed(creator, drm_buf->width[0], drm_buf->height[0], drm_buf->format[0], 0);
  zwp_linux_buffer_params_v1_destroy(creator);
  if (!buffer) {
    fprintf(stderr, "Create wayland linux dmabuf failed.\n");
    return NULL;
  }

  return buffer;
}

static inline int store_objects(int index) {
  struct wl_buffer *buffer = wl_import_dmabuf(&wl_render_base.drm_buf[index]);
  if (buffer == NULL) {
    fprintf(stderr, "Create wayland linux dmabuf failed.\n");
    return -1;
  }
  wl_render_base.frame_callback_object[index].buffer = buffer;

  return 0;
}

static int wl_sync_frame_config(struct Render_Config *config) {
  int dst_fmt = -1;
  bool need_change_color_config = false;
  bool need_generate_buffer = false;
  bool full_color_range = config->full_color_range;
  int colorspace = config->color_space;

  switch (config->pix_fmt) {
  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUVJ444P:
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P:
    need_generate_buffer = true;
    dst_fmt = AV_PIX_FMT_BGR0;
    break;
  case AV_PIX_FMT_YUV444P10:
  case AV_PIX_FMT_YUV420P10:
    need_generate_buffer = true;
    dst_fmt = AV_PIX_FMT_X2RGB10LE;
    break;
  case AV_PIX_FMT_VUYX:
  case AV_PIX_FMT_XV30:
  case AV_PIX_FMT_NV12:
  case AV_PIX_FMT_P010:
    dst_fmt = config->pix_fmt;
    need_change_color_config = true;
    break;
  }

  wl_render_base.dst_fmt = dst_fmt;
  
  wl_render_base.lastcolorspace = -1;
  if (need_change_color_config) {
    uint32_t color_space = colorspace == COLORSPACE_REC_2020 ? WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020 : (colorspace == COLORSPACE_REC_709 ? WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709 : WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601);
    uint32_t range = full_color_range ? WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL : WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED;
    wl_render_base.lastrange = range;
    wl_render_base.lastcolorspace = colorspace;
    wp_color_representation_surface_v1_set_coefficients_and_range(wl_render_base.wp_representation_surface, color_space, range);
  }

  if (need_generate_buffer) {
    wl_render_base.plane_num = generate_gbm_bo(wl_render_base.drm_fd, wl_render_base.drm_buf, MAX_FB_NUM, wl_render_base.gbm_device, frame_width, frame_height, dst_fmt, wl_render_base.size);
    if (wl_render_base.plane_num < 1) {
      fprintf(stderr, "Could not generate drm buf.\n");
      return -1;
    }
    for (int buffer = 0; buffer < MAX_FB_NUM; buffer++) {
      if (store_objects(buffer) < 0)
        return -1;
    }
  }

  if (wl_render_base.supported_format) {
    int found = -1;
    for (int p = 0; p < wl_render_base.supported_format_count; p++) {
      if (wl_render_base.supported_format[p] == wl_render_base.drm_buf[0].format[0]) found = p;
    }
    if (found < 0) {
      fprintf(stderr, "ERROR: wayland linux dmabuf not support the drm format: %.4s.\n", (char *)&wl_render_base.drm_buf[0].format[0]);
      return -1;
    }
  }

  return 0;
}

static int wl_import_image(struct Source_Buffer_Info *buffer, int planes, int composeOrSeperate, void* *image, int index) {
  wl_render_base.plane_num = planes;
  int geted_index = drm_import_hw_buffer (wl_render_base.drm_fd, wl_render_base.drm_buf, buffer, planes, composeOrSeperate, image, index);

  if (wl_render_base.frame_callback_object[index].buffer)
    wl_buffer_destroy(wl_render_base.frame_callback_object[index].buffer);
  wl_render_base.frame_callback_object[index].buffer = NULL;
  if (store_objects(index) < 0)
    return -1;

  return geted_index;
}

static void wl_free_image(void* *image, int planes) {};

static int wl_draw(struct Render_Image *image) {
  int ret = -1;
  const int handle_num = 1;
  uint64_t map_offset[MAX_PLANE_NUM] = {0};

  if (ffmpeg_decoder == SOFTWARE) {
    ret = gbm_convert_image(image, wl_render_base.drm_buf, wl_render_base.drm_buf[image->index].fd[0], handle_num, wl_render_base.plane_num, wl_render_base.dst_fmt, wl_render_base.size, map_offset);
  } else {
    ret = image->index;
  }

  if (ret < 0) {
    fprintf(stderr, "wl draw failed.");
    return ret;
  }

  if (wl_render_base.frame_callback_object[ret].image != image) {
    wl_render_base.frame_callback_object[ret].frame = image->sframe.frame;
    wl_render_base.frame_callback_object[ret].image = image;
  }

  return ret;
}

struct RENDER_CALLBACK wayland_render = {
  .name = "wayland",
  .display_name = "wayland",
  .is_hardaccel_support = true,
  .render_type = WAYLAND_RENDER,
  .decoder_type = SOFTWARE,
  .data = NULL,
  .render_create = wl_render_create,
  .render_init = wl_render_init,
  .render_sync_config = wl_sync_frame_config,
  .render_draw = wl_draw,
  .render_destroy = wl_render_destroy,
  .render_sync_window_size = NULL,
  .render_map_buffer = wl_import_image,
  .render_unmap_buffer = wl_free_image,
};
#endif
#endif
