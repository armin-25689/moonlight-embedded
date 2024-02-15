--- src/video/egl.h.orig	2023-11-03 06:08:34 UTC
+++ src/video/egl.h
@@ -18,7 +18,11 @@
  */
 
 #include <EGL/egl.h>
+#include <libavcodec/avcodec.h>
 
-void egl_init(EGLNativeDisplayType native_display, NativeWindowType native_window, int display_width, int display_height);
+#define YUV444 0x08
+
+void egl_init(void *native_display, int display_width, int display_height, int dcFlag);
 void egl_draw(uint8_t* image[3]);
+void egl_draw_frame(AVFrame* frame);
 void egl_destroy();
