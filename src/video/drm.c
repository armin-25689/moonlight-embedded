
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libyuv/convert_from.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <sys/mman.h>

#include <Limelight.h>

#include "drm_base.h"
#include "drm_base_ffmpeg.h"
#include "ffmpeg.h"
#include "video.h"
#include "../loop.h"
#include "../util.h"

#define SLICES_PER_FRAME 4
#define FRAME_BUFFER_COUNT 2

static struct Drm_Info current_drm_info = {0},old_drm_info = {0};
static struct Soft_Mapping_Info sw_mapping[FRAME_BUFFER_COUNT] = {0};
static const char *drm_device = "/dev/dri/card0";
static void* ffmpeg_buffer = NULL;
static size_t ffmpeg_buffer_size = 0;
static int pipefd[2];
static int display_width = 0, display_height = 0, x_offset = 0, y_offset = 0;
static int frame_width, frame_height;
static int current_mapping = 0, next_mapping = 0;
static uint32_t fb_id = 0;
static uint32_t hdr_metadata_blob_id;

static bool useHDR = false;
static bool first_draw = true;

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
  // now only support nv12
  //uint32_t drmFormat = isYUV444 ? DRM_FORMAT_YUV444 : DRM_FORMAT_YUV420;

  //first convert to nv12
  uint32_t drmFormat;
  drmFormat = DRM_FORMAT_NV12;
  //bool threePlane = false;
  int bpc = 8;
/*
  switch (frame->format) {
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
    threePlane = true;
    break;
  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUVJ444P:
    threePlane = true;
    drmFormat = DRM_FORMAT_YUV444;
    break;
  default:
    drmFormat = DRM_FORMAT_NV12;
    break;
  }
*/
  // 2 is nvxx, 3 is yuv444
  current_mapping = next_mapping;
  next_mapping = (current_mapping + 1) % FRAME_BUFFER_COUNT;

  if (sw_mapping[current_mapping].handle == 0) {
    struct drm_mode_create_dumb createBuf = {};
    createBuf.width = frame->width;
    createBuf.height = frame->height * 2;
    createBuf.bpp = bpc;
    if (drmIoctl(current_drm_info.fd, DRM_IOCTL_MODE_CREATE_DUMB, &createBuf) < 0) {
      fprintf(stderr, "Could not create drm dumb\n");
      return false;
    }
    sw_mapping[current_mapping].handle = createBuf.handle;
    sw_mapping[current_mapping].pitch = createBuf.pitch;
    sw_mapping[current_mapping].size = createBuf.size;

    struct drm_mode_map_dumb mapBuf = {};
    mapBuf.handle = sw_mapping[current_mapping].handle;
    if (drmIoctl(current_drm_info.fd, DRM_IOCTL_MODE_MAP_DUMB, &mapBuf) < 0) {
      fprintf(stderr, "Could not map dumb\n");
      return false;
    }

    sw_mapping[current_mapping].mapping = (uint8_t*)mmap(NULL, sw_mapping[current_mapping].size, PROT_WRITE, MAP_SHARED, current_drm_info.fd, mapBuf.offset);
    if (sw_mapping[current_mapping].mapping == MAP_FAILED) {
      fprintf(stderr, "Could not map dumb to userspace\n");
      return false;
    }

    if (drmPrimeHandleToFD(current_drm_info.fd, sw_mapping[current_mapping].handle, O_CLOEXEC, &sw_mapping[current_mapping].primeFd) < 0) {
      fprintf(stderr, "Software drm mapping: drmPrimeHandleToFD() faild\n");
      return false;
    }
  }

  // We use a single dumb buffer for semi/fully planar formats because some DRM
  // drivers (i915, at least) don't support multi-buffer FBs.
  mappedFrame->nb_objects = 1;
  mappedFrame->objects[0].fd = sw_mapping[current_mapping].primeFd;
  mappedFrame->objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
  mappedFrame->objects[0].size = sw_mapping[current_mapping].size;

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
    plane->pitch = sw_mapping[current_mapping].pitch;
    plane->object_index = 0;
    plane->offset = i == 0 ? 0 : (layer->planes[layer->nb_planes - 1].offset + lastPlaneSize);
    offset[i] = plane->offset;
    pitch[i] = plane->pitch;
    plane_height[i] = (i == 0 || isYUV444) ? frame->height : (frame->height / 2);
    layer->nb_planes++;
    lastPlaneSize = plane->pitch * plane_height[i];
  }

  
  switch (frame->format) {
  case AV_PIX_FMT_YUVJ420P:
  case AV_PIX_FMT_YUV420P:
    I420ToNV12(frame->data[0], frame->linesize[0],
               frame->data[1], frame->linesize[1],
               frame->data[2], frame->linesize[2],
               sw_mapping[current_mapping].mapping, sw_mapping[current_mapping].pitch,
               sw_mapping[current_mapping].mapping + layer->planes[nb_planes[0]].offset + sw_mapping[current_mapping].pitch * frame->height, sw_mapping[current_mapping].pitch,
               frame->width, frame->height);
    break;
  case AV_PIX_FMT_NV12:
    for (int i = 0; i < 2; i++) {
      if (frame->data[i] != NULL) {
        if (frame->linesize[i] == pitch[i]) {
          memcpy(sw_mapping[current_mapping].mapping + offset[i], frame->data[i], pitch[i] * plane_height[i]);
        }
        else {
          for (int j = 0; j < plane_height[i]; j++) {
            memcpy(sw_mapping[current_mapping].mapping + (j * pitch[i]) + offset[i], frame->data[i] + (j * frame->linesize[i]), (frame->linesize[i] < (int)pitch[i] ? frame->linesize[i] : (int)pitch[i]));
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

static int add_fb_for_frame(AVFrame *frame, uint32_t *new_fb_id) {
  AVDRMFrameDescriptor* drmFrame;
  AVDRMFrameDescriptor mappedFrame = {0};
  uint32_t handles[4] = {};
  uint32_t pitches[4] = {};
  uint32_t offsets[4] = {};
  uint64_t modifiers[4] = {};
  uint32_t flags = 0;

  if (frame->format != AV_PIX_FMT_DRM_PRIME) {
    if (!map_software_frame(frame, &mappedFrame)) {
      fprintf(stderr, "Could not map software frame to drm frame\n");
      return -1;
    }
    else {
      drmFrame = &mappedFrame;
    }
  }
  else {
    drmFrame = (AVDRMFrameDescriptor*)frame->data[0];
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
    if (modifiers[i] != DRM_FORMAT_MOD_INVALID && frame->format == AV_PIX_FMT_DRM_PRIME) {
      flags |= DRM_MODE_FB_MODIFIERS;
    }
  }

  // Create a frame buffer object from the PRIME buffer
  // NB: It is an error to pass modifiers without DRM_MODE_FB_MODIFIERS set.
  if (drmModeAddFB2WithModifiers(current_drm_info.fd, frame->width, frame->height, drmFrame->layers[0].format, handles, pitches, offsets, (flags & DRM_MODE_FB_MODIFIERS) ? modifiers : NULL, new_fb_id, flags) < 0) {
    fprintf(stderr, "Could not success drmModeAddFB2WithModifiers(), may be drm format is not supported(only NV12)\n");
    return -1;
  }

  return 0;
}

static int choose_color_config (AVFrame *frame) {
  bool changed = false;
  if (first_draw) {
    int colorspace = ffmpeg_get_frame_colorspace(frame);
    bool fullRange = ffmpeg_is_frame_full_range(frame) == 1 ? true : false;
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
  }
  first_draw = false;
  if (changed) {
    return 0;
  }
  else {
    fprintf(stderr, "Could not set color range for drm\n");
    return -1;
  }
}

static int frame_handle (int pipefd, void *data) {
  AVFrame* frame = NULL;
  while (read(pipefd, &frame, sizeof(void*)) > 0);
  if (frame) {
    if (first_draw)
      choose_color_config(frame);

    uint32_t last_fb_id = fb_id;
    if (add_fb_for_frame(frame, &fb_id) < 0) {
      fprintf(stderr, "Could not success add_fb_for_frame()\n");
      return LOOP_RETURN;
    }

    if (useHDR)
      set_drm_hdr_metadata (current_drm_info.fd, true);
    if (drmModeSetPlane(current_drm_info.fd, current_drm_info.plane_id, current_drm_info.crtc_id, fb_id, 0, x_offset, y_offset, display_width, display_height, 0, 0, frame->width << 16,frame->height << 16) < 0) {
      fprintf(stderr, "Could not success drmModeSetPlane()\n");
      drmModeRmFB(current_drm_info.fd, fb_id);
      fb_id = 0;
      if (last_fb_id != 0)
	drmModeRmFB(current_drm_info.fd, last_fb_id);
      return LOOP_RETURN;
    }

    // Free the previous FB object which has now been superseded
    drmModeRmFB(current_drm_info.fd, last_fb_id);
  }

  return LOOP_OK;
}

int drm_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);

  bool usehdr = false;
  struct Drm_Info *drmInfoPtr = drm_init(drm_device, DRM_FORMAT_NV12, usehdr);
  if (drmInfoPtr == NULL) {
   fprintf(stderr, "Couldn't initialize drm\n");
   return -1;
  }
  memcpy(&current_drm_info, drmInfoPtr, sizeof(struct Drm_Info));
  drmInfoPtr = get_drm_info(false);
  memcpy(&old_drm_info, drmInfoPtr, sizeof(struct Drm_Info));

  frame_width = width;
  frame_height = height;
  display_width = current_drm_info.width;
  display_height = current_drm_info.height;
  //convert_display(&width, &height, &display_width, &display_height, &x_offset, &y_offset);

  if (!drm_is_support_yuv444(AV_PIX_FMT_YUV444P))
    isYUV444 = false;
  // test not success 
  // only support nv12 and p010
  if (ffmpeg_init_drm_hw_ctx(drm_device, isYUV444 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_NV12) < 0)
    return -1;

  int avc_flags;
  avc_flags = SLICE_THREADING;
  avc_flags |= DRM_RENDER;
  if (ffmpeg_init(videoFormat, frame_width, frame_height, avc_flags, FRAME_BUFFER_COUNT, SLICES_PER_FRAME) < 0) {
   fprintf(stderr, "Couldn't initialize video decoding\n");
   return -1;
  }
  if (pipe(pipefd) == -1) {
    fprintf(stderr, "Can't create communication channel between threads\n");
    return -2;
  }

  loop_add_fd(pipefd[0], &frame_handle, EPOLLIN);
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
  return 0;
}

void drm_cleanup() {
  for (int i = 0; i < FRAME_BUFFER_COUNT; i++) {
    if (sw_mapping[i].handle != 0) {
      close(sw_mapping[i].primeFd);
      munmap(sw_mapping[i].mapping, sw_mapping[i].size);
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

  //need close pipefd
  //close(pipefd);

  drm_close();
}

int drm_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  PLENTRY entry = decodeUnit->bufferList;
  int length = 0;

  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);

  while (entry != NULL) {
    memcpy(ffmpeg_buffer+length, entry->data, entry->length);
    length += entry->length;
    entry = entry->next;
  }

  int err = ffmpeg_decode(ffmpeg_buffer, length);
  if (err < 0) {
    // exit
  }

  AVFrame* frame = ffmpeg_get_frame(true);

  if (frame != NULL)
    write(pipefd[1], &frame, sizeof(void*));

  return DR_OK;
}
  

DECODER_RENDERER_CALLBACKS decoder_callbacks_drm = {
  .setup = drm_setup,
  .cleanup = drm_cleanup,
  .submitDecodeUnit = drm_submit_decode_unit,
  .capabilities = CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME) | CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC | CAPABILITY_DIRECT_SUBMIT,
};
