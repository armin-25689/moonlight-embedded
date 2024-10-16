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
#include "video.h"
#include "video_internal.h"

//equal to EGL_NATIVE_VISUAL_ID
#define DEFAULT_FORMAT GBM_FORMAT_XRGB8888
#define DEFAULT_FORMAT_10BIT GBM_FORMAT_XRGB2101010

struct Gbm_Bo {
  uint32_t fd[4];
  uint32_t width[4];
  uint32_t height[4];
  uint32_t format[4];
  uint32_t pitch[4];
  uint32_t offset[4];
  uint64_t modifiers[4];
  uint32_t handle[4];
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
static bool isMaster = true;
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

    gbm_bo[i].fd[0] = gbm_bo_get_fd(gbm_bo[i].bo);
    if (gbm_bo[i].fd[0] < 0) {
      fprintf(stderr, "Failed to get fb for gbm bo\n");
      gbm_bo_destroy(gbm_bo[i].bo);
      return -1;
    }

    gbm_bo[i].handle[0] = gbm_bo_get_handle(gbm_bo[i].bo).u32;
    gbm_bo[i].pitch[0] = gbm_bo_get_stride(gbm_bo[i].bo);
    gbm_bo[i].offset[0] = 0;
    gbm_bo[i].width[0] = display_width;
    gbm_bo[i].height[0] = display_height;
    gbm_bo[i].format[0] = format;
    gbm_bo[i].modifiers[0] = 0;
    drmModeAddFB2(drmInfoPtr->fd, display_width, display_height, format, gbm_bo[i].handle, gbm_bo[i].pitch, gbm_bo[i].offset, &gbm_bo[i].fb_id, 0);
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
    fprintf(stderr, "Could not get connector from drm.\n");
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

  uint32_t bo_flags = GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT; // must need by egl
  //uint32_t bo_flags = GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT; // must need by egl
  //bo_flags |= GBM_BO_USE_FRONT_RENDERING;
  generate_gbm_bo(gbm_bo, display_callback_gbm.format, bo_flags);
  gbm_window = gbm_surface_create(gbm_display, display_width, display_height, DEFAULT_FORMAT, bo_flags);
  if (!gbm_window) {
    fprintf(stderr, "Could not create gbm window.\n");
    return -1;
  }
  if (isMaster && drmModeSetCrtc(drmInfoPtr->fd, crtcPtr->crtc_id, gbm_bo[0].fb_id, x_offset, y_offset, &connPtr->connector_id, 1, connModePtr) < 0) {
    fprintf(stderr, "Could not set fb to drm crtc.\n");
    return -1;
  }

  return 0;
}

/*
static int gbm_attach_display(int width, int height, int index) {
  //if (drmModeSetPlane(drmInfoPtr->fd, drmInfoPtr->plane_id, drmInfoPtr->crtc_id, gbm_bo[index].fb_id, 0, x_offset, y_offset, display_width, display_height, 0, 0, frame_width << 16,frame_height << 16) < 0) {
  int res = drmModeSetPlane(drmInfoPtr->fd, drmInfoPtr->plane_id, drmInfoPtr->crtc_id, gbm_bo[index].fb_id, 0, x_offset, y_offset, display_width, display_height, 0, 0, display_width << 16,display_height << 16);
  if (res < 0) {
    fprintf(stderr, "DRM: drmModeSetPlane() failed: %d.\n", res);
    return -1;
  }
  return 0;
}
*/

static void* gbm_get_display(const char* *device) {
  uint32_t format = wantHdr ? DEFAULT_FORMAT_10BIT : DEFAULT_FORMAT;
  display_callback_gbm.hdr_support = wantHdr ? true : false;

  if (gbm_display == NULL) {  
    drmInfoPtr = drm_init(NULL, format, wantHdr);
    if (drmInfoPtr == NULL && wantHdr) {
      display_callback_gbm.format = DEFAULT_FORMAT;
      drmInfoPtr = drm_init(NULL, display_callback_gbm.format, false);
      if (drmInfoPtr) {
        display_callback_gbm.hdr_support = false;
      }
    }
    if (drmInfoPtr == NULL) {
      fprintf(stderr, "Could not init drm device.\n");
      return NULL;
    }

    drm_magic_t magic;
    if (drmGetMagic(drmInfoPtr->fd, &magic) == 0) {
      if (drmAuthMagic(drmInfoPtr->fd, magic) != 0) {
        isMaster = false;
        //display_callback_gbm.display_put_to_screen = gbm_attach_display;
        fprintf(stderr, "DRM: drmSetMaster() failed.\n");
        return NULL;
      }
    }

    gbm_display = gbm_create_device(drmInfoPtr->fd);
    if (!gbm_display) {
      fprintf(stderr, "Could not create gbm display.\n");
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

static int gbm_display_done(int width, int height, int index) {
  return drm_flip_buffer(drmInfoPtr->fd, crtcPtr->crtc_id, gbm_bo[index].fb_id);
}

static int gbm_vsync_loop(bool *exit, int *index, void(*loop_pre)(void), void(*loop_post)(void)) {
  while (!(*exit)) {
    if (drm_flip_buffer(drmInfoPtr->fd, crtcPtr->crtc_id, gbm_bo[*index].fb_id) < 0) {
      return -1;
    }

    loop_pre();
    loop_post();
  }
  
  return 0;
}

static void gbm_clear_image_cache () {
  for (int i = 0; i < MAX_FB_NUM; i++) {
    if (gbm_bo[i].fb_id != 0) {
      drmModeRmFB(drmInfoPtr->fd, gbm_bo[i].fb_id);
      close(gbm_bo[i].fd[0]);
      gbm_bo_destroy(gbm_bo[i].bo);
      gbm_bo[i].handle[0] = 0;
      gbm_bo[i].fb_id = 0;
      gbm_bo[i].bo = NULL;
    }
  }
}

static void gbm_close_display () {
  drm_restore_display();
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
static void gbm_export_bo(struct Source_Buffer_Info buffers[MAX_FB_NUM], int *buffer_num, int *plane_num) {
  *buffer_num = MAX_FB_NUM;
  *plane_num = 1;
  for (int i = 0; i < *buffer_num; i++) {
    memcpy(&buffers[i], &gbm_bo[i], sizeof(buffers[i]));
  }
}

struct DISPLAY_CALLBACK display_callback_gbm = {
  .name = "gbm",
  .egl_platform = 0x31D7,
  .format = DEFAULT_FORMAT,
  .hdr_support = false,
  .display_get_display = gbm_get_display,
  .display_get_window = gbm_get_window,
  .display_close_display = gbm_close_display,
  .display_setup = gbm_setup,
  .display_setup_post = gbm_setup_post,
  .display_put_to_screen = gbm_display_done,
  .display_get_resolution = gbm_get_resolution,
  .display_change_cursor = gbm_change_cursor,
  .display_vsync_loop = gbm_vsync_loop,
  .display_exported_buffer_info = gbm_export_bo,
  .renders = EGL_RENDER,
};
