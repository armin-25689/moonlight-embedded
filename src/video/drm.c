
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <sys/mman.h>

#include <Limelight.h>

#include "drm_base.h"
#include "drm_base_ffmpeg.h"
#include "video.h"
#include "video_internal.h"

static struct Drm_Info current_drm_info = {0};
static struct Soft_Mapping_Info sw_mapping[MAX_FB_NUM] = {0};
static const char *drm_device = "/dev/dri/card0";
static int display_width, display_height, screen_width, screen_height, frame_width, frame_height;
static int current = 0, next = 0;
static uint32_t fb_id = 0;
static uint32_t plane_format = 0;
static uint32_t hdr_metadata_blob_id;
static int pix_fmt = -1;

static int set_drm_hdr_metadata (int fd, bool enable) {
  if (current_drm_info.conn_hdr_metadata_prop_id != 0 && enable) {
    if (hdr_metadata_blob_id != 0) {
      drmModeDestroyPropertyBlob(fd, hdr_metadata_blob_id);
      hdr_metadata_blob_id = 0;
    }

    struct Hdr_Output_Metadata outputMetadata;
    SS_HDR_METADATA sunshineHdrMetadata;

    if (!LiGetHdrMetadata(&sunshineHdrMetadata)) {
      memset(&sunshineHdrMetadata, 0, sizeof(sunshineHdrMetadata));
    }

    outputMetadata.metadata_type = 0; // HDMI_STATIC_METADATA_TYPE1
    outputMetadata.hdmi_metadata_type1.eotf = 2; // SMPTE ST 2084
    outputMetadata.hdmi_metadata_type1.metadata_type = 0; // Static Metadata Type 1
    for (int i = 0; i < 3; i++) {
      outputMetadata.hdmi_metadata_type1.display_primaries[i].x = sunshineHdrMetadata.displayPrimaries[i].x;
      outputMetadata.hdmi_metadata_type1.display_primaries[i].y = sunshineHdrMetadata.displayPrimaries[i].y;
    }
    outputMetadata.hdmi_metadata_type1.white_point.x = sunshineHdrMetadata.whitePoint.x;
    outputMetadata.hdmi_metadata_type1.white_point.y = sunshineHdrMetadata.whitePoint.y;
    outputMetadata.hdmi_metadata_type1.max_display_mastering_luminance = sunshineHdrMetadata.maxDisplayLuminance;
    outputMetadata.hdmi_metadata_type1.min_display_mastering_luminance = sunshineHdrMetadata.minDisplayLuminance;
    outputMetadata.hdmi_metadata_type1.max_cll = sunshineHdrMetadata.maxContentLightLevel;
    outputMetadata.hdmi_metadata_type1.max_fall = sunshineHdrMetadata.maxFrameAverageLightLevel;

    if (drmModeCreatePropertyBlob(fd, &outputMetadata, sizeof(outputMetadata), &hdr_metadata_blob_id) < 0) {
      hdr_metadata_blob_id = 0;
      fprintf(stderr, "Could not create hdr property blob for connector\n");
      return -1;
    }

    if (drmModeObjectSetProperty(fd, current_drm_info.connector_id, DRM_MODE_OBJECT_CONNECTOR, current_drm_info.conn_hdr_metadata_prop_id, hdr_metadata_blob_id) != 0) {
      fprintf(stderr, "Could not enable hdr mode\n");
      return -1;
    }
  }
  else if (current_drm_info.conn_hdr_metadata_prop_id != 0 && !enable) {
    drmModeObjectSetProperty(fd, current_drm_info.connector_id, DRM_MODE_OBJECT_CONNECTOR, current_drm_info.conn_hdr_metadata_prop_id, 0);
  }

  return 0;
}

static bool map_software_frame(AVFrame *frame, AVDRMFrameDescriptor *mappedFrame) {
  //first convert to nv12
  uint32_t drmFormat;
  int bpc = 8;
  switch (pix_fmt) {
  case AV_PIX_FMT_XV30:
    drmFormat = DRM_FORMAT_XVYU2101010;
    bpc = 16;
    break;
  case AV_PIX_FMT_VUYX:
    drmFormat = DRM_FORMAT_XVUY8888;
    break;
  case AV_PIX_FMT_NV12:
    drmFormat = DRM_FORMAT_NV12;
    break;
  case AV_PIX_FMT_NV21:
    drmFormat = DRM_FORMAT_NV21;
    break;
  case AV_PIX_FMT_P010:
    drmFormat = DRM_FORMAT_P010;
    bpc = 16;
    break;
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P:
    drmFormat = DRM_FORMAT_YUV420;
    break;
  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUVJ444P:
    drmFormat = DRM_FORMAT_YUV444;
    break;
  case AV_PIX_FMT_YUV444P10:
  case AV_PIX_FMT_YUVJ444P10:
    drmFormat = DRM_FORMAT_Q410;
    bpc = 16;
    break;
  default:
    fprintf(stderr, "DRM: Unkown av pix format(%d) here.\n", pix_fmt);
    return -1;
    break;
  }
  bool found = false;
  for (int i = 0; i < NEEDED_DRM_FORMAT_NUM; i++) {
    if (drmFormat == current_drm_info.plane_formats[i]) {
      found = true;
      break;
    }
  }
  if (!found) {
    struct Drm_Info drm_info = {0};
    drm_info.fd = current_drm_info.fd;
    drm_info.fd = current_drm_info.crtc_index;
    if (drm_get_plane_info(&drm_info, drmFormat) < 0) {
      if (drm_get_plane_info(&current_drm_info, DRM_FORMAT_NV12) < 0) {
        fprintf(stderr, "No plane here.\n");
        return -1;
      }
      drmFormat = DRM_FORMAT_NV12;
    }
    else {
      convert_format_op = NULL;
      current_drm_info.plane_id = drm_info.plane_id;
      current_drm_info.plane_color_encoding_prop_id  = drm_info.plane_color_encoding_prop_id;
      current_drm_info.plane_color_range_prop_id  = drm_info.plane_color_range_prop_id;
      memcpy(current_drm_info.plane_formats, drm_info.plane_formats, sizeof(drm_info.plane_formats));
    }
  }

  // 2 is nvxx, 3 is yuv444
  current = next;
  next = (current + 1) % MAX_FB_NUM;

  if (sw_mapping[current].handle == 0) {
    struct drm_mode_create_dumb createBuf = {};
    createBuf.width = frame_width;
    createBuf.height = frame_height * 2;
    createBuf.bpp = bpc;
    if (drmIoctl(current_drm_info.fd, DRM_IOCTL_MODE_CREATE_DUMB, &createBuf) < 0) {
      fprintf(stderr, "Could not create drm dumb\n");
      return false;
    }
    sw_mapping[current].handle = createBuf.handle;
    sw_mapping[current].pitch = createBuf.pitch;
    sw_mapping[current].size = createBuf.size;

    struct drm_mode_map_dumb mapBuf = {};
    mapBuf.handle = sw_mapping[current].handle;
    if (drmIoctl(current_drm_info.fd, DRM_IOCTL_MODE_MAP_DUMB, &mapBuf) < 0) {
      fprintf(stderr, "Could not map dumb\n");
      return false;
    }

    sw_mapping[current].buffer = (uint8_t*)mmap(NULL, sw_mapping[current].size, PROT_WRITE, MAP_SHARED, current_drm_info.fd, mapBuf.offset);
    if (sw_mapping[current].buffer == MAP_FAILED) {
      fprintf(stderr, "Could not map dumb to userspace\n");
      return false;
    }

    if (drmPrimeHandleToFD(current_drm_info.fd, sw_mapping[current].handle, O_CLOEXEC, &sw_mapping[current].primeFd) < 0) {
      fprintf(stderr, "Software drm mapping: drmPrimeHandleToFD() faild\n");
      return false;
    }
  }

  // We use a single dumb buffer for semi/fully planar formats because some DRM
  // drivers (i915, at least) don't support multi-buffer FBs.
  mappedFrame->nb_objects = 1;
  mappedFrame->objects[0].fd = sw_mapping[current].primeFd;
  mappedFrame->objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
  mappedFrame->objects[0].size = sw_mapping[current].size;

  mappedFrame->nb_layers = 1;

  AVDRMLayerDescriptor *layer = &mappedFrame->layers[0];
  layer->format = drmFormat;

  int lastPlaneSize = 0;
  int nb_planes[4] = {0};
  int offset[4] = {0};
  int pitch[4] = {0};
  int plane_height[4] = {0};
  for (int i = 0; i < 2; i++) {
    AVDRMPlaneDescriptor *plane = &layer->planes[layer->nb_planes];
    nb_planes[i] = layer->nb_planes;
    plane->pitch = sw_mapping[current].pitch;
    plane->object_index = 0;
    plane->offset = i == 0 ? 0 : (layer->planes[layer->nb_planes - 1].offset + lastPlaneSize);
    offset[i] = plane->offset;
    pitch[i] = plane->pitch;
    plane_height[i] = (i == 0 || isYUV444) ? frame_height : (frame_height / 2);
    layer->nb_planes++;
    lastPlaneSize = plane->pitch * plane_height[i];
  }

  
  switch (pix_fmt) {
  case AV_PIX_FMT_YUVJ420P:
  case AV_PIX_FMT_YUV420P:
    I420ToNV12(frame->data[0], frame->linesize[0],
               frame->data[1], frame->linesize[1],
               frame->data[2], frame->linesize[2],
               sw_mapping[current].buffer, sw_mapping[current].pitch,
               sw_mapping[current].buffer + layer->planes[nb_planes[0]].offset + sw_mapping[current].pitch * frame->height, sw_mapping[current].pitch,
               frame->width, frame->height);
    break;
  case AV_PIX_FMT_NV12:
    for (int i = 0; i < 2; i++) {
      if (frame->data[i] != NULL) {
        if (frame->linesize[i] == pitch[i]) {
          memcpy(sw_mapping[current].buffer + offset[i], frame->data[i], pitch[i] * plane_height[i]);
        }
        else {
          for (int j = 0; j < plane_height[i]; j++) {
            memcpy(sw_mapping[current].buffer + (j * pitch[i]) + offset[i], frame->data[i] + (j * frame->linesize[i]), (frame->linesize[i] < (int)pitch[i] ? frame->linesize[i] : (int)pitch[i]));
          }
        }
      }
    }
    break;
  default:
    return false;
    break;
  }
  
  return true;
}

static int add_fb_for_frame(uint8_t *frame_data[3], uint32_t *new_fb_id) {
  AVDRMFrameDescriptor* drmFrame;
  AVDRMFrameDescriptor mappedFrame = {0};
  uint32_t handles[4] = {};
  uint32_t pitches[4] = {};
  uint32_t offsets[4] = {};
  uint64_t modifiers[4] = {};
  uint32_t flags = 0;

  if (pix_fmt != AV_PIX_FMT_DRM_PRIME) {
    if (!map_software_frame(frame, &mappedFrame)) {
      fprintf(stderr, "Could not map software frame to drm frame\n");
      return -1;
    }
    else {
      drmFrame = &mappedFrame;
    }
  }
  else {
    drmFrame = (AVDRMFrameDescriptor*)frame_data[0];
  }

  if (drmFrame->nb_layers != 1) {
    fprintf(stderr, "Could not convert frame to fd\n");
    return -1;
  }

  const AVDRMLayerDescriptor *layer = &drmFrame->layers[0];
  for (int i = 0; i < layer->nb_planes; i++) {
    const AVDRMObjectDescriptor *object = &drmFrame->objects[layer->planes[i].object_index];
    if (drmPrimeFDToHandle(current_drm_info.fd, object->fd, &handles[i]) < 0) {
      fprintf(stderr, "Could not success drmPrimeFDToHandle()\n");
      return -1;
    }
    pitches[i] = layer->planes[i].pitch;
    offsets[i] = layer->planes[i].offset;
    modifiers[i] = object->format_modifier;

    // Pass along the modifiers to DRM if there are some in the descriptor
    if (modifiers[i] != DRM_FORMAT_MOD_INVALID && pix_fmt == AV_PIX_FMT_DRM_PRIME) {
      flags |= DRM_MODE_FB_MODIFIERS;
    }
  }

  // Create a frame buffer object from the PRIME buffer
  // NB: It is an error to pass modifiers without DRM_MODE_FB_MODIFIERS set.
  if (drmModeAddFB2WithModifiers(current_drm_info.fd, frame_width, frame_height, drmFrame->layers[0].format, handles, pitches, offsets, (flags & DRM_MODE_FB_MODIFIERS) ? modifiers : NULL, new_fb_id, flags) < 0) {
    fprintf(stderr, "Could not success drmModeAddFB2WithModifiers(), may be drm format is not supported(only NV12)\n");
    return -1;
  }

  return 0;
}

static int drm_choose_color_config (struct Render_Config *config) {
  pix_fmt = config.pix_fmt;
  bool changed = false;
  int colorspace = config->color_space;
  bool fullRange = config->full_color_range == 1 ? true : false;
  const char *drmColorEncoding = colorspace == COLORSPACE_REC_2020 ? "ITU-R BT.2020 YCbCr" : (colorspace == COLORSPACE_REC_709 ? "ITU-R BT.709 YCbCr" : "ITU-R BT.601 YCbCr");
  const char *drmColorRange = fullRange ? "YCbCr full range" : "YCbCr limited range";
  drmModePropertyPtr prop = drmModeGetProperty(current_drm_info.fd, current_drm_info.plane_color_range_prop_id);
  if (!prop) {
    fprintf(stderr, "Could not get plane color range prop\n");
  }
  else {
    for (int i = 0; i < prop->count_enums; i++) {
      if (strcmp(drmColorRange, prop->enums[i].name) == 0) {
        if (drmModeObjectSetProperty(current_drm_info.fd, current_drm_info.plane_id, DRM_MODE_OBJECT_PLANE, current_drm_info.plane_color_range_prop_id, prop->enums[i].value) == 0) {
          changed = true;
          break;
        }
      }
    }
    drmModeFreeProperty(prop);
  }

  prop = NULL;
  prop = drmModeGetProperty(current_drm_info.fd, current_drm_info.plane_color_encoding_prop_id);
  if (!prop) {
    fprintf(stderr, "Could not get plane color range prop\n");
  }
  else {
    for (int i = 0; i < prop->count_enums; i++) {
      if (strcmp(drmColorEncoding, prop->enums[i].name) == 0) {
        if (drmModeObjectSetProperty(current_drm_info.fd, current_drm_info.plane_id, DRM_MODE_OBJECT_PLANE, current_drm_info.plane_color_encoding_prop_id, prop->enums[i].value) == 0) {
          changed = true;
          break;
        }
      }
    }
    drmModeFreeProperty(prop);
  }
  if (changed) {
    return 0;
  }
  else {
    fprintf(stderr, "Could not set color range for drm\n");
    return -1;
  }
}


static int drm_draw(union Render_Image image) { 
  uint32_t last_fb_id = fb_id;
  if (add_fb_for_frame(image.frame_data, &fb_id) < 0) {
    fprintf(stderr, "Could not success add_fb_for_frame()\n");
    return -1;
  }

  if (useHdr) {
    set_drm_hdr_metadata (current_drm_info.fd, true);
  }
  if (drmModeSetPlane(current_drm_info.fd, current_drm_info.plane_id, current_drm_info.crtc_id, fb_id, 0, x_offset, y_offset, display_width, display_height, 0, 0, frame_width << 16,frame_height << 16) < 0) {
    fprintf(stderr, "Could not success drmModeSetPlane()\n");
    drmModeRmFB(current_drm_info.fd, fb_id);
    fb_id = 0;
    return -1;
  }

  // Free the previous FB object which has now been superseded
  if (last_fb_id != 0) {
    drmModeRmFB(current_drm_info.fd, last_fb_id);
  }

  return 0;
}

int drm_setup(int width, int height, int drFlags) {

  frame_width = width;
  frame_height = height;
  display_width = current_drm_info.width;
  display_height = current_drm_info.height;
  screen_width = current_drm_info.width;
  screen_height = current_drm_info.height;
//  convert_display(&width, &height, &display_width, &display_height, &x_offset, &y_offset);
  x_offset = 0;
  y_offset = 0;

  // test not success 
  // only support nv12 and p010
/*
  if (ffmpeg_init_drm_hw_ctx(drm_device, isYUV444 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_NV12) < 0)
    return -1;
*/

  return 0;
}

static void* drm_get_display(const char* *device) {
  uint32_t format = (wantYuv444 && wantHdr) ? DRM_FORMAT_Y410 : (wantYuv444 ? DRM_FORMAT_YUV444 : (wantHdr ? DRM_FORMAT_P010 : DRM_FORMAT_NV12));
  display_callback_drm.hdr_support = wantHdr ? true : false;

  struct Drm_Info *drmInfoPtr = drm_init(drm_device, format, wantHdr);
  if (drmInfoPtr == NULL) {
    if (drmInfoPtr == NULL && (wantHdr || wantYuv444)) {
      format = DRM_FORMAT_NV12;
      drmInfoPtr = drm_init(NULL, format, false);
      if (drmInfoPtr) {
        display_callback_drm.hdr_support = false;
        supportedVideoFormat &= ~VIDEO_FORMAT_MASK_YUV444;
      }
    }
    if (drmInfoPtr == NULL) {
      fprintf(stderr, "Could not init drm device.");
      return NULL;
    }
  }
  plane_format = format;

  memcpy(&current_drm_info, drmInfoPtr, sizeof(struct Drm_Info));

  *device = "/dev/dri/renderD128";
  return &current_drm_info;
}

void drm_cleanup() {
  for (int i = 0; i < MAX_FB_NUM; i++) {
    if (sw_mapping[i].handle != 0) {
      close(sw_mapping[i].primeFd);
      munmap(sw_mapping[i].buffer, sw_mapping[i].size);
      struct drm_mode_destroy_dumb destroyBuf = {0};
      destroyBuf.handle = sw_mapping[i].handle;
      drmIoctl(current_drm_info.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyBuf);
    }
  }
  if (hdr_metadata_blob_id != 0) {
    drmModeDestroyPropertyBlob(current_drm_info.fd, hdr_metadata_blob_id);
    hdr_metadata_blob_id = 0;
  }
  if (fb_id != 0) {
    drmModeRmFB(current_drm_info.fd, fb_id);
    fb_id = 0;
  }

  drm_close();
}

static void drm_setup_post(void *data) {};
static void drm_change_cursor(const char *op) {};

static void* drm_get_window() {
  return NULL;
}

static void drm_get_resolution(int *width, int *height) {
  *width = screen_width;
  *height = screen_height;
  return;
}

static int drm_display_done(int width, int height, int index) {
  return drm_flip_buffer(drmInfoPtr->fd, crtcPtr->crtc_id, gbm_bo[index].fb_id);
}

static int drm_render_init(struct Render_Init_Info *paras) {
  frame_width = paras->frame_width;
  frame_height = paras->frame_height;

  return 0;
}
static void drm_render_destroy() {};
static int drm_render_create(struct Render_Init_Info *paras) { return 0; };

struct DISPLAY_CALLBACK display_callback_drm = {
  .name = "drm",
  .egl_platform = NOT_CARE,
  .format = NOT_CARE,
  .hdr_support = false,
  .display_get_display = drm_get_display,
  .display_get_window = drm_get_window,
  .display_close_display = drm_cleanup,
  .display_setup = drm_setup,
  .display_setup_post = drm_setup_post,
  .display_put_to_screen = drm_display_done,
  .display_get_resolution = drm_get_resolution,
  .display_change_cursor = drm_change_cursor,
  .display_vsync_loop = NULL,
  .display_exported_buffer_info = NULL,
  .renders = DRM_RENDER,
};

struct RENDER_CALLBACK drm_render = {
  .name = "drm",
  .display_name = "drm",
  .is_hardaccel_support = false,
  .render_type = DRM_RENDER,
  .decoder_type = SOFTWARE,
  .data = NULL,
  .render_create = drm_render_create,
  .render_init = drm_render_init,
  .render_sync_config = drm_choose_color_config,
  .render_draw = drm_draw,
  .render_destroy = drm_render_destroy,
};
