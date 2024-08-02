--- src/video/x11.h.orig	2024-08-01 13:37:02 UTC
+++ src/video/x11.h
@@ -0,0 +1,8 @@
+void x_vaapi_draw(AVFrame* frame, int display_width, int display_height);
+bool x_test_vaapi_draw(AVFrame* frame, int display_width, int display_height);
+void* x_get_display(const char *device);
+void* x_get_window();
+int x_setup(int width, int height, int drFlags);
+void x_close_display();
+void x_muilti_threads();
+void x_get_resolution(int *width, int *height);
