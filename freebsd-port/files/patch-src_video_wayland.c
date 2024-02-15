--- src/video/wayland.c.orig	2024-02-15 11:36:00 UTC
+++ src/video/wayland.c
@@ -0,0 +1,270 @@
+/*
+ * This file is part of Moonlight Embedded.
+ *
+ * Copyright (C) 2017 Iwan Timmer
+ *
+ * Moonlight is free software; you can redistribute it and/or modify
+ * it under the terms of the GNU General Public License as published by
+ * the Free Software Foundation; either version 3 of the License, or
+ * (at your option) any later version.
+ *
+ * Moonlight is distributed in the hope that it will be useful,
+ * but WITHOUT ANY WARRANTY; without even the implied warranty of
+ * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
+ * GNU General Public License for more details.
+ *
+ * You should have received a copy of the GNU General Public License
+ * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
+ */
+
+#ifdef HAVE_WAYLAND
+#undef USE_X11
+#include <wayland-client.h>
+#include <wayland-egl.h>
+#include <EGL/egl.h>
+
+#include "wayland.h"
+#include "video.h"
+#include "xdg-shell-client-protocol.h"
+
+#include <stdbool.h>
+#include <stdio.h>
+#include <string.h>
+#include <unistd.h>
+
+static struct wl_display *wl_display = NULL;
+static struct wl_surface *wlsurface;
+static struct wl_egl_window *wl_window;
+static struct wl_compositor *compositor;
+static struct wl_registry *registry;
+static struct wl_output *wl_output;
+static struct wl_seat *wl_seat;
+static struct wl_pointer *wl_pointer;
+static struct xdg_wm_base *xdg_wm_base;
+static struct xdg_toplevel *xdg_toplevel;
+static struct xdg_surface *xdg_surface;
+
+static const char *quitCode = QUITCODE;
+
+static int display_width = 0;
+static int display_height = 0;
+static int window_op_fd = -1;
+static int32_t outputScaleFactor = 0;
+
+static void noop() {};
+
+static void wl_output_get_mode (void *data, struct wl_output *wl_output, uint32_t flags,
+                                int32_t width, int32_t height, int32_t refresh) {
+  display_width = width;
+  display_height = height;
+}
+
+static void wl_output_get_scale (void *data, struct wl_output *wl_output, int32_t factor) {
+  outputScaleFactor = factor;
+}
+
+static const struct wl_output_listener wl_output_listener = {
+  .geometry = noop,
+  .mode = wl_output_get_mode,
+  .done = noop,
+  .scale = wl_output_get_scale,
+  .name = noop,
+  .description = noop,
+};
+
+static void pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *pointer_surface, wl_fixed_t pointer_x, wl_fixed_t pointer_y) {
+  // NULL is hidden cursor
+  wl_pointer_set_cursor(wl_pointer, serial, NULL, 0, 0);
+}
+
+static void pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *wl_surface) {
+}
+
+static const struct wl_pointer_listener wl_pointer_listener = {
+  .enter = pointer_enter,
+  .leave = pointer_leave,
+  .motion = noop,
+  .button = noop,
+  .axis = noop,
+  .frame = noop,
+  .axis_source = noop,
+  .axis_stop = noop,
+  .axis_discrete = noop,
+/*
+  .axis_value120 = noop,
+  .axis_relative_direction = noop,
+*/
+};
+
+static void input_configure(void *data, struct wl_seat *seat, uint32_t capability){
+  if (capability & WL_SEAT_CAPABILITY_POINTER) {
+    wl_pointer = wl_seat_get_pointer(seat);
+    wl_pointer_add_listener(wl_pointer, &wl_pointer_listener, NULL);
+  }
+}
+
+static const struct wl_seat_listener wl_seat_listener = {
+  .capabilities = input_configure,
+  .name = noop,
+};
+
+static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
+  xdg_wm_base_pong(xdg_wm_base, serial);
+}
+
+static const struct xdg_wm_base_listener xdg_wm_base_listener = {
+  .ping = xdg_wm_base_ping,
+};
+
+static void registry_handler(void *data,struct wl_registry *registry, uint32_t id,
+                             const char *interface,uint32_t version){
+  if (strcmp(interface, wl_compositor_interface.name) == 0) {
+    compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 5);
+  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
+    xdg_wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 2);
+    xdg_wm_base_add_listener(xdg_wm_base, &xdg_wm_base_listener, NULL);
+  } else if (strcmp(interface, wl_output_interface.name) == 0) {
+    wl_output = wl_registry_bind(registry, id, &wl_output_interface, 4);
+    wl_output_add_listener(wl_output, &wl_output_listener, NULL);
+  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
+    wl_seat = wl_registry_bind(registry, id, &wl_seat_interface, 5);
+    wl_seat_add_listener(wl_seat, &wl_seat_listener, NULL);
+  }
+}
+
+static void xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface,
+                                         uint32_t serial) {
+  xdg_surface_ack_configure(xdg_surface, serial);
+  wl_surface_commit(wlsurface);
+}
+
+static void registry_remover(void *data, struct wl_registry *registry, uint32_t id) {};
+
+static const struct wl_registry_listener registry_listener= {
+  .global = registry_handler,
+  .global_remove = registry_remover
+};
+
+static void window_close(void *data, struct xdg_toplevel *xdg_toplevel) {
+  write(window_op_fd, &quitCode, sizeof(char *));
+}
+
+static const struct xdg_toplevel_listener xdg_toplevel_listener = {
+  .configure = noop,
+  .close = window_close,
+};
+
+static const struct xdg_surface_listener xdg_surface_listener = {
+  .configure = xdg_surface_handle_configure,
+};
+
+int wayland_setup(int width, int height, int drFlags) {
+
+  if (!wl_display) {
+    fprintf(stderr, "Error: failed to open WL display.\n");
+    return -1;
+  }
+
+  registry = wl_display_get_registry(wl_display);
+  wl_registry_add_listener(registry, &registry_listener, NULL);
+
+  wl_display_dispatch(wl_display);
+  wl_display_roundtrip(wl_display);
+ 
+  if (!(drFlags & DISPLAY_FULLSCREEN) || display_width <= 0 || display_height <= 0) {
+    display_width = width;
+    display_height = height;
+  }
+
+  if (compositor == NULL || xdg_wm_base == NULL) {
+    fprintf(stderr, "Can't find compositor or shell\n");
+    return -1;
+  }
+
+  wlsurface = wl_compositor_create_surface(compositor);
+  if (wlsurface == NULL) {
+    fprintf(stderr, "Can't create surface\n");
+    return -1;
+  }
+  if (outputScaleFactor > 0 ) {
+    wl_surface_commit(wlsurface);
+    wl_surface_set_buffer_scale(wlsurface, outputScaleFactor);
+  }
+
+  xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, wlsurface);
+  xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
+  if (xdg_surface == NULL || xdg_toplevel == NULL) {
+    fprintf(stderr, "Can't create xdg surface or toplevel");
+    return -1;
+  }
+
+  xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
+  xdg_surface_set_window_geometry(xdg_surface, 0, 0, display_width, display_height);
+  xdg_toplevel_add_listener(xdg_toplevel, &xdg_toplevel_listener, NULL);
+
+  xdg_toplevel_set_max_size(xdg_toplevel, display_width, display_height);
+  xdg_toplevel_set_fullscreen(xdg_toplevel, NULL);
+
+  wl_window = wl_egl_window_create(wlsurface, display_width, display_height);
+  if (wl_window == NULL) {
+    fprintf(stderr, "Can't create wayland window");
+    return -1;
+  }
+
+ return 0;
+}
+
+void wl_setup_post() {
+  wl_surface_commit(wlsurface);
+  wl_display_roundtrip(wl_display);
+}
+
+void* wl_get_display(const char *device) {
+  if (wl_display == NULL)
+    wl_display = wl_display_connect(NULL);
+
+  return wl_display;
+}
+
+void wl_close_display() {
+  if (wl_display != NULL) {
+    wl_output_destroy(wl_output);
+    wl_pointer_destroy(wl_pointer);
+    wl_seat_destroy(wl_seat);
+    xdg_toplevel_destroy(xdg_toplevel);
+    xdg_surface_destroy(xdg_surface);
+    xdg_wm_base_destroy(xdg_wm_base);
+    wl_surface_destroy(wlsurface);
+    wl_compositor_destroy(compositor);
+    wl_registry_destroy(registry);
+    wl_egl_window_destroy(wl_window);
+    wl_display_disconnect(wl_display);
+    wl_display = NULL;
+  }
+}
+
+void wl_dispatch_event() {
+  while(wl_display_prepare_read(wl_display) != 0)
+    wl_display_dispatch_pending(wl_display);
+  wl_display_flush(wl_display);
+  wl_display_read_events(wl_display);
+  wl_display_dispatch_pending(wl_display);
+}
+
+void wl_get_resolution(int *width, int *height) {
+  *width = display_width;
+  *height = display_height;
+}
+
+void wl_trans_op_fd(int fd) {
+  window_op_fd = fd;
+}
+
+EGLSurface wl_get_egl_surface(EGLDisplay display, EGLConfig config, void *data) {
+  return eglCreateWindowSurface(display, config, wl_window, data);
+}
+
+EGLDisplay wl_get_egl_display() {
+  return eglGetDisplay(wl_display);
+}
+#endif
