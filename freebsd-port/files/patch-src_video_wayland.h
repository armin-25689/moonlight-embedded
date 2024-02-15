--- src/video/wayland.h.orig	2024-02-15 11:36:00 UTC
+++ src/video/wayland.h
@@ -0,0 +1,13 @@
+#define WAYLAND 0x04
+#define QUITCODE "quit"
+
+void* wl_get_display(const char *deivce);
+void wl_close_display();
+int wayland_setup(int width, int height, int drFlags);
+void wl_setup_post();
+void wl_show_picture();
+void wl_dispatch_event();
+void wl_get_resolution(int *width, int *height);
+void wl_trans_op_fd(int fd);
+EGLSurface wl_get_egl_surface(EGLDisplay display, EGLConfig config, void *data);
+EGLDisplay wl_get_egl_display();
