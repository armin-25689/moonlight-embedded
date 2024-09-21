struct DISPLAY_CALLBACK {
  char *name;
  // equal to EGL_PLATFORM_XXX_KHR
  int egl_platform;
  int format;
  void* (*display_get_display) (const char* *device);
  void* (*display_get_window) ();
  void (*display_close_display) ();
  int (*display_setup) (int width, int height, int drFlags);
  void (*display_setup_post) (void *data);
  int (*display_put_to_screen) (int width, int height, int index);
  void (*display_get_resolution) (int* width, int* height);
  void (*display_change_cursor) (const char *oprate);
  int (*display_vsync_loop) (bool *exit, int *index, void(*loop_pre)(void), void(*loop_post)(void));
  int renders;
};
#ifdef HAVE_X11
extern struct DISPLAY_CALLBACK display_callback_x11;
#endif
#ifdef HAVE_WAYLAND
#define WAYLAND 0x04
extern struct DISPLAY_CALLBACK display_callback_wayland;
#endif
#ifdef HAVE_GBM
extern struct DISPLAY_CALLBACK display_callback_gbm;
#endif
#define QUITCODE "quit"
// compitible for some uint32_t format 
#define NOT_CARE 0
