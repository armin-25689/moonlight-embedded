/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/hwcontext_vulkan.h>
#ifdef HAVE_VAAPI
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#endif

#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <Limelight.h>

#include "render.h"
#include "video.h"
#include "video_internal.h"
#include "../util.h"

#define MAX_SURFACES 16

// prepare for high444 profile. not support now
#ifndef VAProfileH264High444
#define VAProfileH264High444 99
#endif

struct Decoder_Context {
  const int ffmpeg_type;
  const int ffmpeg_fmt;
  const int ffmpeg_src_type;
  const int ffmpeg_src_fmt;
  AVBufferRef* src_device_ref;
  AVBufferRef* src_frames_ref;
  int (*set_opts) (AVDictionary **opts);
  int (*get_supported_format) (AVBufferRef* ref);
  bool (*validate_test) (void *nativeDisplay);
  void (*unexport_frame) (void *data);
  int (*export_frame) (AVBufferRef *device_ref, AVFrame *frame, void *descriptor, int render_type, struct Source_Buffer_Info *buffer_info, int *layers, int *planes);
  void (*clear_resource) (void);
};

#define sw_format_slot 9
struct {
  enum AVPixelFormat yuv444;
  enum AVPixelFormat yuv444_10;
  enum AVPixelFormat yuv420;
  enum AVPixelFormat yuv420_10;
} static hw_sw_format;
static int hwSupportedFormat = 0;

static struct Decoder_Context *decontext = NULL;
static AVBufferRef* device_ref = NULL;
static AVBufferRef* frames_ctx_ref = NULL;
static union {
  VADRMPRIMESurfaceDescriptor vaapi_descriptors[MAX_FB_NUM];
  AVFrame* drm_descriptors[MAX_FB_NUM];
} descriptors;
static void *primeDescriptors[MAX_FB_NUM] = {0};

static inline int get_supported_format (int fmt) {
  int supported_format = fmt;
  if (hw_sw_format.yuv444 < 0) {
    supported_format &= ~(VIDEO_FORMAT_AV1_HIGH8_444 | VIDEO_FORMAT_H265_REXT8_444 | VIDEO_FORMAT_H264_HIGH8_444);
  }
  if (hw_sw_format.yuv444_10 < 0) {
    supported_format &= ~(VIDEO_FORMAT_AV1_HIGH10_444 | VIDEO_FORMAT_H265_REXT10_444);
  }
  if (hw_sw_format.yuv420_10 < 0) {
    supported_format &= ~(VIDEO_FORMAT_AV1_MAIN10 | VIDEO_FORMAT_H265_MAIN10);
  }
  return supported_format;
}

static inline enum AVPixelFormat get_format_from_slot (bool useHDR, bool yuv444) {
  if (yuv444 && useHDR)
    return hw_sw_format.yuv444_10;
  else if (yuv444)
    return hw_sw_format.yuv444;
  else if (useHDR)
    return hw_sw_format.yuv420_10;
  else
    return hw_sw_format.yuv420;
}

/*
static inline bool is_two_plane_swfmt (bool hdr, bool yuv444) {
  static int lastfmt = -2;
  static bool twoplane;
  enum AVPixelFormat fmt = get_format_from_slot(hdr, yuv444);
  if (lastfmt == fmt)
    return twoplane;

  switch (fmt) {
  case AV_PIX_FMT_NV12:
  case AV_PIX_FMT_P010:
  case AV_PIX_FMT_P410:
  case AV_PIX_FMT_NV24:
  case AV_PIX_FMT_NV16:
  case AV_PIX_FMT_P016:
  case AV_PIX_FMT_P416:
    twoplane = true;
    break;
  default:
    twoplane = false;
    break;
  }
  lastfmt = fmt;
  return twoplane;
}
*/

static int vulkan_get_format(AVBufferRef *device_ref) {
  enum AVPixelFormat yuv420[] = { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE };
  enum AVPixelFormat yuv42010[] = { AV_PIX_FMT_P010, AV_PIX_FMT_NONE };
  enum AVPixelFormat yuv444[] = { AV_PIX_FMT_VUYX, AV_PIX_FMT_YUV444P, AV_PIX_FMT_NONE };
  enum AVPixelFormat yuv44410[] = { AV_PIX_FMT_XV30, AV_PIX_FMT_YUV444P10, AV_PIX_FMT_NONE };

  AVHWFramesConstraints *constraints = av_hwdevice_get_hwframe_constraints(device_ref, NULL);
  if (!constraints)
    return 0;

  enum AVPixelFormat *fmt[] = { yuv420, yuv444, yuv42010, yuv44410 };
  enum AVPixelFormat *store[] = { &hw_sw_format.yuv420, &hw_sw_format.yuv444, &hw_sw_format.yuv420_10, &hw_sw_format.yuv444_10 };
  for (int k = 0; k < 4; k++) {
    for (int j = 0; fmt[k][j] != AV_PIX_FMT_NONE; j++) {
      for (int i = 0; constraints->valid_sw_formats[i] != AV_PIX_FMT_NONE; i++) {
        if (constraints->valid_sw_formats[i] == fmt[k][j]) {
          *store[k] = fmt[k][j];
          goto next_fmt;
        }
      }
    }
    next_fmt:;
  }

  av_hwframe_constraints_free(&constraints);

  int supported_format = VIDEO_FORMAT_MASK_H264 | VIDEO_FORMAT_MASK_H265 | VIDEO_FORMAT_MASK_AV1;
  supported_format = get_supported_format (supported_format);
  return supported_format;
}

static bool vulkan_validate_test(void *nativeDisplay) {
  bool support = false;
  AVBufferRef* device_ref = NULL;

  if (av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_VULKAN, NULL, NULL, 0) < 0)
    return false;

  vulkan_get_format(device_ref);
  int selected_fmt = get_format_from_slot(wantHdr, wantYuv444);
  if (selected_fmt != AV_PIX_FMT_NONE) {
      support = true;
  }

  av_buffer_unref(&device_ref);

  return support;
}

static void vulkan_free_render_images(void *descriptor) {
  AVFrame *frame = (AVFrame *) descriptor;
  if (frame->buf[0])
    av_buffer_unref(&frame->buf[0]);
  frame->data[0] = NULL;
  frame->width = 0;
  frame->height = 0;
  return;
}

static int vulkan_export_render_images(AVBufferRef *device_ref, AVFrame *frame, void *descriptor, int render_type, struct Source_Buffer_Info *buffer_info, int *layers, int *plane_num) {

  AVFrame *drm_frame = (AVFrame *) descriptor;
  drm_frame->format = AV_PIX_FMT_DRM_PRIME;
  if (av_hwframe_map(drm_frame, frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) != 0) {
    fprintf(stderr, "Failed to map vulkan frame to drm frame.\n");
    return -1;
  }

  AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)drm_frame->data[0];
  int planes = 0;
  int layer_num = 0;
  for (int i = 0; i < desc->nb_layers; i++) {
    for (int j = 0; j < desc->layers[i].nb_planes; j++) {
      if (planes > 3) {
        vulkan_free_render_images(descriptor);
        fprintf(stderr, "Too many planes from drm prime.\n");
        return -1;
      }
      buffer_info->format[planes] = desc->layers[i].format;
      // use pixel width
      buffer_info->width[planes] = planes == 0 ? drm_frame->width : (isYUV444 ? drm_frame->width : drm_frame->width / 2);
      buffer_info->height[planes] = planes == 0 ? drm_frame->height : (isYUV444 ? drm_frame->height : drm_frame->height / 2);
      buffer_info->fd[planes] = desc->objects[desc->layers[i].planes[j].object_index].fd;
      buffer_info->offset[planes] = desc->layers[i].planes[j].offset;
      buffer_info->stride[planes] = desc->layers[i].planes[j].pitch;
      buffer_info->modifiers[planes] = desc->objects[desc->layers[i].planes[j].object_index].format_modifier;
      planes++;
      layer_num = desc->layers[i].planes[j].object_index + 1;
    }
  }

  // always use min(nb_objects, nb_layers)
  *layers = layer_num;
  *plane_num = planes;

  return planes;
}

static void vulkan_clear() {
  for (int i = 0; i < MAX_FB_NUM; i++) {
    if (primeDescriptors[i]) {
      vulkan_free_render_images(primeDescriptors[i]);
      av_frame_free((AVFrame **)&primeDescriptors[i]);
    }
  }
  return;
}

static int vulkan_set_opts (AVDictionary **opts) {
  return 0;
}

static struct Decoder_Context vulkan_backend = {
  .ffmpeg_type = AV_HWDEVICE_TYPE_VULKAN,
  .ffmpeg_fmt = AV_PIX_FMT_VULKAN,
  .ffmpeg_src_type = AV_HWDEVICE_TYPE_VULKAN,
  .ffmpeg_src_fmt = AV_PIX_FMT_VULKAN,
  .src_device_ref = NULL,
  .src_frames_ref = NULL,
  .set_opts = &vulkan_set_opts,
  .get_supported_format = &vulkan_get_format,
  .validate_test = &vulkan_validate_test,
  .unexport_frame = &vulkan_free_render_images,
  .export_frame = &vulkan_export_render_images,
  .clear_resource = &vulkan_clear,
};

#ifdef HAVE_VAAPI
static int vaapi_is_support_yuv444(AVBufferRef *device_ref) {

  int supported_format = 0;
  int format_num = 0;
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext *va_ctx = device->hwctx;
  int num = vaMaxNumImageFormats(va_ctx->display);
  VAImageFormat formats[num];

  if (vaQueryImageFormats(va_ctx->display, formats, &format_num) == VA_STATUS_SUCCESS) {
    for (int i = 0;i < format_num; i++) {
      switch (formats[i].fourcc) {
      case VA_FOURCC_NV12:
        hw_sw_format.yuv420 = AV_PIX_FMT_NV12;
        break;
      case VA_FOURCC_P010:
        hw_sw_format.yuv420_10 = AV_PIX_FMT_P010;
        break;
      case VA_FOURCC_XYUV:
        hw_sw_format.yuv444 = AV_PIX_FMT_VUYX;
        break;
      case VA_FOURCC_Y410:
        hw_sw_format.yuv444_10 = AV_PIX_FMT_XV30;
        break;
      }
    }
  }

  int pnum = vaMaxNumProfiles(va_ctx->display);
  VAProfile *profiles = pnum > 0 ? malloc(sizeof(VAProfile) * pnum) : NULL;
  int profiles_count = 0;
  VAStatus status = vaQueryConfigProfiles(va_ctx->display, profiles, &profiles_count);
  if (status != VA_STATUS_SUCCESS || profiles == NULL) {
    fprintf(stderr, "Failed to query profiles\n");
    if (pnum > 0)
      free(profiles);
    return 0;
  }
  for (int i = 0; i < profiles_count; i++) {
    switch (profiles[i]) {
    case VAProfileH264High444:
      supported_format |= VIDEO_FORMAT_H264_HIGH8_444;
      break;
    case VAProfileHEVCMain444:
      supported_format |= VIDEO_FORMAT_H265_REXT8_444;
      break;
    case VAProfileHEVCMain444_10:
      supported_format |= VIDEO_FORMAT_H265_REXT10_444;
      break;
    case VAProfileAV1Profile1:
      supported_format |= VIDEO_FORMAT_AV1_HIGH8_444;
      supported_format |= VIDEO_FORMAT_AV1_HIGH10_444;
      break;
    case VAProfileHEVCMain10:
      supported_format |= VIDEO_FORMAT_H265_MAIN10;
      supported_format |= VIDEO_FORMAT_H265;
      break;
    case VAProfileAV1Profile0:
      supported_format |= VIDEO_FORMAT_AV1_MAIN8;
      supported_format |= VIDEO_FORMAT_AV1_MAIN10;
      break;
    }
  }
  supported_format |= VIDEO_FORMAT_H264;

  supported_format = get_supported_format (supported_format);

  if (pnum > 0)
    free(profiles);
  return supported_format;
}

static bool vaapi_validate_test(void *nativeDisplay) {
#ifdef HAVE_DRM
  VADisplay dpy;

  dpy = vaGetDisplayDRM(*((int *)nativeDisplay));

  if (!dpy)
    return false;

  int major,min;
  if (vaInitialize(dpy, &major, &min) != VA_STATUS_SUCCESS) {
    vaTerminate(dpy);
    return false;
  }
  
  VASurfaceAttrib attrs[2];
  int attributeCount = 0;

  // FFmpeg handles setting these quirk flags for us
  if (true) {
    attrs[attributeCount].type = VASurfaceAttribMemoryType;
    attrs[attributeCount].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[attributeCount].value.type = VAGenericValueTypeInteger;
    attrs[attributeCount].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    attributeCount++;
  }

  // These attributes are required for i965 to create a surface that can
  // be successfully exported via vaExportSurfaceHandle(). iHD doesn't
  // need these, but it doesn't seem to hurt either.
  attrs[attributeCount].type = VASurfaceAttribPixelFormat;
  attrs[attributeCount].flags = VA_SURFACE_ATTRIB_SETTABLE;
  attrs[attributeCount].value.type = VAGenericValueTypeInteger;
  attrs[attributeCount].value.value.i = VA_FOURCC_NV12;
  attributeCount++;

  VASurfaceID surfaceId;
  VAStatus st;
  unsigned int rtformat = 0;
  rtformat = VA_RT_FORMAT_YUV420;
  st = vaCreateSurfaces(dpy,
              rtformat,
              1280,
              720,
              &surfaceId,
              1,
              attrs,
              attributeCount);
  if (st != VA_STATUS_SUCCESS) {
    vaTerminate(dpy);
    return false;
  }

  VADRMPRIMESurfaceDescriptor descriptor;

  st = vaExportSurfaceHandle(dpy,
                 surfaceId,
                 VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                 VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                 &descriptor);

  vaDestroySurfaces(dpy, &surfaceId, 1);

  if (st != VA_STATUS_SUCCESS) {
    vaTerminate(dpy);
    return false;
  }

  for (size_t i = 0; i < descriptor.num_objects; ++i) {
    close(descriptor.objects[i].fd);
  }

  vaTerminate(dpy);
  return true;
#else
  return true;
#endif
}

static void vaapi_free_render_images(void *descriptor) {
  VADRMPRIMESurfaceDescriptor *pdescriptor = (VADRMPRIMESurfaceDescriptor *)descriptor;

  if (pdescriptor->num_objects != 0) {
    for (size_t i = 0; i < pdescriptor->num_objects; ++i) {
      if (pdescriptor->objects[i].fd > 0)
        close(pdescriptor->objects[i].fd);
      pdescriptor->objects[i].fd = 0;
    }
    pdescriptor->num_layers = 0;
    pdescriptor->num_objects = 0;
  }
}

static int vaapi_export_render_images(AVBufferRef *device_ref, AVFrame *frame, void *descriptor, int render_type, struct Source_Buffer_Info *buffer_info, int *layers, int *plane_num) {
  ssize_t count = 0;
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext *va_ctx = device->hwctx;
  VASurfaceID surface_id = (VASurfaceID)(uintptr_t)frame->data[3];

  VADRMPRIMESurfaceDescriptor *primeDescriptor = descriptor;

  VAStatus st = vaExportSurfaceHandle(va_ctx->display,
                    surface_id,
                    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                    VA_EXPORT_SURFACE_READ_ONLY | (render_type == EGL_RENDER ? VA_EXPORT_SURFACE_SEPARATE_LAYERS : VA_EXPORT_SURFACE_COMPOSED_LAYERS),
                    primeDescriptor);
  if (st != VA_STATUS_SUCCESS) {
    fprintf(stderr, "Ffmpeg_vaapi: vaExportSurfaceHandle() Failed: %d\n", st);
    return -1;
  }

  st = vaSyncSurface2(va_ctx->display, surface_id, 6000000000);
  if (st != VA_STATUS_SUCCESS) {
    fprintf(stderr, "Ffmpeg_vaapi: vaSyncSurface2() Failed: %d\n", st);
    goto sync_fail;
  }

  int planes = 0;
  for (size_t i = 0; i < primeDescriptor->num_layers; ++i) {
    for (size_t j = 0; j < primeDescriptor->layers[i].num_planes; j++) {
      if (planes > 3) {
        fprintf(stderr, "Ffmpeg_vaapi: Planes number is too big(%d).\n", planes + 1);
        goto sync_fail;
      }
      buffer_info->format[planes] = primeDescriptor->layers[i].drm_format;
      buffer_info->width[planes] = planes == 0 ? frame->width : (isYUV444 ? frame->width : frame->width / 2);
      buffer_info->height[planes] = planes == 0 ? frame->height : (isYUV444 ? frame->height : frame->height / 2);
      buffer_info->fd[planes] = primeDescriptor->objects[primeDescriptor->layers[i].object_index[j]].fd;
      buffer_info->offset[planes] = primeDescriptor->layers[i].offset[j];
      buffer_info->stride[planes] = primeDescriptor->layers[i].pitch[j];
      buffer_info->modifiers[planes] = primeDescriptor->objects[primeDescriptor->layers[i].object_index[j]].drm_format_modifier;
      planes++;
    }
    ++count;
  }
  *layers = (int)count;
  *plane_num = (int)planes;

  return (int)planes;

sync_fail:
  vaapi_free_render_images(primeDescriptor);
  return -1;
}

static void vaapi_clear() { return; };

static int vaapi_set_opts (AVDictionary **opts) { return 0; };

static struct Decoder_Context vaapi_backend = {
  .ffmpeg_type = AV_HWDEVICE_TYPE_VAAPI,
  .ffmpeg_fmt = AV_PIX_FMT_VAAPI,
  .ffmpeg_src_type = AV_HWDEVICE_TYPE_VAAPI,
  .ffmpeg_src_fmt = AV_PIX_FMT_VAAPI,
  .src_device_ref = NULL,
  .src_frames_ref = NULL,
  .set_opts = &vaapi_set_opts,
  .get_supported_format = &vaapi_is_support_yuv444,
  .validate_test = &vaapi_validate_test,
  .unexport_frame = &vaapi_free_render_images,
  .export_frame = &vaapi_export_render_images,
  .clear_resource = &vaapi_clear,
};
#endif

static int is_support_yuv444(AVBufferRef* device_ref) {
  if (hwSupportedFormat)
    return hwSupportedFormat;

  memset(&hw_sw_format, AV_PIX_FMT_NONE, sizeof(hw_sw_format));

  int supported_format = decontext->get_supported_format(device_ref);

  if (!supported_format)
    fprintf(stderr, "Failed to enable yuv444 because of no correct profiles or format\n");

  if (hw_sw_format.yuv444 == AV_PIX_FMT_NONE)
    hw_sw_format.yuv444 = AV_PIX_FMT_NV12;
  if (hw_sw_format.yuv444_10 == AV_PIX_FMT_NONE)
    hw_sw_format.yuv444_10 = AV_PIX_FMT_P010;

  return supported_format;
}

// only execute in get_format()
static inline AVBufferRef* hw_alloc_frames_ctx (struct Decoder_Context* dectx, AVBufferRef* device_ref, AVCodecContext* context) {
  if (device_ref == NULL | context == NULL | dectx == NULL) {
    fprintf(stderr, "Failed to alloc hw context.\n");
    return NULL;
  }

  int init_pools = context->extra_hw_frames;
  enum AVPixelFormat orig_fmt = context->pix_fmt;
  enum AVPixelFormat orig_sfmt = context->sw_pix_fmt;
  enum AVPixelFormat hw_fmt;
  AVBufferRef* ref;
  if (dectx->ffmpeg_type != dectx->ffmpeg_src_type && dectx->src_device_ref) {
    if (dectx->src_frames_ref) {
      av_buffer_unref(&dectx->src_frames_ref);
    }
    ref = dectx->src_device_ref;
    hw_fmt = dectx->ffmpeg_src_fmt;
  }
  else {
    ref = device_ref;
    hw_fmt = dectx->ffmpeg_fmt;
  }

  context->sw_pix_fmt = get_format_from_slot(useHdr, isYUV444);
  context->extra_hw_frames = MAX_SURFACES + 1;

  AVBufferRef *hw_ctx = NULL;
  if (avcodec_get_hw_frames_parameters(context, ref, hw_fmt, &hw_ctx) != 0 || hw_ctx == NULL) {
    fprintf(stderr, "FFMPEG: Alloc hw frames ctx failed.\n");
    goto alloc_failed;
  }

  AVHWFramesContext* fr_ctx = (AVHWFramesContext*) hw_ctx->data;
  if (fr_ctx->initial_pool_size == 0) {
    fr_ctx->initial_pool_size = MAX_SURFACES + 1;
  }

  if (av_hwframe_ctx_init(hw_ctx) < 0) {
    fprintf(stderr, "Failed to initialize HW frame context");
    av_buffer_unref(&hw_ctx);
    goto alloc_failed;
  }
  if (ref != device_ref) {
    AVBufferRef* tmp;
    if (av_hwframe_ctx_create_derived(&tmp, dectx->ffmpeg_fmt, device_ref, hw_ctx, AV_HWFRAME_MAP_READ) != 0) {
      fprintf(stderr, "Failed to derive hw frames context.");
      av_buffer_unref(&hw_ctx);
      goto alloc_failed;
    }
    dectx->src_frames_ref = hw_ctx;
    hw_ctx = tmp;
  }

  if (context->hw_frames_ctx) {
    av_buffer_unref(&context->hw_frames_ctx);
  }
  context->pix_fmt = dectx->ffmpeg_fmt;
  context->hw_frames_ctx = av_buffer_ref(hw_ctx);
  context->slice_flags = SLICE_FLAG_CODED_ORDER | SLICE_FLAG_ALLOW_FIELD;

  return hw_ctx;

alloc_failed:

  context->pix_fmt = orig_fmt;
  context->sw_pix_fmt = orig_sfmt;
  context->extra_hw_frames = init_pools;
  return NULL;
}

static enum AVPixelFormat hw_get_format(AVCodecContext* context, const enum AVPixelFormat* pixel_format) {
  if (frames_ctx_ref)
    av_buffer_unref(&frames_ctx_ref);
  frames_ctx_ref = hw_alloc_frames_ctx(decontext, device_ref, context);
  if (context->pix_fmt == decontext->ffmpeg_fmt)
    return decontext->ffmpeg_fmt;
  else
    return AV_PIX_FMT_NONE;
}

static int hw_get_buffer(AVCodecContext* context, AVFrame* frame, int flags) {
  return av_hwframe_get_buffer(context->hw_frames_ctx, frame, 0);
}

int hw_init_lib(const char *device, int hwtype) {
  AVDictionary *opts = NULL;
  switch (hwtype) {
#ifdef HAVE_VAAPI
  case INIT_VAAPI:
    decontext = &vaapi_backend;
    break;
#endif
  case INIT_VULKAN:
    decontext = &vulkan_backend;
    break;
  default:
    fprintf(stderr, "The ffmpeg hw device is not supported.\n");
    return -1;
  }

  int drm_fd = -1;
  char drmNode[64] = {'\0'};
  drm_fd = get_drm_render_fd(drmNode);
  void *dis = (void *)&drm_fd;
  bool valid = decontext->validate_test(dis);
  if (drm_fd >= 0)
    close(drm_fd);
  if (!valid) {
    fprintf(stderr, "The ffmpeg hw decoder device test failed.\n");
    goto failed;
  }

/*
  const char *drm_device;
  if (device == NULL)
    drm_device = drmNode;
  else
    drm_device = device;
*/

  memset(&descriptors, 0, sizeof(descriptors));
  switch (decontext->ffmpeg_fmt) {
  case AV_PIX_FMT_VAAPI:
    for (int i = 0; i < MAX_FB_NUM; i++) {
      primeDescriptors[i] = &descriptors.vaapi_descriptors[i];
    }
    break;
  case AV_PIX_FMT_VULKAN:
    for (int i = 0; i < MAX_FB_NUM; i++) {
      descriptors.drm_descriptors[i] = av_frame_alloc();
      primeDescriptors[i] = descriptors.drm_descriptors[i];
    }
    break;
  default:
    goto failed;
  }

  if (device_ref)
    av_buffer_unref(&device_ref);
  if (decontext->src_device_ref)
    av_buffer_unref(&decontext->src_device_ref);

  decontext->set_opts(&opts);
  if (decontext->ffmpeg_type != decontext->ffmpeg_src_type) {
    if (av_hwdevice_ctx_create(&decontext->src_device_ref, decontext->ffmpeg_src_type, device, opts, 0) != 0) {
      goto failed;
    }
    if (av_hwdevice_ctx_create_derived_opts(&device_ref, decontext->ffmpeg_type, decontext->src_device_ref, opts, 0) != 0) {
      av_buffer_unref(&decontext->src_device_ref);
      goto failed;
    }
  }
  else {
    if (av_hwdevice_ctx_create(&device_ref, decontext->ffmpeg_type, device, opts, 0) != 0) {
      goto failed;
    }
  }

  hwSupportedFormat = is_support_yuv444(device_ref);

  return 0;
failed:
  if (opts)
    av_dict_free(&opts);
  decontext->clear_resource();
  fprintf(stderr, "Failed to initialize hw lib: device(%d).\n", decontext->ffmpeg_type);
  return -1;
}

int hw_init(AVCodecContext* decoder_ctx) {
  if (decontext == NULL || device_ref == NULL)
    return -1;

  if (decoder_ctx->hw_device_ctx) {
    av_buffer_unref(&decoder_ctx->hw_device_ctx);
  }
  decoder_ctx->hw_device_ctx = av_buffer_ref(device_ref);
  if (decoder_ctx->hw_device_ctx == NULL)
    return -1;

  decoder_ctx->get_format = hw_get_format;
  decoder_ctx->get_buffer2 = hw_get_buffer;
  return 0;
}

void hw_destroy() {
  if (decontext) {
    decontext->clear_resource();
    if (decontext->src_device_ref)
      av_buffer_unref(&decontext->src_device_ref);
    if (decontext->src_frames_ref)
      av_buffer_unref(&decontext->src_frames_ref);
  }
  if (device_ref)
    av_buffer_unref(&device_ref);
  if (frames_ctx_ref)
    av_buffer_unref(&frames_ctx_ref);
  decontext = NULL;
  return;
}

static void hw_free_render_images(void *opaque, uint8_t *data) {
  struct Render_Image *image = (struct Render_Image *) opaque;
  void *descriptor = image->images.descriptor;

  if (image->images.free != NULL) {
    image->images.free(image->images.image_data, image->images.layers);
  }
  if (!descriptor)
    return;
  decontext->unexport_frame(descriptor);
  return;
}

int hw_export_render_images(AVFrame *frame, struct Render_Image *image, int render_type) {
  int layers;
  int planes;

  if (!image->images.descriptor) {
    if (!primeDescriptors[image->index]) {
      fprintf(stderr, "Ffmpeg_vaapi: Has no descriptors.\n");
      return -1;
    }
    image->images.descriptor = primeDescriptors[image->index];
  }
  if (image->images.create == NULL || image->images.free == NULL) {
    fprintf(stderr, "Ffmpeg_vaapi: Has no export images function implement.\n");
    return -1;
  }

  AVBufferRef *ref = av_buffer_create(NULL, 0, &hw_free_render_images, image, 0);
  if (ref == NULL) {
    fprintf(stderr, "Ffmpeg_vaapi: Could not create buffer ref.\n");
    return -1;
  }
  if (frame->opaque_ref)
    av_buffer_unref(&frame->opaque_ref);
  frame->opaque_ref = ref;

  if (decontext->export_frame(device_ref, frame, image->images.descriptor, render_type, &image->images.buf_info, &layers, &planes) < 1) {
    fprintf(stderr, "Ffmpeg_vaapi: Could not export frame.\n");
    return -1;
  }

  if (image->images.create(&image->images.buf_info, planes, layers, image->images.image_data, image->index) < 0) {
    decontext->unexport_frame(image->images.descriptor);
    return -1;
  }

  image->images.layers = layers;
  image->images.planes = planes;
  return 0;
}

int hw_supported_video_format() {
  return hwSupportedFormat;
}
