//#include <stdbool.h>

void x_vaapi_draw(AVFrame* frame, int display_width, int display_height);
bool x_test_vaapi_draw(AVFrame* frame, int display_width, int display_height);
void* x_get_display(const char *device);
int x_setup(int width, int height, int drFlags);
void x_close_display();
void x_muilti_threads();
void x_get_resolution(int *width, int *height);
EGLSurface x_get_egl_surface(EGLDisplay display, EGLConfig config, void *data);
EGLDisplay x_get_egl_display();
