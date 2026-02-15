#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>

#include <sys/consio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "display.h"
#include "drm_base.h"
#include "gbm.h"
#include "video.h"
#include "video_internal.h"
#include "../loop.h"
#include "../input/evdev.h"

#include "ffmpeg.h"
#include "render.h"
#include "convert.h"

//equal to EGL_NATIVE_VISUAL_ID
#define DEFAULT_FORMAT DRM_FORMAT_XRGB8888
#define DEFAULT_FORMAT_10BIT DRM_FORMAT_XRGB2101010

static struct _drm_buf drm_buf[MAX_FB_NUM] = {0};
static struct Drm_Info *drmInfoPtr;
static drmModeConnectorPtr connPtr;
static drmModeModeInfoPtr connModePtr;
static void *gbm_display = NULL;
static void *gbm_window = NULL;
static bool isMaster = true;
static uint32_t last_fbid = 0;
static uint32_t hdr_blob = 0;

static int frame_width,frame_height,display_width,display_height;
static uint64_t fps_time;

struct _drm_render_config {
  bool full_color_range;
  bool need_change_color;
  int src_fmt;
  int dst_fmt;
  int plane_num;
  int handle_num;
  int bpp;
  int buffer_multi;
  int colorspace;
  AVFrame *frame;
  uint32_t plane_format;
  uint64_t size[MAX_FB_NUM][4];
} static drm_config = {0};

static int (*drm_draw_function) (struct Render_Image *image);
static int drm_copy (struct Render_Image *image);
static int drm_direct(struct Render_Image *image) { return image->index; };
// render

struct Tty_Stat {
  int fd;
  int index;
  struct termios term;
  struct vt_mode vt;
  int kd;
  bool has_get;
  bool out;
};
static struct Tty_Stat tty_stat = { .fd = -1, .index = -1, .has_get = false, .out = false, };

static inline void clear_tty (struct Tty_Stat *tty) {
  if (tty->fd < 0) return;
  loop_remove_fd(tty->fd);
  close(tty->fd);
  memset(tty, 0, sizeof(struct Tty_Stat));
  tty->fd = -1;
}

static int stdin_handle (int fd, void *data) {
  unsigned char key;
  int ret;

  while ((ret = read(fd, &key, 1)) > 0);
  if (ret == 0)
    clear_tty((struct Tty_Stat *) data);

  return 0;
}

static inline int get_termios (int fd, struct termios *term) {
  return tcgetattr(fd, term);
}
static int set_new_tty (int fd, struct Tty_Stat *tty) {
  if (!tty->has_get) {
    if (get_termios(fd, &tty->term) < 0) {
      perror("Error: get termios failed: ");
      return -1;
    }
    if (ioctl(fd, KDGETMODE, &tty->kd) < 0) {
      perror("Error: get kd mode failed: ");
      return -1;
    }
  }

  struct termios term = tty->term;
  term.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  term.c_iflag &= ~(ICRNL | INPCK | BRKINT | IXON);
  term.c_cc[VMIN] = 1;
  term.c_cc[VTIME] = 0;
  if (tcsetattr(fd, TCSANOW, &term) < 0) {
    perror("Error: set termios failed: ");
    return -1;
  }

  int mode = KD_GRAPHICS;
  if (ioctl(fd, KDSETMODE, &mode) < 0) {
    perror("Error: set kd mode failed: ");
    return -1;
  }

  tty->has_get = true;

  return 0;
}
static int set_orig_tty (int fd, struct Tty_Stat *tty) {
  int ret = -1;
  if (tty->has_get) {
    ret = tcsetattr(fd, TCSANOW, &tty->term);
    if (ret < 0) perror("Error: set termios failed: ");
    ret = ioctl(fd, KDSETMODE, &tty->kd);
    if (ret < 0) perror("Error: set kd mode failed: ");
  }

  clear_tty(tty);

  return ret; 
}
static int tty_opt (struct Tty_Stat *tty, int (*change_opt) (int fd, struct Tty_Stat *t)) {
  int ret = -1;
  if (!isatty(STDIN_FILENO)) {
    fprintf(stderr, "Is not valid tty or redirect stdin.\n");
    return -1;
  }
  const char *tty_name = ttyname(0);

  if (tty == NULL) return ret;

  if (strncmp(tty_name, "/dev/ttyv", 9) == 0) {
    if (tty->fd < 0) {
      if (!drmInfoPtr->have_atomic) return 0;
      tty->fd = open(tty_name,  O_RDWR | O_NOCTTY |O_NONBLOCK);
      if (tty->fd < 0)
        return ret;
      int index = -1;
      sscanf(tty_name, "%*[^0-9]%d", &index);
      if (index < 0) {
        close(tty->fd);
        tty->fd = -1;
        return ret;
      }
      tty->index = index;
      loop_add_fd1(tty->fd, &stdin_handle, EPOLLIN, tty);
    }
    ret = change_opt(tty->fd, tty);
    if (ret < 0) {
      perror("Error: Set/Get tty attr faild: ");
      clear_tty(tty);
    }
  }

  return ret;
}

static void get_aligned_width (int width, int ajustedw, int srcw, int *dstw) {
  if (srcw == width) {
    *dstw = ajustedw;
    return;
  }
  for (int i = 4; i < 512; i = i * 2) {
    int mid = width / i;
    if (ajustedw == (i * mid + i)) {
      *dstw = (((int)(srcw / i)) * i + i);
      return;
    }
  }
  return;
}

static uint32_t drm_generate_drm_buf (int drm_fd, int src_format, int width, int height, uint32_t flags, struct _drm_buf *drm_buf, int buffer_num) {

  uint32_t format = translate_format_to_drm(src_format, &drm_config.bpp, &drm_config.buffer_multi, &drm_config.plane_num);
  if (format <= 0) {
    fprintf(stderr, "drm: not support pix format.\n");
    return -1;
  }
  drm_config.plane_format = format;
  
  int handle_num;
  for (int i = 0; i < buffer_num; i++) {
    int plane_width[4] = { width, drm_config.buffer_multi == drm_config.plane_num ? width : (int)(width / 2),
                           drm_config.buffer_multi == drm_config.plane_num ? width : (int)(width / 2), width };
    int plane_height[4] = { height, drm_config.buffer_multi != 2 ? height : (int)(height / 2),
                            drm_config.buffer_multi != 2 ? height : (int)(height / 2), height };
    if (flags & SEPERATE_PLANE) {
      handle_num = drm_config.plane_num;
    }
    else {
      handle_num = 1;
    }
    for (int j = 0; j < handle_num; j++) {
      struct drm_mode_create_dumb createBuf = {};
      createBuf.width = plane_width[j];
      createBuf.height = plane_height[j] * (handle_num == 1 ? drm_config.buffer_multi : 1);
      createBuf.bpp = drm_config.bpp;
      if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &createBuf) < 0) {
        perror("Could not create drm dumb: ");
        return -1;
      }
      if (drmPrimeHandleToFD(drm_fd, createBuf.handle, O_CLOEXEC, &drm_buf[i].fd[j]) < 0) {
        fprintf(stderr, "Cannot get drm fd.\n");
        return -1;
      }
      drm_buf[i].handle[j] = createBuf.handle;
      drm_buf[i].pitch[j] = createBuf.pitch;
      drm_buf[i].width[j] = plane_width[j];
      drm_buf[i].height[j] = plane_height[j];
      drm_config.size[i][j] = createBuf.size;
    }
    if (handle_num == 1) {
      for (int k = 1; k < drm_config.plane_num; k++) {
        int dstpitch = 0;
        get_aligned_width(drm_buf[i].width[0], (drm_buf[i].pitch[0] * 8 / drm_config.bpp), plane_width[k], &dstpitch);
        drm_buf[i].handle[k] = drm_buf[i].handle[0];
        drm_buf[i].pitch[k] = dstpitch > 0 ? (dstpitch * drm_config.bpp / 8) : drm_buf[i].pitch[0];
        drm_buf[i].height[k] = plane_height[k];
        drm_buf[i].width[k] = plane_width[k];
        drm_config.size[i][k] = drm_config.size[i][0];
      }
    }
    for (int k = 0; k < drm_config.plane_num; k++) {
      drm_buf[i].offset[k] = (k == 0 || handle_num != 1) ? 0 : drm_buf[i].pitch[k - 1] * drm_buf[i].height[k - 1] + drm_buf[i].offset[k - 1];
      drm_buf[i].format[k] = format;
      drm_buf[i].modifiers[k] = DRM_FORMAT_MOD_LINEAR;
    }
    int add_flags = DRM_MODE_FB_MODIFIERS;
    drmModeAddFB2WithModifiers(drm_fd, width, height, format, drm_buf[i].handle, drm_buf[i].pitch, drm_buf[i].offset, drm_buf[i].modifiers, &drm_buf[i].fb_id, add_flags);
    if (!drm_buf[i].fb_id) {
      perror("Failed to create framebuffer from drm buffer object: ");
      for (int m = 0; m < handle_num; m++) {
        struct drm_mode_destroy_dumb destroyBuf = {0};
        destroyBuf.handle = drm_buf[i].handle[m];
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyBuf);
      }
      return -1;
    }
  }
  drm_config.handle_num = handle_num;

  return format;
}

static int drm_setup(int width, int height, int fps, int drFlags) {
  fps_time = ((int)(1000000 / (fps)));
  // need to implement get screen width and height
  connPtr = drmModeGetConnector(drmInfoPtr->fd, drmInfoPtr->connector_id);
  if (connPtr == NULL) {
    fprintf(stderr, "Could not get connector from drm.\n");
    return -1;
  }

  connModePtr = &drmInfoPtr->crtc_mode;
  if (drFlags & MODESET) {
    int findmode = 0;
    for (int i = 0; i < connPtr->count_modes; i++) {
      if (width == connPtr->modes[i].hdisplay && height == connPtr->modes[i].vdisplay && fps == connPtr->modes[i].vrefresh && (connPtr->modes[i].flags & DRM_MODE_FLAG_INTERLACE) == 0) {
        findmode = 1;
        drmInfoPtr->width = connPtr->modes[i].hdisplay;
        drmInfoPtr->height = connPtr->modes[i].vdisplay;
        connModePtr = &connPtr->modes[i];
        memcpy(&drmInfoPtr->crtc_mode,&connPtr->modes[i],sizeof(connPtr->modes[i]));
        break;
      }
    }
    if (findmode == 0) {
      fprintf(stderr, "Could not find supported resolution with configured: width-%d height-%d.\n", width, height);
      fprintf(stderr, "supported resolution is:");
      for (int i = 0; i < connPtr->count_modes; i++) {
        fprintf(stderr, " %dx%d@%d ", connPtr->modes[i].hdisplay, connPtr->modes[i].vdisplay, connPtr->modes[i].vrefresh);
      }
      fprintf(stderr, "\n");
    }
  }

  frame_width = width;
  frame_height = height;
  display_width = drmInfoPtr->width;
  display_height = drmInfoPtr->height;

  uint32_t rotate = drFlags & DISPLAY_ROTATE_MASK;
  if (rotate) drm_opt_commit(DRM_ADD_COMMIT, NULL, drmInfoPtr->plane_id, drmInfoPtr->plane_rotation_prop_id, (rotate >> 2));

  if (drFlags & DRM_RENDER) {
    gbm_close_display (-1, NULL, MAX_FB_NUM, gbm_display, NULL);
    gbm_display = NULL;
    if (drFlags & ENABLE_HARDWARE_ACCELERATION_2) {
      drm_draw_function = &drm_direct;
    } else {
      drm_draw_function = &drm_copy;
    }
  } else {
    if ((drFlags & EGL_RENDER) == 0) return -1;
    uint32_t format = wantHdr ? DEFAULT_FORMAT_10BIT : DEFAULT_FORMAT;
    frame_width = display_width;
    frame_height = display_height;
    format = DEFAULT_FORMAT;
    display_callback_drm.hdr_support = false;
    if (generate_gbm_buffer(drmInfoPtr->fd, drm_buf, MAX_FB_NUM, gbm_display, frame_width, frame_height, AV_PIX_FMT_BGR0) < 0)
      return -1;
    gbm_window = gbm_get_window(drmInfoPtr->fd, gbm_display, frame_width, frame_height, format);
    if (gbm_window == NULL)
      return -1;
    if (isMaster && drm_set_display(drmInfoPtr->fd, drmInfoPtr->crtc_id, frame_width, frame_height, display_width, display_height, &connPtr->connector_id, 1, connModePtr, drm_buf[0].fb_id) < 0) {
      fprintf(stderr, "Could not set fb to drm crtc.\n");
    }
  }

  tty_opt (&tty_stat, &set_new_tty);

  return 0;
}

static void* drm_get_display(const char* *device) {
  uint32_t format = wantHdr ? DEFAULT_FORMAT_10BIT : DEFAULT_FORMAT;
  drm_config.dst_fmt = -1;
  drmInfoPtr = drm_init(NULL, format, wantHdr);
  if (drmInfoPtr == NULL && wantHdr) {
    format = DEFAULT_FORMAT;
    drmInfoPtr = drm_init(NULL, format, false);
    if (drmInfoPtr) {
      display_callback_drm.hdr_support = false;
    }
  }
  if (drmInfoPtr == NULL) {
    fprintf(stderr, "Could not init drm device.\n");
    return NULL;
  }
  if (!drmInfoPtr->have_atomic)
    display_callback_drm.hdr_support = false;

  if (drmSetMaster(drmInfoPtr->fd) < 0) {
    fprintf(stderr, "DRM: drmSetMaster() failed.\n");
    return NULL;
  }

  *device = "/dev/dri/renderD128";
  gbm_display = gbm_get_display(&drmInfoPtr->fd);

  return gbm_display;
}

static void drm_clear_image_cache (int drm_fd, struct _drm_buf *drm_buf, int buffer_num) {
  for (int i = 0; i < buffer_num; i++) {
    if (drm_buf[i].fb_id != 0) {
      drmModeRmFB(drm_fd, drm_buf[i].fb_id);
      for (int j = 0; j < drm_config.handle_num; j++) {
        close(drm_buf[i].fd[j]);
        if (gbm_display == NULL) {
          struct drm_mode_destroy_dumb destroyBuf = {0};
          destroyBuf.handle = drm_buf[i].handle[j];
          drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyBuf);
        }
      }
      memset(&drm_buf[i], 0, sizeof(drm_buf[i]));
      memset(drm_buf[i].fd, -1, sizeof(drm_buf[i].fd));
    }
  }
}

static void drm_cleanup (void *data) {
  if (hdr_blob > 0)
    drmModeDestroyPropertyBlob(drmInfoPtr->fd, hdr_blob);
  hdr_blob = 0;
  if (!tty_stat.out)
    drm_restore_display();
  drm_clear_image_cache(drmInfoPtr->fd, drm_buf, MAX_FB_NUM);
  gbm_close_display (-1, drm_buf, MAX_FB_NUM, gbm_display, gbm_window);
  gbm_display = NULL;
  gbm_window = NULL;
  if (connPtr != NULL)
    drmModeFreeConnector(connPtr);
  drm_close();

  tty_opt (&tty_stat, &set_orig_tty);

  return;
}

static void drm_setup_post(void *data) {
  return;
}

static void* drm_get_window() {
  return gbm_window;
}

static void drm_get_resolution(int *width, int *height, bool isfullscreen) {
  *width = drmInfoPtr->width;
  *height = drmInfoPtr->height;
  return;
}

static int set_hdr_metadata_blob (struct Drm_Info *drmInfoPtr, uint32_t *hdr_blob) {
  struct hdr_output_metadata data = {0};
  SS_HDR_METADATA sunshineHdrMetadata = {0};
  static bool usehdr = false;
  bool hdrp = false;

  if (!LiGetHdrMetadata(&sunshineHdrMetadata)) {
    hdrp = false;
  }
  else 
    hdrp = true;

  if (usehdr == hdrp) return 0;

  if (!hdrp) {
    drm_opt_commit(DRM_ADD_COMMIT, NULL, drmInfoPtr->connector_id, drmInfoPtr->conn_colorspace_prop_id, drmInfoPtr->conn_colorspace_values[DEFAULTCOLOR]);
    drm_opt_commit(DRM_ADD_COMMIT, NULL, drmInfoPtr->connector_id, drmInfoPtr->conn_hdr_metadata_prop_id, 0);
    drm_opt_commit(DRM_ADD_COMMIT, NULL, drmInfoPtr->plane_id, drmInfoPtr->plane_eotf_prop_id, 0);
    usehdr = hdrp;
    return 0;
  }

  data.metadata_type = 0; // HDMI_STATIC_METADATA_TYPE1
  data.hdmi_metadata_type1.eotf = 2; // SMPTE ST 2084
  data.hdmi_metadata_type1.metadata_type = 0; // Static Metadata Type 1
  for (int i = 0; i < 3; i++) {
    data.hdmi_metadata_type1.display_primaries[i].x = sunshineHdrMetadata.displayPrimaries[i].x;
    data.hdmi_metadata_type1.display_primaries[i].y = sunshineHdrMetadata.displayPrimaries[i].y;
  }
  data.hdmi_metadata_type1.white_point.x = sunshineHdrMetadata.whitePoint.x;
  data.hdmi_metadata_type1.white_point.y = sunshineHdrMetadata.whitePoint.y;
  data.hdmi_metadata_type1.max_display_mastering_luminance = sunshineHdrMetadata.maxDisplayLuminance;
  data.hdmi_metadata_type1.min_display_mastering_luminance = sunshineHdrMetadata.minDisplayLuminance;
  data.hdmi_metadata_type1.max_cll = sunshineHdrMetadata.maxContentLightLevel;
  data.hdmi_metadata_type1.max_fall = sunshineHdrMetadata.maxFrameAverageLightLevel;

  if (*hdr_blob > 0)
    drmModeDestroyPropertyBlob(drmInfoPtr->fd, *hdr_blob);
  *hdr_blob = 0;
  if (drmModeCreatePropertyBlob(drmInfoPtr->fd, &data, sizeof(struct hdr_output_metadata), hdr_blob) < 0) {
    perror("Failed to create hdr metadata blob: ");
    return -1;
  }

  drm_opt_commit(DRM_ADD_COMMIT, NULL, drmInfoPtr->connector_id, drmInfoPtr->conn_colorspace_prop_id, 
                 (drm_config.need_change_color && drmInfoPtr->conn_colorspace_values[D2020YCC] > 0) ? drmInfoPtr->conn_colorspace_values[D2020YCC] : drmInfoPtr->conn_colorspace_values[D2020RGB]);
  drm_opt_commit(DRM_ADD_COMMIT, NULL, drmInfoPtr->connector_id, drmInfoPtr->conn_hdr_metadata_prop_id, (uint64_t)(*hdr_blob));
  drm_opt_commit(DRM_ADD_COMMIT, NULL, drmInfoPtr->plane_id, drmInfoPtr->plane_eotf_prop_id, 2);
  usehdr = hdrp;

  return 0;
}

static int drm_display_done(int width, int height, int index) {
  return 0;
}

static int drm_display_loop(void *data, int width, int height, int index) {
  int ret = -1;
  uint32_t fb_id;

  if (tty_stat.out) {
    usleep(fps_time);
    return 0;
  }

  fb_id = drm_buf[index].fb_id;
  if (fb_id <= 0)
    return ret;
  if (last_fbid == fb_id && drmInfoPtr->have_atomic)
    fb_id = 0;
  uint32_t dwidth = (uint32_t)frame_width;
  uint32_t dheight = (uint32_t)frame_height;

  set_hdr_metadata_blob (drmInfoPtr, &hdr_blob);

  ret = drm_flip_buffer(drmInfoPtr->fd, drmInfoPtr->crtc_id, fb_id, hdr_blob, dwidth, dheight);

  return ret;
}

static void drm_export_buffer(struct Source_Buffer_Info buffers[MAX_FB_NUM], int *buffer_num, int *plane_num) {
  *buffer_num = MAX_FB_NUM;
  *plane_num = 1;
  for (int i = 0; i < *buffer_num; i++) {
    memcpy(&buffers[i], &drm_buf[i], sizeof(buffers[i]));
  }
}

static void drm_switch_vt(struct WINDOW_OP *op, int flags) {
  if (op->switch_vt > 0) {
    if (tty_stat.index < 0) return;
    if (tty_stat.index == (op->switch_vt - 1)) {
      if (tty_stat.out) {
        if (op->from_display_server)
          usleep(1000000);
        else
          usleep(fps_time * 2);
        if (drmSetMaster(drmInfoPtr->fd) == 0) {
          drm_opt_commit(DRM_RESTORE_COMMIT, NULL, 0, 0, 0);
          tty_stat.out = false;
          sync_input_state(true);
        }
      }
    }
    else {
      //out
      if (!tty_stat.out) {
        tty_stat.out = true;
        drmDropMaster(drmInfoPtr->fd);
        sync_input_state(false);
        usleep(fps_time * 3);
        if (drmSetMaster(drmInfoPtr->fd) == 0) {
          drm_restore_display();
          usleep(fps_time);
          drmDropMaster(drmInfoPtr->fd);
        }
      }
    }
  }
  return;
}

struct DISPLAY_CALLBACK display_callback_drm = {
  .name = "drm",
  .egl_platform = 0x31D7,
  .format = DEFAULT_FORMAT,
  .hdr_support = true,
  .display_get_display = drm_get_display,
  .display_get_window = drm_get_window,
  .display_close_display = drm_cleanup,
  .display_setup = drm_setup,
  .display_setup_post = drm_setup_post,
  .display_put_to_screen = drm_display_done,
  .display_get_resolution = drm_get_resolution,
  .display_modify_window = drm_switch_vt,
  .display_vsync_loop = drm_display_loop,
  .display_exported_buffer_info = drm_export_buffer,
  .renders = DRM_RENDER | EGL_RENDER,
};

static int drm_render_create(struct Render_Init_Info *paras) { return 0; };

static int drm_render_init(struct Render_Init_Info *paras) { return 0; };

static void drm_render_destroy() {};

static int get_config_from_frame(struct Render_Config *config) {
  bool need_change_color_config = false;
  bool need_generate_buffer = false;
  drm_config.src_fmt = config->pix_fmt;
  drm_config.full_color_range = config->full_color_range;
  drm_config.colorspace = config->color_space;

  switch (config->pix_fmt) {
  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUVJ444P:
    need_generate_buffer = true;
    drm_config.dst_fmt = AV_PIX_FMT_BGR0;
    break;
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P:
    drm_config.dst_fmt = AV_PIX_FMT_NV12;
    need_change_color_config = true;
    need_generate_buffer = true;
    break;
  case AV_PIX_FMT_VUYX:
  case AV_PIX_FMT_XV30:
  case AV_PIX_FMT_NV12:
  case AV_PIX_FMT_P010:
    drm_config.dst_fmt = config->pix_fmt;
    need_change_color_config = true;
    break;
  case AV_PIX_FMT_YUV444P10:
    need_generate_buffer = true;
    drm_config.dst_fmt = AV_PIX_FMT_X2RGB10LE;
    break;
  case AV_PIX_FMT_YUV420P10:
    need_generate_buffer = true;
    need_change_color_config = true;
    drm_config.dst_fmt = AV_PIX_FMT_P010;
    break;
  }
  int flags = 0;
  if (need_generate_buffer) {
    drm_clear_image_cache(drmInfoPtr->fd, drm_buf, MAX_FB_NUM);
    if (drm_generate_drm_buf(drmInfoPtr->fd, drm_config.dst_fmt, frame_width, frame_height, flags, drm_buf, MAX_FB_NUM) < 0) {
      fprintf(stderr, "Could not generate buf.\n");
      return -1;
    }
  }

  if (drm_get_plane_info(drmInfoPtr, drm_config.plane_format) < 0) {
    fprintf(stderr, "Could not find supported format with planes.\n");
    return -1;
  }

  if (need_change_color_config) {
    drm_config.need_change_color = need_change_color_config;
    enum DrmColorSpace colorspace = drm_config.colorspace == COLORSPACE_REC_2020 ? DBT2020 : (drm_config.colorspace == COLORSPACE_REC_709 ? DBT709 : DBT601);
    drm_choose_color_config(colorspace, drm_config.full_color_range);
  }

  if (isMaster && drm_set_display(drmInfoPtr->fd, drmInfoPtr->crtc_id, frame_width, frame_height, display_width, display_height, &connPtr->connector_id, 1, connModePtr, drm_buf[0].fb_id) < 0) {
    fprintf(stderr, "Could not set fb to drm crtc.\n");
    return -1;
  }

  return 0;
}

static inline int drm_convert_image(struct Render_Image *image, struct _drm_buf *drm_buf, int drm_fd, int handle_num, int plane_num, int dst_fmt, uint64_t size[MAX_PLANE_NUM], uint64_t map_offset[MAX_PLANE_NUM]) {
  AVFrame * sframe = (AVFrame *)image->sframe.frame;
  uint8_t *data_buffer[4] = {0};

  for (int m = 0; m < handle_num; m++) {
    uint8_t *data_ptr = (uint8_t*)mmap(NULL, size[m], PROT_WRITE, MAP_SHARED, drm_fd, map_offset[m]);
    if (data_ptr == MAP_FAILED) {
      perror("Could not map dumb buffer to userspace: ");
      return -1;
    }

    if (handle_num == 1) {
      for (int i = 0; i < plane_num; i++) {
        data_buffer[i] = data_ptr + drm_buf[image->index].offset[i];
      }
    } else {
      data_buffer[m] = data_ptr;
    }
  }

  convert_frame(sframe, data_buffer, drm_buf[image->index].pitch, dst_fmt);

  if (handle_num == 1) {
    munmap(data_buffer[0], size[0]);
  } else {
    for (int m = 0; m < handle_num; m++) {
      munmap(data_buffer[m], size[m]);
    }
  }

  return image->index;
}

static int drm_copy(struct Render_Image *image) { 
  uint64_t map_offset[MAX_PLANE_NUM] = {0};

  for (int m = 0; m < drm_config.handle_num; m++) {
    struct drm_mode_map_dumb mapBuf = {};
    mapBuf.handle = drm_buf[image->index].handle[m];
    if (drmIoctl(drmInfoPtr->fd, DRM_IOCTL_MODE_MAP_DUMB, &mapBuf) < 0) {
      fprintf(stderr, "Could not map dumb\n");
      return -1;
    }
    map_offset[m] = mapBuf.offset;
  }

  return drm_convert_image(image, drm_buf, drmInfoPtr->fd, drm_config.handle_num, drm_config.plane_num, drm_config.dst_fmt, drm_config.size[image->index], map_offset);
}

int drm_import_hw_buffer (int fd, struct _drm_buf *drm_buf, struct Source_Buffer_Info *buffer, int planes, int composeOrSeperate, void* *image, int index) {

  for (int i = 0; i < planes; i++) {
    drm_buf[index].fd[i] = buffer->fd[i];
    drm_buf[index].width[i] = buffer->width[i];
    drm_buf[index].height[i] = buffer->height[i];
    drm_buf[index].format[i] = buffer->format[i];
    drm_buf[index].pitch[i] = buffer->stride[i];
    drm_buf[index].offset[i] = buffer->offset[i];
    drm_buf[index].modifiers[i] = buffer->modifiers[i];
  }

  return index;
}

static inline void drm_free_hw_buffer (int fd, void* *image, int planes) {
  if (image[0] == NULL) return;

  uint32_t fb_id = *((uint32_t *)image[0]);
  uint32_t *handle = ((uint32_t *)image[1]);

  if (fb_id != 0) {
    drmModeRmFB(fd, fb_id);
  }
  if (handle) {
    for (int i = 0; i < planes; i++) {
      if (handle[i])
        drmCloseBufferHandle(fd, handle[i]);
    }
  }
  *((uint32_t *)image[0]) = 0;
  memset(image[1], 0, sizeof(drm_buf[0].handle));

  return;
}

static int drm_import_buffer (struct Source_Buffer_Info *buffer, int planes, int composeOrSeperate, void* *image, int index) {
  if (drm_import_hw_buffer(drmInfoPtr->fd, drm_buf, buffer, planes, composeOrSeperate, image, index) < 0) {
    drm_free_hw_buffer(drmInfoPtr->fd, image, planes);
    return -1;
  }

  int handle_num = composeOrSeperate == COMPOSE_PLANE ?  1 : planes;

  for (int i = 0; i < handle_num; i++) {
    if (drmPrimeFDToHandle(drmInfoPtr->fd, drm_buf[index].fd[i], &drm_buf[index].handle[i]) < 0) {
      for (int k = 0; k < i; k++) {
        drmCloseBufferHandle(drmInfoPtr->fd, drm_buf[index].handle[k]);
      }
      fprintf(stderr, "Could not success drmPrimeFDToHandle(%d, %d, %d)\n", drmInfoPtr->fd, drm_buf[index].fd[i], i);
      return -1;
    }
  }
  if (handle_num == 1) {
    for (int k = 1; k < planes; k++) {
      drm_buf[index].handle[k] = drm_buf[index].handle[0];
    }
  }
  uint32_t dformat = drm_buf[index].format[0] == DRM_FORMAT_Y410 ? DRM_FORMAT_XVYU2101010 : drm_buf[index].format[0];
  int flags = drm_buf[index].modifiers[0] != DRM_FORMAT_MOD_INVALID ? DRM_MODE_FB_MODIFIERS : 0;
  drmModeAddFB2WithModifiers(drmInfoPtr->fd, drm_buf[index].width[0], drm_buf[index].height[0], dformat, drm_buf[index].handle, drm_buf[index].pitch, drm_buf[index].offset, drm_buf[index].modifiers, &drm_buf[index].fb_id, flags);
  if (drm_buf[index].fb_id == 0) {
    perror("Failed to create framebuffer from drm buffer object: ");
    for (int i = 0; i < 4; i++) {
      if (drm_buf[index].handle[i] > 0)
        drmCloseBufferHandle(drmInfoPtr->fd, drm_buf[index].handle[i]);
    }
    memset(drm_buf[index].handle, 0, sizeof(drm_buf[index].handle));
    return -1;
  }

  image[0] = &drm_buf[index].fb_id;
  image[1] = drm_buf[index].handle;


  return index;
}

static void drm_free_buffer (void* *image, int planes) {
  drm_free_hw_buffer(drmInfoPtr->fd, image, planes);
  return;
}

static int drm_draw(struct Render_Image *image) { 
  int colorspace = ffmpeg_get_frame_colorspace(image->sframe.frame);
  if (drm_config.need_change_color && drm_config.colorspace != colorspace) {
    drm_config.colorspace = colorspace;
    enum DrmColorSpace colorspace = drm_config.colorspace == COLORSPACE_REC_2020 ? DBT2020 : (drm_config.colorspace == COLORSPACE_REC_709 ? DBT709 : DBT601);
    drm_choose_color_config(colorspace, drm_config.full_color_range);
  }

  return drm_draw_function(image);
}

struct RENDER_CALLBACK drm_render = {
  .name = "drm",
  .display_name = "drm",
  .is_hardaccel_support = true,
  .render_type = DRM_RENDER,
  .decoder_type = SOFTWARE,
  .data = NULL,
  .render_create = drm_render_create,
  .render_init = drm_render_init,
  .render_sync_config = get_config_from_frame,
  .render_draw = drm_draw,
  .render_destroy = drm_render_destroy,
  .render_sync_window_size = NULL,
  .render_map_buffer = drm_import_buffer,
  .render_unmap_buffer = drm_free_buffer,
};
