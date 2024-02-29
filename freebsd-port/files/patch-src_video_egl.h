--- src/video/egl.h.orig	2024-02-20 04:01:31 UTC
+++ src/video/egl.h
@@ -18,7 +18,9 @@
  */
 
 #include <EGL/egl.h>
+#include <libavcodec/avcodec.h>
 
-void egl_init(EGLNativeDisplayType native_display, NativeWindowType native_window, int display_width, int display_height);
-void egl_draw(uint8_t* image[3]);
+void egl_init(void *native_display, int display_width, int display_height, int dcFlag);
+void egl_draw(AVFrame* frame, uint8_t* image[3]);
+void egl_draw_frame(AVFrame* frame);
 void egl_destroy();
