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
#include <va/va_drmcommon.h>
#include <va/va_x11.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <X11/Xlib.h>

#include <Limelight.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stdbool.h>
#include <unistd.h>

#define EGL_ATTRIB_COUNT 30 * 2
#ifndef EGL_EXT_image_dma_buf_import
#define EGL_LINUX_DMA_BUF_EXT             0x3270
#define EGL_LINUX_DRM_FOURCC_EXT          0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT         0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT     0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT      0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT         0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT     0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT      0x3277
#define EGL_DMA_BUF_PLANE2_FD_EXT         0x3278
#define EGL_DMA_BUF_PLANE2_OFFSET_EXT     0x3279
#define EGL_DMA_BUF_PLANE2_PITCH_EXT      0x327A
#define EGL_YUV_COLOR_SPACE_HINT_EXT      0x327B
#define EGL_SAMPLE_RANGE_HINT_EXT         0x327C
#define EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT 0x327D
#define EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT 0x327E
#define EGL_ITU_REC601_EXT                0x327F
#define EGL_ITU_REC709_EXT                0x3280
#define EGL_ITU_REC2020_EXT               0x3281
#define EGL_YUV_FULL_RANGE_EXT            0x3282
#define EGL_YUV_NARROW_RANGE_EXT          0x3283
#define EGL_YUV_CHROMA_SITING_0_EXT       0x3284
#define EGL_YUV_CHROMA_SITING_0_5_EXT     0x3285
#endif

#ifndef EGL_EXT_image_dma_buf_import_modifiers
#define EGL_DMA_BUF_PLANE3_FD_EXT         0x3440
#define EGL_DMA_BUF_PLANE3_OFFSET_EXT     0x3441
#define EGL_DMA_BUF_PLANE3_PITCH_EXT      0x3442
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
#define EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT 0x3447
#define EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT 0x3448
#define EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT 0x3449
#define EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT 0x344A
#endif

typedef struct VAAPIDevicePriv {
	Display *x11_display;
	int drm_fd;
} VAAPIDevicePriv;

#define MAX_SURFACES 16

bool isYUV444 = false;

static Display *x11_display = NULL;
static int drm_fd = -1;

static AVBufferRef* device_ref;
static VADRMPRIMESurfaceDescriptor primeDescriptor;

static bool is_support_yuv444() {
  int format_num = 0;
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext *va_ctx = device->hwctx;
  int num = vaMaxNumImageFormats(va_ctx->display);
  VAImageFormat formats[num];

  VAStatus status = vaQueryImageFormats(va_ctx->display, formats, &format_num);
  if (status == VA_STATUS_SUCCESS) {
    for (int i = 0;i < format_num; i++) {
      if (formats[i].fourcc == VA_FOURCC_444P) {
        return true;
        break;
      }
    }
  }

  return false;
}

static enum AVPixelFormat va_get_format(AVCodecContext* context, const enum AVPixelFormat* pixel_format) {
  AVBufferRef* hw_ctx = av_hwframe_ctx_alloc(device_ref);
  if (hw_ctx == NULL) {
    fprintf(stderr, "Failed to initialize Vaapi buffer\n");
    return AV_PIX_FMT_NONE;
  }

  AVHWFramesContext* fr_ctx = (AVHWFramesContext*) hw_ctx->data;
  fr_ctx->format = AV_PIX_FMT_VAAPI;
  if (isYUV444 && !is_support_yuv444())
    isYUV444 = false;
  if (isYUV444) {
    printf("Using YUV444 format to decode!It's need host support!\n");
    fr_ctx->sw_format = AV_PIX_FMT_YUV444P;
  } else
    fr_ctx->sw_format = AV_PIX_FMT_NV12;
  fr_ctx->width = context->coded_width;
  fr_ctx->height = context->coded_height;
  fr_ctx->initial_pool_size = MAX_SURFACES + 1;

  if (av_hwframe_ctx_init(hw_ctx) < 0) {
    fprintf(stderr, "Failed to initialize VAAPI frame context");
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
    AVHWDeviceContext *ctx = (AVHWDeviceContext*) device_ref->data;
    VAAPIDevicePriv *priv = (VAAPIDevicePriv*) ctx->user_opaque;
    drm_fd = priv->drm_fd;
    x11_display = priv->x11_display;
    return 0;
  }
  return -1;
}

int vaapi_init(AVCodecContext* decoder_ctx) {
  decoder_ctx->get_format = va_get_format;
  decoder_ctx->get_buffer2 = va_get_buffer;
  return 0;
}

bool test_vaapi_queue(AVFrame* dec_frame, Window win, int width, int height) {
  VASurfaceID surface = (VASurfaceID)(uintptr_t)dec_frame->data[3];
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext *va_ctx = device->hwctx;
  VAStatus status = vaPutSurface(va_ctx->display, surface, win, 0, 0, dec_frame->width, dec_frame->height, 0, 0, width, height, NULL, 0, 0);
  if (status != VA_STATUS_SUCCESS) {
    return false;
  }
  return true;
}

void vaapi_queue(AVFrame* dec_frame, Window win, int width, int height) {
  VASurfaceID surface = (VASurfaceID)(uintptr_t)dec_frame->data[3];
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext *va_ctx = device->hwctx;
  vaPutSurface(va_ctx->display, surface, win, 0, 0, dec_frame->width, dec_frame->height, 0, 0, width, height, NULL, 0, 0);
}

bool isVaapiCanDirectRender() {
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext* va_ctx = (AVVAAPIDeviceContext*)device->hwctx;
  VAEntrypoint entrypoints[vaMaxNumEntrypoints(va_ctx->display)];
  int entrypointCount;
  VAStatus status = vaQueryConfigEntrypoints(va_ctx->display, VAProfileNone, entrypoints, &entrypointCount);
  if (status == VA_STATUS_SUCCESS) {
    for (int i = 0; i < entrypointCount; i++) {
      // Without VAEntrypointVideoProc support, the driver will crash inside vaPutSurface()
      if (entrypoints[i] == VAEntrypointVideoProc)
        return true;
    }
  }
  return false;
}

// Ensure that vaExportSurfaceHandle() is supported by the VA-API driver
bool canExportSurfaceHandle(bool isTenBit) {
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext* va_ctx = (AVVAAPIDeviceContext*)device->hwctx;
  VASurfaceID surfaceId;
  VAStatus st;
  VADRMPRIMESurfaceDescriptor descriptor;
  VASurfaceAttrib attrs[2];
  int attributeCount = 0;

  // FFmpeg handles setting these quirk flags for us
  if (!(va_ctx->driver_quirks & AV_VAAPI_DRIVER_QUIRK_ATTRIB_MEMTYPE)) {
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
  if (isYUV444)
    attrs[attributeCount].value.value.i = isTenBit ? VA_FOURCC_Y410 : VA_FOURCC_444P;
  else
    attrs[attributeCount].value.value.i = isTenBit ? VA_FOURCC_P010 : VA_FOURCC_NV12;
  attributeCount++;

  unsigned int rtformat = 0;
  if (isYUV444)
    rtformat = isTenBit ? VA_RT_FORMAT_YUV444_10 : VA_RT_FORMAT_YUV444;
  else
    rtformat = isTenBit ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
  st = vaCreateSurfaces(va_ctx->display,
              rtformat,
              1280,
              720,
              &surfaceId,
              1,
              attrs,
              attributeCount);
  if (st != VA_STATUS_SUCCESS)
    return false;

  st = vaExportSurfaceHandle(va_ctx->display,
                 surfaceId,
                 VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                 VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                 &descriptor);

  vaDestroySurfaces(va_ctx->display, &surfaceId, 1);

  if (st != VA_STATUS_SUCCESS)
    return false;

  for (size_t i = 0; i < descriptor.num_objects; ++i) {
    close(descriptor.objects[i].fd);
  }

  return true;
}

void freeEGLImages(EGLDisplay dpy, 
                   EGLImage images[4]) {
  for (size_t i = 0; i < primeDescriptor.num_layers; ++i) {
    eglDestroyImage(dpy, images[i]);
  }
  for (size_t i = 0; i < primeDescriptor.num_objects; ++i) {
    close(primeDescriptor.objects[i].fd);
  }
  primeDescriptor.num_layers = 0;
  primeDescriptor.num_objects = 0;
}

ssize_t exportEGLImages(AVFrame *frame, EGLDisplay dpy, bool eglIsSupportExtDmaBufMod,
                        EGLImage images[4]) {
  ssize_t count = 0;
  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
  AVVAAPIDeviceContext *va_ctx = device->hwctx;

  VASurfaceID surface_id = (VASurfaceID)(uintptr_t)frame->data[3];
  VAStatus st = vaExportSurfaceHandle(va_ctx->display,
                    surface_id,
                    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                    VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                    &primeDescriptor);
  if (st != VA_STATUS_SUCCESS)
    return -1;

  if (primeDescriptor.num_layers > 4)
    return -1;

  st = vaSyncSurface(va_ctx->display, surface_id);
  if (st != VA_STATUS_SUCCESS) {
    goto sync_fail;
  }

  for (size_t i = 0; i < primeDescriptor.num_layers; ++i) {
    // Max 30 attributes (1 key + 1 value for each)
    EGLAttrib attribs[EGL_ATTRIB_COUNT] = {
      EGL_LINUX_DRM_FOURCC_EXT, primeDescriptor.layers[i].drm_format,
      EGL_WIDTH, i == 0 ? frame->width : (isYUV444 ? frame->width : frame->width / 2),
      EGL_HEIGHT, i == 0 ? frame->height : (isYUV444 ? frame->height : frame->height / 2),
    };

    int attribIndex = 6;
    for (size_t j = 0; j < primeDescriptor.layers[i].num_planes; j++) {
      switch (j) {
      case 0:
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attribs[attribIndex++] = primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].fd;
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attribs[attribIndex++] = primeDescriptor.layers[i].offset[0];
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attribs[attribIndex++] = primeDescriptor.layers[i].pitch[0];
        if (eglIsSupportExtDmaBufMod) {
          attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier & 0xFFFFFFFF);
          attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier >> 32);
        }
        break;

      case 1:
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_FD_EXT;
        attribs[attribIndex++] = primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].fd;
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
        attribs[attribIndex++] = primeDescriptor.layers[i].offset[1];
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
        attribs[attribIndex++] = primeDescriptor.layers[i].pitch[1];
        if (eglIsSupportExtDmaBufMod) {
          attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier & 0xFFFFFFFF);
          attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier >> 32);
        }
        break;

      case 2:
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_FD_EXT;
        attribs[attribIndex++] = primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].fd;
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
        attribs[attribIndex++] = primeDescriptor.layers[i].offset[2];
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
        attribs[attribIndex++] = primeDescriptor.layers[i].pitch[2];
        if (eglIsSupportExtDmaBufMod) {
          attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier & 0xFFFFFFFF);
          attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier >> 32);
        }
        break;

      case 3:
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_FD_EXT;
        attribs[attribIndex++] = primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].fd;
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_OFFSET_EXT;
        attribs[attribIndex++] = primeDescriptor.layers[i].offset[3];
        attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_PITCH_EXT;
        attribs[attribIndex++] = primeDescriptor.layers[i].pitch[3];
        if (eglIsSupportExtDmaBufMod) {
          attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT;
          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier & 0xFFFFFFFF);
          attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT;
          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier >> 32);
        }
        break;

      default:
        goto sync_fail;
      }
    }

    // Terminate the attribute list
    attribs[attribIndex++] = EGL_NONE;
    if (attribIndex > EGL_ATTRIB_COUNT)
        goto sync_fail;

    images[i] = eglCreateImage(dpy, EGL_NO_CONTEXT,
                   EGL_LINUX_DMA_BUF_EXT,
                   NULL, attribs);
    if (!images[i]) {
      fprintf(stderr, "eglCreateImage() Failed: %d\n", eglGetError());
      goto create_image_fail;
    }

    ++count;
  }
  return count;

create_image_fail:
  primeDescriptor.num_layers = count;
sync_fail:
  freeEGLImages(dpy, images);
  return -1;
}

bool isFrameFullRange(const AVFrame* frame) {
  return frame->color_range == AVCOL_RANGE_JPEG;
}

int getFrameColorspace(const AVFrame* frame) {
  switch (frame->colorspace) {
  case AVCOL_SPC_SMPTE170M:
  case AVCOL_SPC_BT470BG:
    return COLORSPACE_REC_601;
  case AVCOL_SPC_BT709:
    return COLORSPACE_REC_709;
  case AVCOL_SPC_BT2020_NCL:
  case AVCOL_SPC_BT2020_CL:
    return COLORSPACE_REC_2020;
  default:
    return COLORSPACE_REC_601;
  }
}

void *get_display_from_vaapi(bool isXDisplay) {
  if (isXDisplay)
    return x11_display;
  else
    return &drm_fd;
}
