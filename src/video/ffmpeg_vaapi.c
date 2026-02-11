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

#include <va/va.h>
#ifdef HAVE_X11
#include <va/va_x11.h>
#endif
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>

#include <Limelight.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "render.h"
#include "video_internal.h"

#define MAX_SURFACES 16

// prepare for high444 profile. not support now
#ifndef VAProfileH264High444
#define VAProfileH264High444 99
#endif

static AVBufferRef* device_ref;
static VADRMPRIMESurfaceDescriptor prime_descriptors[MAX_FB_NUM] = {0};
static VADRMPRIMESurfaceDescriptor *primeDescriptors[MAX_FB_NUM] = {0};

#define sw_format_slot 9
struct {
  enum AVPixelFormat yuv444;
  enum AVPixelFormat yuv444_10;
  enum AVPixelFormat yuv420;
  enum AVPixelFormat yuv420_10;
} static hw_sw_format;
static int vaapiSupportedFormat = 0;

static int is_support_yuv444() {
  if (vaapiSupportedFormat)
    return vaapiSupportedFormat;

  memset(&hw_sw_format, AV_PIX_FMT_NONE, sizeof(hw_sw_format));

  int supported_format = 0;
  int format_num = 0;
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext *va_ctx = device->hwctx;
  int num = vaMaxNumImageFormats(va_ctx->display);
  VAImageFormat formats[num];

  if (vaQueryImageFormats(va_ctx->display, formats, &format_num) == VA_STATUS_SUCCESS) {
    for (int i = 0;i < format_num; i++) {
      switch (formats[i].fourcc) {
/*
      case VA_FOURCC_444P:
        break;
*/
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

  if (hw_sw_format.yuv444 < 0) {
    supported_format &= ~(VIDEO_FORMAT_AV1_HIGH8_444 | VIDEO_FORMAT_H265_REXT8_444 | VIDEO_FORMAT_H264_HIGH8_444);
  }
  if (hw_sw_format.yuv444_10 < 0) {
    supported_format &= ~(VIDEO_FORMAT_AV1_HIGH10_444 | VIDEO_FORMAT_H265_REXT10_444);
  }
  if (hw_sw_format.yuv420_10 < 0) {
    supported_format &= ~(VIDEO_FORMAT_AV1_MAIN10 | VIDEO_FORMAT_H265_MAIN10);
  }

  if (!supported_format)
    fprintf(stderr, "Failed to enable yuv444 because of no correct profiles or format\n");
  if (pnum > 0)
    free(profiles);
  return supported_format;
}

static enum AVPixelFormat get_format_from_slot (bool useHDR, bool yuv444) {
  if (yuv444 && useHDR)
    return hw_sw_format.yuv444_10;
  else if (yuv444)
    return hw_sw_format.yuv444;
  else if (useHDR)
    return hw_sw_format.yuv420_10;
  else
    return hw_sw_format.yuv420;
}

static enum AVPixelFormat va_get_format(AVCodecContext* context, const enum AVPixelFormat* pixel_format) {
  AVBufferRef* hw_ctx = av_hwframe_ctx_alloc(device_ref);
  if (hw_ctx == NULL) {
    fprintf(stderr, "Failed to initialize Vaapi buffer\n");
    return AV_PIX_FMT_NONE;
  }

  AVHWFramesContext* fr_ctx = (AVHWFramesContext*) hw_ctx->data;
  fr_ctx->format = AV_PIX_FMT_VAAPI;
  fr_ctx->sw_format = get_format_from_slot(useHdr, isYUV444);
  if (fr_ctx->sw_format == AV_PIX_FMT_NONE) {
    fprintf(stderr, "Failed to initialize VAAPI frame context because of no pix_fomat");
    return AV_PIX_FMT_NONE;
  }
  fr_ctx->width = context->coded_width;
  fr_ctx->height = context->coded_height;
  fr_ctx->initial_pool_size = MAX_SURFACES + 1;

  if (av_hwframe_ctx_init(hw_ctx) < 0) {
    fprintf(stderr, "Failed to initialize VAAPI frame context");
    av_buffer_unref(&hw_ctx);
    return AV_PIX_FMT_NONE;
  }

  context->pix_fmt = AV_PIX_FMT_VAAPI;
  context->hw_device_ctx = device_ref;
  context->hw_frames_ctx = hw_ctx;
  context->slice_flags = SLICE_FLAG_CODED_ORDER | SLICE_FLAG_ALLOW_FIELD;
  return AV_PIX_FMT_VAAPI;
}

static int va_get_buffer(AVCodecContext* context, AVFrame* frame, int flags) {
  return av_hwframe_get_buffer(context->hw_frames_ctx, frame, 0);
}

int vaapi_init_lib(const char *device) {
  if(av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_VAAPI, device, NULL, 0) == 0) {
    vaapiSupportedFormat = is_support_yuv444();
    return 0;
  }
  fprintf(stderr, "Failed to initialize VAAPI lib");
  return -1;
}

int vaapi_init(AVCodecContext* decoder_ctx) {
  for (int i = 0; i < MAX_FB_NUM; i++) {
    primeDescriptors[i] = &prime_descriptors[i];
  }

  decoder_ctx->get_format = va_get_format;
  decoder_ctx->get_buffer2 = va_get_buffer;
  return 0;
}

bool vaapi_validate_test(char *displayName, char *renderName, void *nativeDisplay) {
  bool isTenBit = false;
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
  attrs[attributeCount].value.value.i = isTenBit ? VA_FOURCC_P010 : VA_FOURCC_NV12;
  attributeCount++;

  VASurfaceID surfaceId;
  VAStatus st;
  unsigned int rtformat = 0;
  rtformat = isTenBit ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
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
}

void vaapi_free_render_images(void **renderImages, void *descriptor, void(*render_unmap_buffer) (void** image, int planes)) {
  if (!descriptor)
    return;

  VADRMPRIMESurfaceDescriptor *pdescriptor = (VADRMPRIMESurfaceDescriptor *)descriptor;

  if (render_unmap_buffer != NULL) {
    render_unmap_buffer(renderImages, pdescriptor->num_layers);
  }
  if (pdescriptor->num_objects != 0) {
    for (size_t i = 0; i < pdescriptor->num_objects; ++i) {
      if (pdescriptor->objects[i].fd > -1)
        close(pdescriptor->objects[i].fd);
      pdescriptor->objects[i].fd = -1;
    }
    pdescriptor->num_layers = 0;
    pdescriptor->num_objects = 0;
  }
}

int vaapi_export_render_images(AVFrame *frame, struct Render_Image *image, void *descriptor, int render_type,
      int(*render_map_buffer) (struct Source_Buffer_Info *buffer, int planes,
                               int composeOrSeperate, void** image, int index),
      void(*render_unmap_buffer) (void** image, int planes)) {
  ssize_t count = 0;
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext *va_ctx = device->hwctx;
  void **renderImages = image->images.image_data;
  VASurfaceID surface_id = (VASurfaceID)(uintptr_t)frame->data[3];

  VADRMPRIMESurfaceDescriptor *primeDescriptor = descriptor;
  if (!primeDescriptor) {
    fprintf(stderr, "Ffmpeg_vaapi: cannot get clear descriptor.\n");
    return -1;
  }

  if (render_map_buffer == NULL) {
    fprintf(stderr, "Ffmpeg_vaapi: Has no export images function implement.\n");
    return -1;
  }

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
  struct Source_Buffer_Info buffer_info = {0};
  memset(buffer_info.fd, -1, sizeof(buffer_info.fd));
  for (size_t i = 0; i < primeDescriptor->num_layers; ++i) {
    for (size_t j = 0; j < primeDescriptor->layers[i].num_planes; j++) {
      if (planes > 3) {
        fprintf(stderr, "Ffmpeg_vaapi: Planes number is too big(%d).\n", planes + 1);
        goto create_image_fail;
      }
      buffer_info.format[planes] = primeDescriptor->layers[i].drm_format;
      buffer_info.width[planes] = planes == 0 ? frame->width : (isYUV444 ? frame->width : frame->width / 2);
      buffer_info.height[planes] = planes == 0 ? frame->height : (isYUV444 ? frame->height : frame->height / 2);
      buffer_info.fd[planes] = primeDescriptor->objects[primeDescriptor->layers[i].object_index[j]].fd;
      buffer_info.offset[planes] = primeDescriptor->layers[i].offset[j];
      buffer_info.stride[planes] = primeDescriptor->layers[i].pitch[j];
      buffer_info.modifiers[planes] = primeDescriptor->objects[primeDescriptor->layers[i].object_index[j]].drm_format_modifier;
      planes++;
    }
    ++count;
  }

  if (render_map_buffer(&buffer_info, planes, (count != planes) ? COMPOSE_PLANE : SEPERATE_PLANE, renderImages, image->index) < 0) {
    goto create_image_fail;
  }

  return (int)planes;

create_image_fail:
  primeDescriptor->num_layers = planes;
sync_fail:
  vaapi_free_render_images(renderImages, primeDescriptor, NULL);
  return -1;
}

void *vaapi_get_descriptors_ptr() {
  return primeDescriptors;
}

int vaapi_supported_video_format() {
  return vaapiSupportedFormat;
}

ssize_t software_store_frame (AVFrame *frame, struct Render_Image *image, void *descriptor, int render_type,
                              int(*render_map_buffer)(struct Source_Buffer_Info *buffer,
                                                      int planes, int composeOrSeperate,
                                                      void* *image)) {
  image->sframe.frame_data = frame->data;

  // always return 1 planes
  return 1;
}
