#define WAYLAND 0x04

void* wl_get_display(const char *deivce);
void wl_close_display();
int wayland_setup(int width, int height, int drFlags);
void wl_setup_post();
void wl_show_picture();
void wl_dispatch_event();
void wl_get_resolution(int *width, int *height);
EGLSurface wl_get_egl_surface(EGLDisplay display, EGLConfig config, void *data);
EGLDisplay wl_get_egl_display();
