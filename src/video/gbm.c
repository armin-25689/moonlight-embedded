#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "display.h"
#include "drm_base.h"
#include "video_internal.h"

//equal to EGL_NATIVE_VISUAL_ID
#define DEFAULT_FORMAT GBM_FORMAT_XRGB8888

struct Gbm_Bo {
  uint32_t fd;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t pitch;
  uint32_t offset;
  uint32_t handle;
  uint32_t fb_id;
  struct gbm_bo *bo;
};

static struct gbm_device *gbm_display = NULL;
static struct gbm_surface *gbm_window = NULL;
static struct Gbm_Bo gbm_bo[MAX_FB_NUM] = {0};
static struct Drm_Info *drmInfoPtr;
static drmModeConnectorPtr connPtr;
static drmModeCrtcPtr crtcPtr;
static drmModeModeInfoPtr connModePtr;
// ayuv is packed format, yuv444 is 3_plane format
//static uint32_t supported_gbm_format[] = { GBM_FORMAT_XRGB8888, GBM_FORMAT_AYUV, GBM_FORMAT_YUV444, GBM_FORMAT_NV12, GBM_FORMAT_YUV420 };

static int frame_width,frame_height,display_width,display_height,x_offset = 0,y_offset = 0;

/*
static int validate_gbm_config (uint32_t format, uint32_t gbm_bo_flags) {
  gbm_device_is_format_supported(gbm_display, format, gbm_bo_flags );
}
*/

static int generate_gbm_bo(struct Gbm_Bo gbm_bo[MAX_FB_NUM], uint32_t format, uint32_t bo_flags) {
  for (int i = 0; i < MAX_FB_NUM; i++) {
    gbm_bo[i].bo = gbm_bo_create(gbm_display, display_width, display_height, format,
                              bo_flags);
    if (!gbm_bo[i].bo) {
      fprintf(stderr, "Failed to create a gbm buffer.\n");
      return -1;
    }

    gbm_bo[i].fd = gbm_bo_get_fd(gbm_bo[i].bo);
    if (gbm_bo[i].fd < 0) {
      fprintf(stderr, "Failed to get fb for gbm bo");
      gbm_bo_destroy(gbm_bo[i].bo);
      return -1;
    }

    gbm_bo[i].handle = gbm_bo_get_handle(gbm_bo[i].bo).u32;
    gbm_bo[i].pitch = gbm_bo_get_stride(gbm_bo[i].bo);
    gbm_bo[i].offset = 0;
    gbm_bo[i].width = display_width;
    gbm_bo[i].height = display_height;
    gbm_bo[i].format = format;
    drmModeAddFB2(drmInfoPtr->fd, display_width, display_height, format, &gbm_bo[i].handle, &gbm_bo[i].pitch, &gbm_bo[i].offset, &gbm_bo[i].fb_id, 0);
    if (!gbm_bo[i].fb_id) {
      fprintf(stderr, "Failed to create framebuffer from gbm buffer object.\n");
      gbm_bo_destroy(gbm_bo[i].bo);
      return -1;
    }
  }
  return 0;
}

static int gbm_setup(int width, int height, int drFlags) {
  // need to implement get screen width and height
  connPtr = drmModeGetConnector(drmInfoPtr->fd, drmInfoPtr->connector_id);
  crtcPtr = drmModeGetCrtc(drmInfoPtr->fd, drmInfoPtr->crtc_id);
  if (connPtr == NULL || crtcPtr == NULL) {
    fprintf(stderr, "Could not get connector from drm.");
    return -1;
  }
  connModePtr = &connPtr->modes[0];

  frame_width = width;
  frame_height = height;
  display_width = drmInfoPtr->width;
  display_height = drmInfoPtr->height;
//  convert_display(&width, &height, &display_width, &display_height, &x_offset, &y_offset);
  x_offset = 0;
  y_offset = 0;

  uint32_t format = DEFAULT_FORMAT;
  uint32_t bo_flags = GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT; // must need by egl
  //uint32_t bo_flags = GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT; // must need by egl
  //bo_flags |= GBM_BO_USE_FRONT_RENDERING;
  generate_gbm_bo(gbm_bo, format, bo_flags);
  gbm_window = gbm_surface_create(gbm_display, display_width, display_height, DEFAULT_FORMAT, bo_flags);
  if (!gbm_window) {
    fprintf(stderr, "Could not create gbm window.");
    return -1;
  }
  if (drmModeSetCrtc(drmInfoPtr->fd, crtcPtr->crtc_id, gbm_bo[0].fb_id, x_offset, y_offset, &connPtr->connector_id, 1, connModePtr) < 0) {
    fprintf(stderr, "Could not set fb to drm crtc.");
    return -1;
  }

  return 0;
}

static void* gbm_get_display(const char* *device) {
  if (gbm_display == NULL) {  
    drmInfoPtr = drm_init(NULL, DRM_FORMAT_ARGB8888, false);
    if (drmInfoPtr == NULL) {
      fprintf(stderr, "Could not init drm device.");
      return NULL;
    }

    gbm_display = gbm_create_device(drmInfoPtr->fd);
    if (!gbm_display) {
      fprintf(stderr, "Could not create gbm display.");
      drm_close();
      return NULL;
    }
  }

  *device = "/dev/dri/renderD128";
  return gbm_display;
}

static void* gbm_get_window() {
  return gbm_window;
}

static void gbm_get_resolution(int *width, int *height) {
  *width = drmInfoPtr->width;
  *height = drmInfoPtr->height;
  return;
}

/*
static int gbm_wait_vsync() {
  union drm_wait_vblank vb = {0};
  vb.request.type= DRM_VBLANK_RELATIVE | ((drmInfoPtr->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK);
  vb.request.sequence = 1;
  vb.request.signal = 0;
  return ioctl(drmInfoPtr->fd, DRM_IOCTL_WAIT_VBLANK, &vb);
}
*/

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void *data) {
  int *done = data;
  *done = 1;
}
static drmEventContext evctx = {
  .version = DRM_EVENT_CONTEXT_VERSION,
  .page_flip_handler = page_flip_handler,
};

static int gbm_display_done(int width, int height, int index) {
  int done = 0;
  int res = drmModePageFlip(drmInfoPtr->fd, crtcPtr->crtc_id, gbm_bo[index].fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, &done);
  if (res < 0) {
    fprintf(stderr, "drmModePageFlip() failed: %d", res);
    return -1;
  }

  while (!done) {
    drmHandleEvent(drmInfoPtr->fd, &evctx);
  }
  
  return 0;
}

static int gbm_vsync_loop(bool *exit, int *index, void(*loop_pre)(void), void(*loop_post)(void)) {
  while (!(*exit)) {
    int done = 0;
    int res = drmModePageFlip(drmInfoPtr->fd, crtcPtr->crtc_id, gbm_bo[*index].fb_id,
                              DRM_MODE_PAGE_FLIP_EVENT, &done);
    if (res < 0) {
      fprintf(stderr, "drmModePageFlip() failed: %d", res);
      return -1;
    }

    while (!done) {
      drmHandleEvent(drmInfoPtr->fd, &evctx);
    }
    loop_pre();
    loop_post();
  }
  
  //gbm_wait_vsync();
  return 0;
}

static void gbm_clear_image_cache () {
  for (int i = 0; i < MAX_FB_NUM; i++) {
    if (gbm_bo[i].fb_id != 0) {
      drmModeRmFB(drmInfoPtr->fd, gbm_bo[i].fb_id);
      close(gbm_bo[i].fd);
      gbm_bo_destroy(gbm_bo[i].bo);
      gbm_bo[i].handle = 0;
      gbm_bo[i].fb_id = 0;
      gbm_bo[i].bo = NULL;
    }
  }
}

static void gbm_close_display () {
  gbm_clear_image_cache();
  if (gbm_window)
    gbm_surface_destroy(gbm_window);
  gbm_window = NULL;
  if (gbm_display)
    gbm_device_destroy(gbm_display);
  gbm_display = NULL;
  if (connPtr != NULL)
    drmModeFreeConnector(connPtr);
  if (crtcPtr != NULL)
    drmModeFreeCrtc(crtcPtr);
  drm_close();
  return;
}

static void gbm_setup_post(void *data) {};
static void gbm_change_cursor(const char *op) {};

struct DISPLAY_CALLBACK display_callback_gbm = {
  .name = "gbm",
  .egl_platform = 0x31D7,
  .format = DEFAULT_FORMAT,
  .display_get_display = gbm_get_display,
  .display_get_window = gbm_get_window,
  .display_close_display = gbm_close_display,
  .display_setup = gbm_setup,
  .display_setup_post = gbm_setup_post,
  .display_put_to_screen = gbm_display_done,
  .display_get_resolution = gbm_get_resolution,
  .display_change_cursor = gbm_change_cursor,
  .display_vsync_loop = gbm_vsync_loop,
  .renders = EGL_RENDER,
};

void export_bo(struct Source_Buffer_Info buffers[MAX_FB_NUM], int *buffer_num, int *plane_num) {
  *buffer_num = MAX_FB_NUM;
  *plane_num = 1;
  for (int i = 0; i < *buffer_num; i++) {
    memcpy(&buffers[i], &gbm_bo[i], sizeof(buffers[i]));
  }
}
