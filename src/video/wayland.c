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
#include "../input/evdev.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static struct wl_display *wl_display = NULL;
static struct wl_surface *wlsurface;
static struct wl_egl_window *wl_window;
static struct wl_compositor *compositor;
static struct wl_registry *registry;
static struct wl_output *wl_output = NULL;
static struct wl_seat *wl_seat = NULL;
static struct wl_pointer *wl_pointer = NULL;
static struct xdg_wm_base *xdg_wm_base;
static struct xdg_toplevel *xdg_toplevel;
static struct xdg_surface *xdg_surface;
static struct wp_viewporter *wp_viewporter = NULL;
static struct wp_viewport *wp_viewport = NULL;
static struct zwlr_output_manager_v1 *wlr_output_manager = NULL;

static const char *quitCode = QUITCODE;

static int display_width = 0, display_height = 0;
static int output_width = 0, output_height = 0;
static int window_op_fd = -1;
static int32_t outputScaleFactor = 0;
static uint32_t pointerSerial = 0;
static double fractionalScale = 0;
static bool isFullscreen = false;
static bool isGrabing = true;

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

static void pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *pointer_surface, wl_fixed_t pointer_x, wl_fixed_t pointer_y) {
  // NULL is hidden cursor
  if (isGrabing)
    wl_pointer_set_cursor(wl_pointer, serial, NULL, 0, 0);
  pointerSerial = serial;
  fake_grab_window(true);
}

static void pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *wl_surface) {
  fake_grab_window(false);
}

static const struct wl_pointer_listener wl_pointer_listener = {
  .enter = pointer_enter,
  .leave = pointer_leave,
  .motion = noop,
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

static void input_configure(void *data, struct wl_seat *seat, uint32_t capability){
  if (capability & WL_SEAT_CAPABILITY_POINTER) {
    wl_pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(wl_pointer, &wl_pointer_listener, NULL);
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
  write(window_op_fd, &quitCode, sizeof(char *));
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
  if (width != 0 && height != 0)
    wp_viewport_set_destination(wp_viewport, width, height);
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
    wp_viewport_set_destination(wp_viewport, (int)(display_width / fractionalScale), (int)(display_height / fractionalScale));
  } else if (outputScaleFactor > 0) {
    wp_viewport_set_destination(wp_viewport, (int)(display_width / outputScaleFactor), (int)(display_height / outputScaleFactor));
  }
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

  wl_window = wl_egl_window_create(wlsurface, display_width, display_height);
  if (wl_window == NULL) {
    fprintf(stderr, "Can't create wayland window");
    return -1;
  }

 return 0;
}

static void wl_setup_post(void *data) {
  window_op_fd = *((int *)data);
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

static void wl_close_display() {
  if (wl_display != NULL) {
    if (wl_output != NULL) {
      wl_output_release(wl_output);
      wl_output = NULL;
    }
    if (wlr_output_manager != NULL) {
      zwlr_output_manager_v1_destroy(wlr_output_manager);
      wlr_output_manager = NULL;
    }
    if (wl_pointer != NULL) {
      wl_pointer_release(wl_pointer);
      wl_pointer = NULL;
    }
    if (wl_seat != NULL) {
      wl_seat_release(wl_seat);
      wl_seat = NULL;
    }
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

static void wl_get_resolution(int *width, int *height) {
  *width = output_width;
  *height = output_height;
}

static void wl_change_cursor(const char *op) {
  if (strcmp(op, "hide") == 0) {
    isGrabing  = true;
    wl_pointer_set_cursor(wl_pointer, pointerSerial, NULL, 0, 0);
  } else {
    isGrabing  = false;
  }
}

static void* wl_get_window() {
  return wl_window;
}

struct DISPLAY_CALLBACK display_callback_wayland = {
  .name = "wayland",
  .egl_platform = 0x31D8,
  .format = NOT_CARE,
  .display_get_display = wl_get_display,
  .display_get_window = wl_get_window,
  .display_close_display = wl_close_display,
  .display_setup = wayland_setup,
  .display_setup_post = wl_setup_post,
  .display_put_to_screen = wl_dispatch_event,
  .display_get_resolution = wl_get_resolution,
  .display_change_cursor = wl_change_cursor,
  .display_vsync_loop = NULL,
  .renders = EGL_RENDER,
};
#endif
