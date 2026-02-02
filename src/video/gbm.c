#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libdrm/drm_fourcc.h>
#include <libavutil/frame.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "convert.h"
#include "drm_base.h"
#include "render.h"
#include "video_internal.h"
#include "../util.h"

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

uint32_t bo_flags = GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT; // must need by egl

int generate_gbm_bo(int fd, struct _drm_buf gbm_buf[], int buffer_num, void *display, int width, int height, int src_fmt, uint64_t size[MAX_PLANE_NUM]) {
  struct Gbm_Bo *gbm_bo = (struct Gbm_Bo *)gbm_buf;
  struct gbm_device *gbm_display = (struct gbm_device *)display;
  int planes = -1;
  int bpp, buffer_multi, plane_num;

  uint32_t format = translate_format_to_drm(src_fmt, &bpp, &buffer_multi, &plane_num);

  for (int i = 0; i < buffer_num; i++) {
    struct gbm_bo *bo = gbm_bo_create(gbm_display, width, height, format, bo_flags);
    if (!bo) {
      fprintf(stderr, "Failed to create a gbm bo.\n");
      return -1;
    }
    gbm_bo[i].bo = bo;
    planes = gbm_bo_get_plane_count(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);

    for (int k = 0; k < planes;  k++) {
      gbm_bo[i].fd[k] = gbm_bo_get_fd_for_plane(bo, k);
      gbm_bo[i].handle[k] = gbm_bo_get_handle_for_plane(bo, k).u32;
      gbm_bo[i].pitch[k] = gbm_bo_get_stride_for_plane(bo, k);
      gbm_bo[i].offset[k] = gbm_bo_get_offset(bo, k);
      gbm_bo[i].width[k] = (k != 0 && format == GBM_FORMAT_YUV420) ? (int)(width / 2) : width;
      gbm_bo[i].height[k] = (k != 0 && (format == GBM_FORMAT_YUV420 || format == GBM_FORMAT_NV12)) ? (int)(height / 2) : height;
      gbm_bo[i].format[k] = format;
      gbm_bo[i].modifiers[k] = modifier;
      size[i] = gbm_bo[i].pitch[k] * gbm_bo[i].height[k];
    }
  }

  return planes;
}

int generate_gbm_buffer(int fd, struct _drm_buf gbm_buf[], int buffer_num, void *display, int width, int height, int src_fmt) {
  uint64_t size[MAX_PLANE_NUM];
  int planes = generate_gbm_bo(fd, gbm_buf, buffer_num, display, width, height, src_fmt, size);
  if (planes < 1) {
    fprintf(stderr, "Failed to create gbm buffer.\n");
    return -1;
  }

  for (int i = 0; i < buffer_num; i++) {
    int flags = 0;
    if (gbm_buf[i].modifiers[0] != DRM_FORMAT_MOD_INVALID) {
      flags = DRM_MODE_FB_MODIFIERS;
    }
    drmModeAddFB2WithModifiers(fd, width, height, gbm_buf[i].format[0], gbm_buf[i].handle, gbm_buf[i].pitch, gbm_buf[i].offset, gbm_buf[i].modifiers, &gbm_buf[i].fb_id, flags);
    if (!gbm_buf[i].fb_id) {
      fprintf(stderr, "Failed to create framebuffer from gbm buffer object.\n");
      gbm_bo_destroy((struct gbm_bo *)gbm_buf[i].data);
      return -1;
    }
  }

  return planes;
}

void* gbm_get_window(int fd, void * display, int width, int height, uint32_t format) {
  struct gbm_surface *gbm_window = gbm_surface_create((struct gbm_device *)display, width, height, format, bo_flags);
  if (!gbm_window) {
    fprintf(stderr, "Could not create gbm window.\n");
    return NULL;
  }

  return gbm_window;
}

void* gbm_get_display(int *fd) {
  int gbm_fd = *fd;
  bool has_fd = gbm_fd < 0 ? false : true;

  if (!has_fd) {
    char drmNode[64] = {'\0'};
    gbm_fd = get_drm_render_fd(drmNode);
    if (gbm_fd < 0) {
      fprintf(stderr, "Could not open device %s.\n", drmNode);
      return NULL;
    }
  }
  struct gbm_device *gbm_display = gbm_create_device(gbm_fd);
  if (!gbm_display) {
    if (!has_fd)
      close(gbm_fd);
    fprintf(stderr, "Could not create gbm display.\n");
    return NULL;
  }
  if (!has_fd)
    *fd = gbm_fd;

  return gbm_display;
}

void gbm_close_display (int gbm_fd, void *data, int buffer_num, void *display, void *window) {

  if (data) {
    struct Gbm_Bo *gbm_bo = (struct Gbm_Bo *)data;
    for (int i = 0; i < buffer_num; i++) {
      if (gbm_bo[i].bo != NULL) {
        gbm_bo_destroy(gbm_bo[i].bo);
        gbm_bo[i].bo = NULL;
      }
    }
  }
  if (window) {
    struct gbm_surface *gbm_window = (struct gbm_surface *)window;
    gbm_surface_destroy(gbm_window);
  }
  if (display) {
    struct gbm_device *gbm_display = (struct gbm_device *)display;
    gbm_device_destroy(gbm_display);
  }
  if (gbm_fd >= 0)
    close(gbm_fd);

  return;
}

int gbm_convert_image(struct Render_Image *image, struct _drm_buf *drm_buf, int drm_fd, int handle_num, int plane_num, int dst_fmt, uint64_t size[MAX_PLANE_NUM], uint64_t map_offset[MAX_PLANE_NUM]) {
  AVFrame * sframe = (AVFrame *)image->sframe.frame;
  struct gbm_bo *bo = (struct gbm_bo *)drm_buf[image->index].data;

  uint8_t *data_buffer[MAX_PLANE_NUM] = {0};
  void *mapped_handle[MAX_PLANE_NUM] = {0};

  for (int m = 0; m < handle_num; m++) {
    uint32_t stride = 0;
    void *data_ptr = gbm_bo_map(bo, 0, 0, drm_buf[image->index].width[m], drm_buf[image->index].height[m], GBM_BO_TRANSFER_READ_WRITE, &stride, &mapped_handle[m]);
    if (!data_ptr) {
      fprintf(stderr, "Could not map gbm to userspace.\n");
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
    gbm_bo_unmap(bo, mapped_handle[0]);
  } else {
    for (int m = 0; m < handle_num; m++) {
      gbm_bo_unmap(bo, mapped_handle[m]);
    }
  }

  return image->index;
}
