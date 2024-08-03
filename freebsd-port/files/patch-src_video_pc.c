--- src/video/pc.c.orig	2024-08-03 07:59:40 UTC
+++ src/video/pc.c
@@ -0,0 +1,402 @@
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
+#include <libavcodec/avcodec.h>
+#include <stdbool.h>
+#include <stdio.h>
+#include <stdlib.h>
+#include <string.h>
+#include <unistd.h>
+#include <fcntl.h>
+
+#include "egl.h"
+#include "ffmpeg.h"
+#ifdef HAVE_VAAPI
+#include "ffmpeg_vaapi.h"
+#endif
+#include "video.h"
+
+#ifdef HAVE_X11
+#include "../input/x11.h"
+#include "x11.h"
+#endif
+#ifdef HAVE_WAYLAND
+#include "wayland.h"
+#endif
+
+#include "../input/evdev.h"
+#include "../loop.h"
+#include "../util.h"
+
+#define X11_VDPAU_ACCELERATION ENABLE_HARDWARE_ACCELERATION_1
+#define X11_VAAPI_ACCELERATION ENABLE_HARDWARE_ACCELERATION_2
+#define SLICES_PER_FRAME 4
+
+
+enum WindowType windowType = 0;
+extern bool isUseGlExt;
+
+static bool isTenBit;
+static bool firstDraw = true;
+
+static void* ffmpeg_buffer = NULL;
+static size_t ffmpeg_buffer_size = 0;
+
+static void *display = NULL;
+static void *window = NULL;
+
+static int pipefd[2];
+static int windowpipefd[2];
+
+static int display_width = 0, display_height = 0;
+static int frame_width, frame_height, screen_width, screen_height;
+static const char *quitRequest = QUITCODE;
+
+typedef struct Setupargs {
+  int videoFormat;
+  int width;
+  int height;
+  int avc_flags;
+  int buffer_count;
+  int thread_count;
+  int drFlags;
+}SetupArgs;
+static SetupArgs ffmpegArgs;
+
+static int window_op_handle (int pipefd, void *data) {
+  char *opCode = NULL;
+
+  while (read(pipefd, &opCode, sizeof(char *)) > 0);
+  if (strcmp(opCode, QUITCODE) == 0) {
+    return LOOP_RETURN;
+#ifdef HAVE_WAYLAND
+  } else if (strcmp(opCode, GRABCODE) == 0) {
+    if (windowType & WAYLAND_WINDOW)
+      wl_change_cursor("hide");
+  } else if (strcmp(opCode, UNGRABCODE) == 0) {
+    if (windowType & WAYLAND_WINDOW)
+      wl_change_cursor("display");
+#endif
+  }
+
+  return LOOP_OK;
+}
+
+static int (*render_handler) (AVFrame* frame);
+
+static int frame_handle (int pipefd, void *data) {
+  AVFrame* frame = NULL;
+  while (read(pipefd, &frame, sizeof(void*)) > 0);
+  if (frame) {
+    return render_handler(frame);
+  }
+
+  return LOOP_OK;
+}
+
+static int software_draw (AVFrame* frame) {
+   if (firstDraw) {
+     firstDraw = false;
+     if (isYUV444 && (!(frame->linesize[0] == frame->linesize[2] && frame->linesize[1] == frame->linesize[0]))) {
+       fprintf(stderr, "There is not yuv444 format. Please try remove -yuv444 option to draw video!\n");
+       return LOOP_RETURN;
+     }
+   }
+  egl_draw(frame);
+  #ifdef HAVE_WAYLAND
+  if (windowType & WAYLAND_WINDOW)
+    wl_dispatch_event();
+  #endif
+  return LOOP_OK;
+}
+
+static int vaapi_egl_draw (AVFrame* frame) {
+#ifdef HAVE_VAAPI
+  egl_draw(frame);
+  #ifdef HAVE_WAYLAND
+  if (windowType & WAYLAND_WINDOW)
+    wl_dispatch_event();
+  #endif
+  return LOOP_OK;
+#endif
+  return LOOP_RETURN;
+}
+
+static int test_vaapi_egl_draw (AVFrame* frame) {
+   if (firstDraw) {
+     firstDraw = false;
+     if (isYUV444 && (!(frame->linesize[0] == frame->linesize[2] && frame->linesize[1] == frame->linesize[0]))) {
+       fprintf(stderr, "There is not yuv444 format. Please try remove -yuv444 option to draw video!\n");
+       return LOOP_RETURN;
+     }
+   }
+#ifdef HAVE_VAAPI
+  int dcFlag = 0;
+  egl_init(display, window, frame_width, frame_height, screen_width, screen_height, dcFlag);
+
+  if (!vaapi_can_export_surface_handle(isTenBit) ||
+      !vaapi_is_can_direct_render()) {
+    isUseGlExt = false;
+  }
+
+  if (isUseGlExt) {
+    render_handler = vaapi_egl_draw;
+  } else {
+  fprintf(stderr, "Render failed and Please try another platform!\n");
+    return LOOP_RETURN;
+  }
+  return LOOP_OK;
+#endif
+  return LOOP_RETURN;
+}
+
+int x11_init(bool vdpau, bool vaapi) {
+  windowType = getenv("WAYLAND_DISPLAY") != NULL ? WAYLAND_WINDOW : X11_WINDOW;
+  //windowType = getenv("WAYLAND_DISPLAY") != NULL ? WAYLAND_WINDOW : (getenv("DISPLAY") != NULL ? X11_WINDOW : GBM_WINDOW);
+  const char *displayDevice = getenv("DISPLAY");
+
+  if (vaapi) {
+  #ifdef HAVE_VAAPI
+    #ifdef HAVE_X11
+    if (!(windowType & WAYLAND_WINDOW))
+      x_muilti_threads();
+    #endif
+    if (vaapi_init_lib(NULL) != -1) {
+      isSupportYuv444 = vaapi_is_support_yuv444(0);
+      switch (windowType) {
+      case X11_WINDOW:
+      #ifdef HAVE_X11
+        display = x_get_display(displayDevice);
+      #endif
+        break;
+      case WAYLAND_WINDOW:
+      #ifdef HAVE_WAYLAND
+        display = wl_get_display(NULL);
+      #endif
+        break;
+      case GBM_WINDOW:
+        break;
+      }
+      return INIT_VAAPI;
+    }
+  #endif
+  }
+
+  // yuv444 is always supported by software decoder
+  isSupportYuv444 = true;
+  #ifdef HAVE_WAYLAND
+  if (windowType & WAYLAND_WINDOW) {
+    display = wl_get_display(NULL);
+    if (!display)
+      return 0;
+    else {
+      return INIT_EGL;
+    }
+  }
+  #endif
+  windowType = X11_WINDOW;
+  #ifdef HAVE_X11
+  x_muilti_threads();
+
+  display = x_get_display(displayDevice);
+  #endif
+  if (!display)
+    return 0;
+
+  return INIT_EGL;
+}
+
+int x11_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
+  ffmpegArgs.drFlags = drFlags;
+
+  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
+
+  if (!(windowType & WAYLAND_WINDOW)) {
+#ifdef HAVE_X11
+    if (x_setup(width, height, drFlags) == -1)
+      return -1;
+    x_get_resolution(&screen_width, &screen_height);
+    window = x_get_window();
+    printf("Based x11 window\n");
+#endif
+  }
+  else {
+#ifdef HAVE_WAYLAND
+    if (wayland_setup(width, height, drFlags) == -1)
+      return -1;
+    wl_get_resolution(&screen_width, &screen_height);
+    window = wl_get_window();
+    printf("Based wayland window\n");
+#endif
+  }
+  if (drFlags & DISPLAY_FULLSCREEN) {
+    display_width = screen_width;
+    display_height = screen_height;
+  } else {
+    display_width = width;
+    display_height = height;
+  }
+  frame_width = width;
+  frame_height = height;
+
+  int avc_flags;
+  if (drFlags & X11_VDPAU_ACCELERATION)
+    avc_flags = VDPAU_ACCELERATION;
+  else if (drFlags & X11_VAAPI_ACCELERATION)
+    avc_flags = VAAPI_ACCELERATION;
+  else
+    avc_flags = SLICE_THREADING;
+
+  if (ffmpeg_init(videoFormat, frame_width, frame_height, avc_flags, 2, SLICES_PER_FRAME) < 0) {
+    fprintf(stderr, "Couldn't initialize video decoding\n");
+    return -1;
+  }
+  ffmpegArgs.videoFormat = videoFormat;
+  ffmpegArgs.width = frame_width;
+  ffmpegArgs.height = frame_height;
+  ffmpegArgs.avc_flags = avc_flags;
+  ffmpegArgs.buffer_count = 2;
+  ffmpegArgs.thread_count = SLICES_PER_FRAME;
+
+  if (ffmpeg_decoder == SOFTWARE) {
+    int dcFlag = 0;
+    #define FULLSCREEN 0x08
+    if (drFlags & DISPLAY_FULLSCREEN)
+      dcFlag |= FULLSCREEN;
+    egl_init(display, window, frame_width, frame_height, screen_width, screen_height, dcFlag);
+    #undef FULLSCREEN
+    render_handler = software_draw;
+  } else {
+    render_handler = test_vaapi_egl_draw;
+  }
+  isTenBit = videoFormat & VIDEO_FORMAT_MASK_10BIT;
+
+  if (pipe(pipefd) == -1 || pipe(windowpipefd) == -1) {
+    fprintf(stderr, "Can't create communication channel between threads\n");
+    return -2;
+  }
+
+  loop_add_fd(pipefd[0], &frame_handle, EPOLLIN);
+  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
+
+  loop_add_fd(windowpipefd[0], &window_op_handle, EPOLLIN);
+  fcntl(windowpipefd[0], F_SETFL, O_NONBLOCK);
+
+  evdev_trans_op_fd(windowpipefd[1]);
+#ifdef HAVE_WAYLAND
+  if (windowType & WAYLAND_WINDOW) {
+    wl_trans_op_fd(windowpipefd[1]);
+    wl_setup_post();
+  }
+#endif
+
+  firstDraw = true;
+
+  return 0;
+}
+
+int x11_setup_vdpau(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
+  return x11_setup(videoFormat, width, height, redrawRate, context, drFlags | X11_VDPAU_ACCELERATION);
+}
+
+int x11_setup_vaapi(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
+  return x11_setup(videoFormat, width, height, redrawRate, context, drFlags | X11_VAAPI_ACCELERATION);
+}
+
+void x11_cleanup() {
+  egl_destroy();
+  ffmpeg_destroy();
+  if (windowType & WAYLAND_WINDOW) {
+  #ifdef HAVE_WAYLAND
+    wl_close_display();
+  #endif
+  }
+  else {
+  #ifdef HAVE_X11
+    x_close_display();
+  #endif
+  }
+}
+
+int x11_submit_decode_unit(PDECODE_UNIT decodeUnit) {
+  PLENTRY entry = decodeUnit->bufferList;
+  int length = 0;
+
+  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);
+
+  while (entry != NULL) {
+    memcpy(ffmpeg_buffer+length, entry->data, entry->length);
+    length += entry->length;
+    entry = entry->next;
+  }
+
+  int err = ffmpeg_decode(ffmpeg_buffer, length);
+
+  if (err < 0) {
+    if (ffmpeg_decoder == SOFTWARE) {
+      write(windowpipefd[1], &quitRequest, sizeof(char *));
+      return DR_NEED_IDR;
+    }
+#ifdef HAVE_VAAPI
+    #ifdef HAVE_X11
+    // try to change sw_format for gpu decoder
+    if (!(windowType & WAYLAND_WINDOW)) {
+      grab_window(false);
+      x11_input_remove();
+      x_close_display();
+    }
+    #endif
+    ffmpeg_destroy();
+    usleep(10000);
+    if (x11_init(true, true) <= INIT_EGL || 
+        (!(windowType & WAYLAND_WINDOW) && x_setup(ffmpegArgs.width, ffmpegArgs.height, ffmpegArgs.drFlags) <= -1) ||
+        ffmpeg_init(ffmpegArgs.videoFormat, ffmpegArgs.width, ffmpegArgs.height, ffmpegArgs.avc_flags, ffmpegArgs.buffer_count, ffmpegArgs.thread_count) < 0)
+#endif
+      write(windowpipefd[1], &quitRequest, sizeof(char *));
+    return DR_NEED_IDR;
+  }
+
+  AVFrame* frame = ffmpeg_get_frame(true);
+
+  if (frame != NULL)
+    write(pipefd[1], &frame, sizeof(void*));
+
+  return DR_OK;
+}
+
+DECODER_RENDERER_CALLBACKS decoder_callbacks_x11 = {
+  .setup = x11_setup,
+  .cleanup = x11_cleanup,
+  .submitDecodeUnit = x11_submit_decode_unit,
+  .capabilities = CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME) | CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC | CAPABILITY_DIRECT_SUBMIT,
+};
+
+DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vdpau = {
+  .setup = x11_setup_vdpau,
+  .cleanup = x11_cleanup,
+  .submitDecodeUnit = x11_submit_decode_unit,
+  .capabilities = CAPABILITY_DIRECT_SUBMIT,
+};
+
+DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vaapi = {
+  .setup = x11_setup_vaapi,
+  .cleanup = x11_cleanup,
+  .submitDecodeUnit = x11_submit_decode_unit,
+  .capabilities = CAPABILITY_DIRECT_SUBMIT,
+};
