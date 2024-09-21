#include "video_internal.h"

struct Render_Config {
  int color_space;
  int color_range;
  int pix_fmt;
  int plane_nums;
  int image_nums;
  enum PixelFormatOrder yuv_order;
  bool full_color_range;
  bool use_hdr;
  bool vsync;
};
struct Render_Init_Info {
  int frame_width;
  int frame_height;
  int screen_width;
  int screen_height;
  int format;
  bool is_full_screen;
  bool is_yuv444;
  bool use_display_buffer;
  int egl_platform;
  void *display;
  void *window;
};
union Render_Image {
  struct {
    void *image_data[4];
    void *descriptor;
  } images;
  uint8_t **frame_data;
};
struct RENDER_CALLBACK {
  char *name;
  char *display_name;
  int render_type;
  int decoder_type;
  bool is_hardaccel_support;
  bool extension_support;
  void *data;
  union Render_Image images[MAX_FB_NUM];
  int (*render_create) (struct Render_Init_Info *paras);
  int (*render_init) (struct Render_Init_Info *paras);
  void (*render_sync_config) (struct Render_Config *config);
  int (*render_draw) (union Render_Image image);
  void (*render_destroy) ();
};

extern struct RENDER_CALLBACK egl_render;
#ifdef HAVE_X11
extern struct RENDER_CALLBACK x11_render;
#endif
