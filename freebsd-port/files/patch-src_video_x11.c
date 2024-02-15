--- src/video/x11.c.orig	2023-11-03 06:08:34 UTC
+++ src/video/x11.c
@@ -17,74 +17,75 @@
  * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
  */
 
+
+#include <X11/Xatom.h>
+#include <X11/Xutil.h>
+
+#include "../input/x11.h"
+#ifndef USE_X11
+#define USE_X11 1
+#endif
 #include "video.h"
-#include "egl.h"
 #include "ffmpeg.h"
 #ifdef HAVE_VAAPI
 #include "ffmpeg_vaapi.h"
 #endif
 
-#include "../input/x11.h"
-#include "../loop.h"
-#include "../util.h"
+#include "x11.h"
 
-#include <X11/Xatom.h>
-#include <X11/Xutil.h>
-
 #include <stdbool.h>
 #include <stdio.h>
 #include <string.h>
-#include <unistd.h>
-#include <fcntl.h>
-#include <poll.h>
 
-#define X11_VDPAU_ACCELERATION ENABLE_HARDWARE_ACCELERATION_1
-#define X11_VAAPI_ACCELERATION ENABLE_HARDWARE_ACCELERATION_2
-#define SLICES_PER_FRAME 4
-
-static void* ffmpeg_buffer = NULL;
-static size_t ffmpeg_buffer_size = 0;
-
 static Display *display = NULL;
 static Window window;
 
-static int pipefd[2];
-
 static int display_width;
 static int display_height;
 
-static int frame_handle(int pipefd) {
-  AVFrame* frame = NULL;
-  while (read(pipefd, &frame, sizeof(void*)) > 0);
-  if (frame) {
-    if (ffmpeg_decoder == SOFTWARE)
-      egl_draw(frame->data);
-    #ifdef HAVE_VAAPI
-    else if (ffmpeg_decoder == VAAPI)
-      vaapi_queue(frame, window, display_width, display_height);
-    #endif
-  }
+static bool startedMuiltiThreads = false;
 
-  return LOOP_OK;
+void x_vaapi_draw(AVFrame* frame, int width, int height) {
+  #ifdef HAVE_VAAPI
+  return vaapi_queue(frame, window, width, height);
+  #endif
 }
 
-int x11_init(bool vdpau, bool vaapi) {
-  XInitThreads();
-  display = XOpenDisplay(NULL);
-  if (!display)
-    return 0;
-
+bool x_test_vaapi_draw(AVFrame* frame, int width, int height) {
   #ifdef HAVE_VAAPI
-  if (vaapi && vaapi_init_lib(display) == 0)
-    return INIT_VAAPI;
+  return test_vaapi_queue(frame, window, width, height);
   #endif
+}
 
-  return INIT_EGL;
+void* x_get_display(const char *device) {
+  if (display == NULL) {
+    display = (Display *) get_display_from_vaapi(true);
+    if (display == NULL)
+      display = XOpenDisplay(device);
+  }
+
+  return display;
 }
 
-int x11_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
-  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
+void x_close_display() {
+  if (display != NULL)
+    XCloseDisplay(display);
+}
 
+void x_muilti_threads() {
+  if (!startedMuiltiThreads) {
+    XInitThreads();
+    startedMuiltiThreads = true;
+  }
+}
+
+void x_get_resolution (int *width, int *height) {
+  *width = display_width;
+  *height = display_height;
+}
+
+int x_setup(int width, int height, int drFlags) {
+
   if (!display) {
     fprintf(stderr, "Error: failed to open X display.\n");
     return -1;
@@ -100,7 +101,7 @@ int x11_setup(int videoFormat, int width, int height, 
   }
 
   Window root = DefaultRootWindow(display);
-  XSetWindowAttributes winattr = { .event_mask = PointerMotionMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask };
+  XSetWindowAttributes winattr = { .event_mask = FocusChangeMask };
   window = XCreateWindow(display, root, 0, 0, display_width, display_height, 0, CopyFromParent, InputOutput, CopyFromParent, CWEventMask, &winattr);
   XMapWindow(display, window);
   XStoreName(display, window, "Moonlight");
@@ -122,85 +123,15 @@ int x11_setup(int videoFormat, int width, int height, 
   }
   XFlush(display);
 
-  int avc_flags;
-  if (drFlags & X11_VDPAU_ACCELERATION)
-    avc_flags = VDPAU_ACCELERATION;
-  else if (drFlags & X11_VAAPI_ACCELERATION)
-    avc_flags = VAAPI_ACCELERATION;
-  else
-    avc_flags = SLICE_THREADING;
-
-  if (ffmpeg_init(videoFormat, width, height, avc_flags, 2, SLICES_PER_FRAME) < 0) {
-    fprintf(stderr, "Couldn't initialize video decoding\n");
-    return -1;
-  }
-
-  if (ffmpeg_decoder == SOFTWARE)
-    egl_init(display, window, width, height);
-
-  if (pipe(pipefd) == -1) {
-    fprintf(stderr, "Can't create communication channel between threads\n");
-    return -2;
-  }
-  loop_add_fd(pipefd[0], &frame_handle, POLLIN);
-  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
-
   x11_input_init(display, window);
 
   return 0;
 }
 
-int x11_setup_vdpau(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
-  return x11_setup(videoFormat, width, height, redrawRate, context, drFlags | X11_VDPAU_ACCELERATION);
+EGLSurface x_get_egl_surface(EGLDisplay display, EGLConfig config, void *data) {
+  return eglCreateWindowSurface(display, config, window, data);
 }
 
-int x11_setup_vaapi(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
-  return x11_setup(videoFormat, width, height, redrawRate, context, drFlags | X11_VAAPI_ACCELERATION);
+EGLDisplay x_get_egl_display() {
+  return eglGetDisplay(display);
 }
-
-void x11_cleanup() {
-  ffmpeg_destroy();
-  egl_destroy();
-}
-
-int x11_submit_decode_unit(PDECODE_UNIT decodeUnit) {
-  PLENTRY entry = decodeUnit->bufferList;
-  int length = 0;
-
-  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);
-
-  while (entry != NULL) {
-    memcpy(ffmpeg_buffer+length, entry->data, entry->length);
-    length += entry->length;
-    entry = entry->next;
-  }
-
-  ffmpeg_decode(ffmpeg_buffer, length);
-
-  AVFrame* frame = ffmpeg_get_frame(true);
-  if (frame != NULL)
-    write(pipefd[1], &frame, sizeof(void*));
-
-  return DR_OK;
-}
-
-DECODER_RENDERER_CALLBACKS decoder_callbacks_x11 = {
-  .setup = x11_setup,
-  .cleanup = x11_cleanup,
-  .submitDecodeUnit = x11_submit_decode_unit,
-  .capabilities = CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME) | CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC | CAPABILITY_DIRECT_SUBMIT,
-};
-
-DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vdpau = {
-  .setup = x11_setup_vdpau,
-  .cleanup = x11_cleanup,
-  .submitDecodeUnit = x11_submit_decode_unit,
-  .capabilities = CAPABILITY_DIRECT_SUBMIT,
-};
-
-DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vaapi = {
-  .setup = x11_setup_vaapi,
-  .cleanup = x11_cleanup,
-  .submitDecodeUnit = x11_submit_decode_unit,
-  .capabilities = CAPABILITY_DIRECT_SUBMIT,
-};
