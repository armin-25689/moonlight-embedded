--- src/video/ffmpeg_vaapi.c.orig	2024-08-03 07:59:40 UTC
+++ src/video/ffmpeg_vaapi.c
@@ -18,16 +18,149 @@
  */
 
 #include <va/va.h>
-#include <va/va_x11.h>
+#include <va/va_drm.h>
+#include <va/va_drmcommon.h>
 #include <libavcodec/avcodec.h>
 #include <libavutil/hwcontext.h>
 #include <libavutil/hwcontext_vaapi.h>
-#include <X11/Xlib.h>
+#include <libavutil/pixdesc.h>
 
+#include <Limelight.h>
+#include <EGL/egl.h>
+#include <GLES3/gl3.h>
+
+#include <fcntl.h>
+#include <stdbool.h>
+#include <string.h>
+#include <unistd.h>
+
+#include "ffmpeg_vaapi_egl.h"
+
+#define EGL_ATTRIB_COUNT 30 * 2
+#ifndef EGL_EXT_image_dma_buf_import
+#define EGL_LINUX_DMA_BUF_EXT             0x3270
+#define EGL_LINUX_DRM_FOURCC_EXT          0x3271
+#define EGL_DMA_BUF_PLANE0_FD_EXT         0x3272
+#define EGL_DMA_BUF_PLANE0_OFFSET_EXT     0x3273
+#define EGL_DMA_BUF_PLANE0_PITCH_EXT      0x3274
+#define EGL_DMA_BUF_PLANE1_FD_EXT         0x3275
+#define EGL_DMA_BUF_PLANE1_OFFSET_EXT     0x3276
+#define EGL_DMA_BUF_PLANE1_PITCH_EXT      0x3277
+#define EGL_DMA_BUF_PLANE2_FD_EXT         0x3278
+#define EGL_DMA_BUF_PLANE2_OFFSET_EXT     0x3279
+#define EGL_DMA_BUF_PLANE2_PITCH_EXT      0x327A
+#define EGL_YUV_COLOR_SPACE_HINT_EXT      0x327B
+#define EGL_SAMPLE_RANGE_HINT_EXT         0x327C
+#define EGL_YUV_CHROMA_HORIZONTAL_SITING_HINT_EXT 0x327D
+#define EGL_YUV_CHROMA_VERTICAL_SITING_HINT_EXT 0x327E
+#define EGL_ITU_REC601_EXT                0x327F
+#define EGL_ITU_REC709_EXT                0x3280
+#define EGL_ITU_REC2020_EXT               0x3281
+#define EGL_YUV_FULL_RANGE_EXT            0x3282
+#define EGL_YUV_NARROW_RANGE_EXT          0x3283
+#define EGL_YUV_CHROMA_SITING_0_EXT       0x3284
+#define EGL_YUV_CHROMA_SITING_0_5_EXT     0x3285
+#endif
+
+#ifndef EGL_EXT_image_dma_buf_import_modifiers
+#define EGL_DMA_BUF_PLANE3_FD_EXT         0x3440
+#define EGL_DMA_BUF_PLANE3_OFFSET_EXT     0x3441
+#define EGL_DMA_BUF_PLANE3_PITCH_EXT      0x3442
+#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
+#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
+#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
+#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
+#define EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT 0x3447
+#define EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT 0x3448
+#define EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT 0x3449
+#define EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT 0x344A
+#endif
+
 #define MAX_SURFACES 16
 
+
 static AVBufferRef* device_ref;
+static VADRMPRIMESurfaceDescriptor primeDescriptor;
 
+#define sw_format_slot 9
+static enum AVPixelFormat hw_sw_format[sw_format_slot] = { AV_PIX_FMT_NONE };
+static enum AVPixelFormat sharedFmt = AV_PIX_FMT_NONE;
+static bool vaapiIsSupportYuv444 = false;
+static bool isYUV444 = false;
+
+static bool is_support_yuv444() {
+  if (vaapiIsSupportYuv444)
+    return true;
+  memset(&hw_sw_format, AV_PIX_FMT_NONE, sizeof(hw_sw_format));
+  bool supported = false;
+  int sw_format = 0;
+  int format_num = 0;
+  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
+  AVVAAPIDeviceContext *va_ctx = device->hwctx;
+  int num = vaMaxNumImageFormats(va_ctx->display);
+  VAImageFormat formats[num];
+
+  if (vaQueryImageFormats(va_ctx->display, formats, &format_num) == VA_STATUS_SUCCESS) {
+    for (int i = 0;i < format_num; i++) {
+      switch (formats[i].fourcc) {
+      case VA_FOURCC_444P:
+        hw_sw_format[sw_format++] = AV_PIX_FMT_YUV444P;
+        supported = true;
+        break;
+      case VA_FOURCC_XYUV:
+        hw_sw_format[sw_format++] = AV_PIX_FMT_VUYX;
+        // vuyx need keep the first
+        if (sw_format != 0 && hw_sw_format[0] > 0) {
+          hw_sw_format[sw_format - 1] = hw_sw_format[0];
+        }
+        hw_sw_format[0] = AV_PIX_FMT_VUYX;
+        supported = true;
+        break;
+      case VA_FOURCC_Y410:
+        hw_sw_format[sw_format++] = AV_PIX_FMT_XV30;
+        supported = true;
+        break;
+      }
+    }
+  }
+
+  int pnum = vaMaxNumProfiles(va_ctx->display);
+  VAProfile *profiles = pnum > 0 ? malloc(sizeof(VAProfile) * pnum) : NULL;
+  int profiles_count = 0;
+  VAStatus status = vaQueryConfigProfiles(va_ctx->display, profiles, &profiles_count);
+  if (status != VA_STATUS_SUCCESS || profiles == NULL) {
+    fprintf(stderr, "Failed to query profiles\n");
+    if (pnum > 0)
+      free(profiles);
+    return false;
+  }
+  for (int i = 0; i < profiles_count; i++) {
+    if (profiles[i] == VAProfileHEVCMain444 || profiles[i] == VAProfileHEVCMain444_10) {
+      hw_sw_format[sw_format++] = AV_PIX_FMT_NV12;
+      free(profiles);
+      return supported;
+    }
+  }
+
+  fprintf(stderr, "Failed to enable yuv444 because of no correct profiles or format\n");
+  if (pnum > 0)
+    free(profiles);
+  return false;
+}
+
+static enum AVPixelFormat get_format_from_slot () {
+  enum AVPixelFormat yuv444_fmt = AV_PIX_FMT_NONE;
+  for (int i = 0; i < sw_format_slot; i++) {
+    if (hw_sw_format[i] != AV_PIX_FMT_NONE) {
+      yuv444_fmt = hw_sw_format[i];
+      hw_sw_format[i] = AV_PIX_FMT_NONE;
+      printf("Try to use pixel format for yuv444: %s \n", av_get_pix_fmt_name(yuv444_fmt));
+      return yuv444_fmt;
+    }
+  }
+  return AV_PIX_FMT_NONE;
+}
+
 static enum AVPixelFormat va_get_format(AVCodecContext* context, const enum AVPixelFormat* pixel_format) {
   AVBufferRef* hw_ctx = av_hwframe_ctx_alloc(device_ref);
   if (hw_ctx == NULL) {
@@ -37,16 +170,31 @@ static enum AVPixelFormat va_get_format(AVCodecContext
 
   AVHWFramesContext* fr_ctx = (AVHWFramesContext*) hw_ctx->data;
   fr_ctx->format = AV_PIX_FMT_VAAPI;
-  fr_ctx->sw_format = AV_PIX_FMT_NV12;
+  if (isYUV444 && !vaapiIsSupportYuv444) {
+    isYUV444 = false;
+    fprintf(stderr, "Failed to initialize VAAPI frame context because of not supported YUV444 format");
+    return AV_PIX_FMT_NONE;
+  } else if (isYUV444) {
+    fr_ctx->sw_format = get_format_from_slot();
+    if (fr_ctx->sw_format == AV_PIX_FMT_NONE) {
+      fprintf(stderr, "Failed to initialize VAAPI frame context because of no pix_fomat");
+      return AV_PIX_FMT_NONE;
+    }
+  }
+  else
+    fr_ctx->sw_format = AV_PIX_FMT_NV12;
   fr_ctx->width = context->coded_width;
   fr_ctx->height = context->coded_height;
   fr_ctx->initial_pool_size = MAX_SURFACES + 1;
 
   if (av_hwframe_ctx_init(hw_ctx) < 0) {
     fprintf(stderr, "Failed to initialize VAAPI frame context");
+    av_buffer_unref(&hw_ctx);
     return AV_PIX_FMT_NONE;
   }
 
+  sharedFmt = fr_ctx->sw_format;
+
   context->pix_fmt = AV_PIX_FMT_VAAPI;
   context->hw_device_ctx = device_ref;
   context->hw_frames_ctx = hw_ctx;
@@ -58,8 +206,13 @@ static int va_get_buffer(AVCodecContext* context, AVFr
   return av_hwframe_get_buffer(context->hw_frames_ctx, frame, 0);
 }
 
-int vaapi_init_lib() {
-  return av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_VAAPI, ":0", NULL, 0);
+int vaapi_init_lib(const char *device) {
+  if(av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_VAAPI, device, NULL, 0) == 0) {
+    vaapiIsSupportYuv444 = is_support_yuv444();
+    return 0;
+  }
+  fprintf(stderr, "Failed to initialize VAAPI lib");
+  return -1;
 }
 
 int vaapi_init(AVCodecContext* decoder_ctx) {
@@ -68,9 +221,253 @@ int vaapi_init(AVCodecContext* decoder_ctx) {
   return 0;
 }
 
-void vaapi_queue(AVFrame* dec_frame, Window win, int width, int height) {
-  VASurfaceID surface = (VASurfaceID)(uintptr_t)dec_frame->data[3];
+bool vaapi_is_can_direct_render() {
   AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
+  AVVAAPIDeviceContext* va_ctx = (AVVAAPIDeviceContext*)device->hwctx;
+  VAEntrypoint entrypoints[vaMaxNumEntrypoints(va_ctx->display)];
+  int entrypointCount;
+  VAStatus status = vaQueryConfigEntrypoints(va_ctx->display, VAProfileNone, entrypoints, &entrypointCount);
+  if (status == VA_STATUS_SUCCESS) {
+    for (int i = 0; i < entrypointCount; i++) {
+      // Without VAEntrypointVideoProc support, the driver will crash inside vaPutSurface()
+      if (entrypoints[i] == VAEntrypointVideoProc)
+        return true;
+    }
+  }
+  return false;
+}
+
+// Ensure that vaExportSurfaceHandle() is supported by the VA-API driver
+bool vaapi_can_export_surface_handle(bool isTenBit) {
+  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
+  AVVAAPIDeviceContext* va_ctx = (AVVAAPIDeviceContext*)device->hwctx;
+  VASurfaceID surfaceId;
+  VAStatus st;
+  VADRMPRIMESurfaceDescriptor descriptor;
+  VASurfaceAttrib attrs[2];
+  int attributeCount = 0;
+
+  // FFmpeg handles setting these quirk flags for us
+  if (!(va_ctx->driver_quirks & AV_VAAPI_DRIVER_QUIRK_ATTRIB_MEMTYPE)) {
+    attrs[attributeCount].type = VASurfaceAttribMemoryType;
+    attrs[attributeCount].flags = VA_SURFACE_ATTRIB_SETTABLE;
+    attrs[attributeCount].value.type = VAGenericValueTypeInteger;
+    attrs[attributeCount].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
+    attributeCount++;
+  }
+
+  // These attributes are required for i965 to create a surface that can
+  // be successfully exported via vaExportSurfaceHandle(). iHD doesn't
+  // need these, but it doesn't seem to hurt either.
+  attrs[attributeCount].type = VASurfaceAttribPixelFormat;
+  attrs[attributeCount].flags = VA_SURFACE_ATTRIB_SETTABLE;
+  attrs[attributeCount].value.type = VAGenericValueTypeInteger;
+  attrs[attributeCount].value.value.i = isTenBit ? VA_FOURCC_P010 : VA_FOURCC_NV12;
+  attributeCount++;
+
+  unsigned int rtformat = 0;
+  rtformat = isTenBit ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
+  st = vaCreateSurfaces(va_ctx->display,
+              rtformat,
+              1280,
+              720,
+              &surfaceId,
+              1,
+              attrs,
+              attributeCount);
+  if (st != VA_STATUS_SUCCESS)
+    return false;
+
+  st = vaExportSurfaceHandle(va_ctx->display,
+                 surfaceId,
+                 VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
+                 VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
+                 &descriptor);
+
+  vaDestroySurfaces(va_ctx->display, &surfaceId, 1);
+
+  if (st != VA_STATUS_SUCCESS)
+    return false;
+
+  for (size_t i = 0; i < descriptor.num_objects; ++i) {
+    close(descriptor.objects[i].fd);
+  }
+
+  return true;
+}
+
+void vaapi_free_egl_images(EGLDisplay dpy, 
+                   EGLImage images[4]) {
+  for (size_t i = 0; i < primeDescriptor.num_layers; ++i) {
+    eglDestroyImage(dpy, images[i]);
+  }
+  for (size_t i = 0; i < primeDescriptor.num_objects; ++i) {
+    close(primeDescriptor.objects[i].fd);
+  }
+  primeDescriptor.num_layers = 0;
+  primeDescriptor.num_objects = 0;
+}
+
+ssize_t vaapi_export_egl_images(AVFrame *frame, EGLDisplay dpy, bool eglIsSupportExtDmaBufMod,
+                        EGLImage images[4]) {
+  ssize_t count = 0;
+  AVHWDeviceContext* device = (AVHWDeviceContext*) device_ref->data;
   AVVAAPIDeviceContext *va_ctx = device->hwctx;
-  vaPutSurface(va_ctx->display, surface, win, 0, 0, dec_frame->width, dec_frame->height, 0, 0, width, height, NULL, 0, 0);
+
+  VASurfaceID surface_id = (VASurfaceID)(uintptr_t)frame->data[3];
+  VAStatus st = vaExportSurfaceHandle(va_ctx->display,
+                    surface_id,
+                    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
+                    VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
+                    &primeDescriptor);
+  if (st != VA_STATUS_SUCCESS)
+    return -1;
+
+  if (primeDescriptor.num_layers > 4)
+    return -1;
+
+  st = vaSyncSurface(va_ctx->display, surface_id);
+  if (st != VA_STATUS_SUCCESS) {
+    goto sync_fail;
+  }
+
+  for (size_t i = 0; i < primeDescriptor.num_layers; ++i) {
+    // Max 30 attributes (1 key + 1 value for each)
+    EGLAttrib attribs[EGL_ATTRIB_COUNT] = {
+      EGL_LINUX_DRM_FOURCC_EXT, primeDescriptor.layers[i].drm_format,
+      EGL_WIDTH, i == 0 ? frame->width : (isYUV444 ? frame->width : frame->width / 2),
+      EGL_HEIGHT, i == 0 ? frame->height : (isYUV444 ? frame->height : frame->height / 2),
+/*
+      // need test111111111111
+      EGL_WIDTH, frame->linesize[i],
+      EGL_HEIGHT, i == 0 ? frame->height : (isYUV444 ? frame->height : frame->height / 2),
+*/
+    };
+
+    int attribIndex = 6;
+    for (size_t j = 0; j < primeDescriptor.layers[i].num_planes; j++) {
+      switch (j) {
+      case 0:
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_FD_EXT;
+        attribs[attribIndex++] = primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].fd;
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
+        attribs[attribIndex++] = primeDescriptor.layers[i].offset[0];
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
+        attribs[attribIndex++] = primeDescriptor.layers[i].pitch[0];
+        if (eglIsSupportExtDmaBufMod) {
+          attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
+          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier & 0xFFFFFFFF);
+          attribs[attribIndex++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
+          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier >> 32);
+        }
+        break;
+
+      case 1:
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_FD_EXT;
+        attribs[attribIndex++] = primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].fd;
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
+        attribs[attribIndex++] = primeDescriptor.layers[i].offset[1];
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
+        attribs[attribIndex++] = primeDescriptor.layers[i].pitch[1];
+        if (eglIsSupportExtDmaBufMod) {
+          attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
+          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier & 0xFFFFFFFF);
+          attribs[attribIndex++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
+          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier >> 32);
+        }
+        break;
+
+      case 2:
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_FD_EXT;
+        attribs[attribIndex++] = primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].fd;
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
+        attribs[attribIndex++] = primeDescriptor.layers[i].offset[2];
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
+        attribs[attribIndex++] = primeDescriptor.layers[i].pitch[2];
+        if (eglIsSupportExtDmaBufMod) {
+          attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
+          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier & 0xFFFFFFFF);
+          attribs[attribIndex++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
+          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier >> 32);
+        }
+        break;
+
+      case 3:
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_FD_EXT;
+        attribs[attribIndex++] = primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].fd;
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_OFFSET_EXT;
+        attribs[attribIndex++] = primeDescriptor.layers[i].offset[3];
+        attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_PITCH_EXT;
+        attribs[attribIndex++] = primeDescriptor.layers[i].pitch[3];
+        if (eglIsSupportExtDmaBufMod) {
+          attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT;
+          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier & 0xFFFFFFFF);
+          attribs[attribIndex++] = EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT;
+          attribs[attribIndex++] = (EGLint)(primeDescriptor.objects[primeDescriptor.layers[i].object_index[j]].drm_format_modifier >> 32);
+        }
+        break;
+
+      default:
+        goto sync_fail;
+      }
+    }
+
+    // Terminate the attribute list
+    attribs[attribIndex++] = EGL_NONE;
+    if (attribIndex > EGL_ATTRIB_COUNT)
+        goto sync_fail;
+
+    images[i] = eglCreateImage(dpy, EGL_NO_CONTEXT,
+                   EGL_LINUX_DMA_BUF_EXT,
+                   NULL, attribs);
+    if (!images[i]) {
+      fprintf(stderr, "eglCreateImage() Failed: %d\n", eglGetError());
+      goto create_image_fail;
+    }
+
+    ++count;
+  }
+  return count;
+
+create_image_fail:
+  primeDescriptor.num_layers = count;
+sync_fail:
+  vaapi_free_egl_images(dpy, images);
+  return -1;
+}
+
+int vaapi_get_plane_info (enum AVPixelFormat **pix_fmt, int *plane_num, enum PixelFormatOrder *plane_order) {
+  *pix_fmt = &sharedFmt;
+  int planes = av_pix_fmt_count_planes(sharedFmt);
+  *plane_num = planes <= 0 ? 4 : planes;
+  switch (sharedFmt) {
+  case AV_PIX_FMT_VUYX:
+    *plane_order = VUYX_ORDER;
+    break;
+  case AV_PIX_FMT_XV30:
+  case AV_PIX_FMT_XV36:
+    *plane_order = XVYU_ORDER;
+    break;
+  case AV_PIX_FMT_YUV420P:
+  case AV_PIX_FMT_YUV444P:
+  case AV_PIX_FMT_YUV444P10:
+    *plane_order = YUVX_ORDER;
+    break;
+  case AV_PIX_FMT_NV12:
+  case AV_PIX_FMT_NV16:
+  case AV_PIX_FMT_NV24:
+  case AV_PIX_FMT_P010:
+  case AV_PIX_FMT_P016:
+    *plane_order = YUVX_ORDER;
+    break;
+  default:
+    *plane_order = YUVX_ORDER;
+    break;
+  }
+  return 0;
+}
+
+bool vaapi_is_support_yuv444(int needyuv444) {
+  isYUV444 = needyuv444 > 0 ? true : false;
+  return vaapiIsSupportYuv444;
 }
