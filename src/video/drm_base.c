
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#include "drm_base.h"

#ifndef DRM_MODE_COLORIMETRY_DEFAULT
#define DRM_MODE_COLORIMETRY_DEFAULT     0
#endif
#ifndef DRM_MODE_COLORIMETRY_BT2020_RGB
#define DRM_MODE_COLORIMETRY_BT2020_RGB  9
#endif

static struct Drm_Info current_drm_info = {0},old_drm_info = {0};
static const char *drm_device = "/dev/dri/card0";
static enum AVPixelFormat sw_pix_fmt = AV_PIX_FMT_NV12;

static AVBufferRef* device_ref = NULL;

static bool useHDR = false;
static bool drmIsSupportYuv444 = false;

void convert_display (int *src_w, int *src_h, int *dst_w, int *dst_h, int *dst_x, int *dst_y) {
  int dstH = ceilf((float)(*dst_w) * (*src_h) / (*src_w));
  int dstW = ceilf((float)(*dst_h) * (*src_w) / (*src_h));

  if (dstH > *dst_h) {
    *dst_x += (*dst_w - dstW) / 2;
    *dst_w = dstW;
  }
  else {
    *dst_y += (*dst_h - dstH) / 2;
    *dst_h = dstH;
  }
  return;
}

static int num_compare (const void *a, const void *b) {
  return -(*(int *)a - *(int *)b);
}

static int drm_choose_connector (int fd, uint32_t bestConn[MAX_CONNECTOR]) {
  int index = 0;
  uint64_t connSize[MAX_CONNECTOR] = {0};
  uint64_t connSort[MAX_CONNECTOR] = {0};
  uint32_t conns[MAX_CONNECTOR] = {0};
  
  drmModeRes* drmres = drmModeGetResources(fd);
  if (!drmres) {
    perror("Could not get drm resources");
    return -1;
  }
  // first ,get connector
  for (int i = 0; i < drmres->count_connectors; i++) {
    drmModeConnector* conn = drmModeGetConnector(fd, drmres->connectors[i]);
    if (!conn) {
      // no connector, ignore
      continue;
    }
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 && conn->encoder_id != 0) {
      if (index > MAX_CONNECTOR)
        continue;
      connSize[index] = conn->mmWidth * conn->mmHeight;
      conns[index++] = conn->connector_id;
      if (old_drm_info.connector_id == 0) {
        old_drm_info.connector_id = conn->connector_id;
        for (int j = 0; j < conn->count_encoders; j++) {
          drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoders[j]);
          if (!enc) {
            continue;
          }
    
          for (int k = 0; k < drmres->count_crtcs; k++) {
            if (!(enc->possible_crtcs & (1 << k))) {
              continue;
            }
            if (drmres->crtcs[k] != enc->crtc_id) {
              continue;
            }
            old_drm_info.encoder_id = enc->encoder_id;
            old_drm_info.crtc_id = enc->crtc_id;
            break;
          }
          drmModeFreeEncoder(enc);
          if (old_drm_info.encoder_id != 0)
            break;
        }
      }
    }
    drmModeFreeConnector(conn);
    continue;
  }

  memcpy(connSort, connSize, sizeof(connSize));
  qsort(connSort, index, sizeof(uint64_t), num_compare);
  for (int i = 0; i < index; i++) {
    for (int j = 0; j < index; j++) {
      if (connSize[j] == connSort[i]) {
        bestConn[i] = conns[j];
      }
    }
  }

  drmModeFreeResources(drmres);
  return index > 0 ? index : -1;
}

static int drm_choose_crtc (int fd) {
  drmModeRes* drmres = drmModeGetResources(fd);
  if (!drmres) {
    perror("Could not get drm resources\n");
    return -1;
  }
  // first ,get connector
  uint32_t conns[MAX_CONNECTOR];
  int connNum = drm_choose_connector (fd, conns);
  if (connNum < 1) {
    perror("No available connector here\n");
    return -1;
  }
  // then ,get crtc
  for (int i = 0; i < connNum; i++) {
    drmModeConnector* conn = drmModeGetConnector(fd, conns[i]);
    if (!conn) {
      // no connector, ignore
      continue;
    }
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 && conn->encoder_id != 0) {
      memcpy(&current_drm_info.crtc_mode,&conn->modes[0],sizeof(conn->modes[0]));
    }
    else {
      drmModeFreeConnector(conn);
      continue;
    }

    // then , get encoder
    for (int j = 0; j < conn->count_encoders; j++) {
      drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoders[j]);
      if (!enc) {
        continue;
      }

      for (int k = 0; k < drmres->count_crtcs; k++) {
        if (!(enc->possible_crtcs & (1 << k)))
          continue;

        // let drmres crtc older equal to the enc->crtc_id
        if (drmres->crtcs[k] != enc->crtc_id)
          continue;

        drmModeFreeEncoder(enc);
        drmModeFreeConnector(conn);
        current_drm_info.crtc_index = k;
        current_drm_info.connector_id = conn->connector_id;
        current_drm_info.encoder_id = enc->encoder_id;
        current_drm_info.crtc_id = enc->crtc_id;
        goto found_crtc;
      }
      drmModeFreeEncoder(enc);
      continue;
    }
    drmModeFreeConnector(conn);
  }

 found_crtc:
  drmModeFreeResources(drmres);
    
  if (current_drm_info.connector_id == 0 || current_drm_info.encoder_id == 0 || current_drm_info.crtc_id == 0) {
    fprintf(stderr, "Not found connector or encoder for drm\n");
    return -1;
  }

  drmModeCrtc* crtc = drmModeGetCrtc(fd, current_drm_info.crtc_id);
  memcpy(&old_drm_info.crtc_mode,&crtc->mode,sizeof(crtc->mode));
  old_drm_info.crtc_fb_id = crtc->buffer_id;
  old_drm_info.width = crtc->width;
  old_drm_info.height = crtc->height;
  current_drm_info.width = current_drm_info.crtc_mode.hdisplay;
  current_drm_info.height = current_drm_info.crtc_mode.vdisplay;
  drmModeFreeCrtc(crtc);
  if (current_drm_info.width <= 0 || current_drm_info.height <= 0) {
    fprintf(stderr, "Could not get width and height from crtc\n");
    return -1;
  }

  return 0;
}

static int drm_choose_plane (int fd, uint32_t format) {
  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  drmModePlaneRes* res = drmModeGetPlaneResources(fd);
  if (!res) {
    fprintf(stderr, "Could not get res for plane\n");
    return -1;
  }

  for (int  i = 0; i < res->count_planes; i++) {
    drmModePlane* plane = drmModeGetPlane(fd, res->planes[i]);
    if (!plane)
      continue;

    int formats_index = 0;
    bool found_format = false;
    for (int j = 0; j < plane->count_formats; j++) {
      switch (plane->formats[j]) {
      case DRM_FORMAT_Y410:
      case DRM_FORMAT_YUV444:
	drmIsSupportYuv444 = true;
        current_drm_info.plane_formats[formats_index++] = plane->formats[j];
        break;
      case DRM_FORMAT_YUV420:
      case DRM_FORMAT_P010:
      case DRM_FORMAT_NV12:
      case DRM_FORMAT_AYUV:
	current_drm_info.plane_formats[formats_index++] = plane->formats[j];
	break;
      }
      if (formats_index >= NEEDED_DRM_FORMAT_NUM)
	break;
      if (format == plane->formats[j])
        found_format = true;
    }

    // at least need support yuv444 and yuv420
    // but normaly just support nv12 and p010
    if (formats_index < 1 || !found_format) {
      formats_index = 0;
      memset(current_drm_info.plane_formats, 0, sizeof(current_drm_info.plane_formats));
      drmIsSupportYuv444 = false;
      drmModeFreePlane(plane);
      continue;
    }

    if ((plane->possible_crtcs & (1 << current_drm_info.crtc_index))) {
      drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd,res->planes[i], DRM_MODE_OBJECT_PLANE);
      if (!props) {
        formats_index = 0;
        drmIsSupportYuv444 = false;
        memset(current_drm_info.plane_formats, 0, sizeof(current_drm_info.plane_formats));
	drmModeFreePlane(plane);
        continue;
      }

      for (int j = 0; j < props->count_props; j++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[j]);
        if (!prop)
          continue;
        if (strcmp(prop->name, "type") == 0 && (props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY || props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY)) {
          current_drm_info.plane_id = plane->plane_id;
        }
        else if (strcmp(prop->name, "COLOR_ENCODING") == 0) {
          current_drm_info.plane_color_encoding_prop_id  = prop->prop_id;
        }
        else if (strcmp(prop->name, "COLOR_RANGE") == 0) {
          current_drm_info.plane_color_range_prop_id  = prop->prop_id;
        }
        drmModeFreeProperty(prop);
      }
      drmModeFreeObjectProperties(props);
    }
    drmModeFreePlane(plane);
  }
  drmModeFreePlaneResources(res);

  if (current_drm_info.plane_id == 0) {
    fprintf(stderr, "Could not get plane info\n");
    return -1;
  }

  return 0;
}

static int drm_get_connector_hdr_props (int fd) {
  drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, current_drm_info.connector_id, DRM_MODE_OBJECT_CONNECTOR);
  if (!props){
    fprintf(stderr, "Could not get property for connector\n");
    return -1;
  }

  for (int i = 0; i < props->count_props; i++) {
    drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop)
      continue;

    if (strcmp(prop->name, "HDR_OUTPUT_METADATA") == 0) {
      current_drm_info.conn_hdr_metadata_prop_id = prop->prop_id;
    }
    else if (strcmp(prop->name, "Colorspace") == 0) {
      current_drm_info.conn_colorspace_prop_id = prop->prop_id;
    }
    else if (strcmp(prop->name, "max bpc") == 0 && useHDR) {
      current_drm_info.conn_max_bpc_prop_id = prop->prop_id;
    }
    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(props);

  if (current_drm_info.conn_hdr_metadata_prop_id == 0) {
    fprintf(stderr, "Could not get hdr property for connector\n");
    return -1;
  }

  return 0;
}

static int drm_set_connector_hdr_mode (int fd, bool enable) {
  if (current_drm_info.conn_max_bpc_prop_id != 0 && enable) {
    if (drmModeObjectSetProperty(fd, current_drm_info.connector_id, DRM_MODE_OBJECT_CONNECTOR, current_drm_info.conn_max_bpc_prop_id, 16) == 0) {
      printf("Enabled 48-bit HDMI Deep Color\n");
    }
    else if (drmModeObjectSetProperty(fd, current_drm_info.connector_id, DRM_MODE_OBJECT_CONNECTOR, current_drm_info.conn_max_bpc_prop_id, 12) == 0) {
      printf("Enabled 36-bit HDMI Deep Color\n");
    }
    else if (drmModeObjectSetProperty(fd, current_drm_info.connector_id, DRM_MODE_OBJECT_CONNECTOR, current_drm_info.conn_max_bpc_prop_id, 10) == 0) {
      printf("Enabled 30-bit HDMI Deep Color\n");
    }
    else {
      fprintf(stderr, "Could not set hdr property for connector\n");
      return -1;
    }
  }

  if (drmModeObjectSetProperty(fd, current_drm_info.connector_id, DRM_MODE_OBJECT_CONNECTOR, current_drm_info.conn_colorspace_prop_id, enable ? DRM_MODE_COLORIMETRY_BT2020_RGB : DRM_MODE_COLORIMETRY_DEFAULT) != 0) {
    fprintf(stderr, "Could not set colorspace property for connector\n");
    return -1;
  }

  if(!enable) {
    drmModeObjectSetProperty(fd, current_drm_info.connector_id, DRM_MODE_OBJECT_CONNECTOR, current_drm_info.conn_hdr_metadata_prop_id, 0);
  }
  return 0;
}

/*
static enum AVPixelFormat drm_get_format(AVCodecContext* context, const enum AVPixelFormat* pixel_format) {
  AVBufferRef* hw_ctx = av_hwframe_ctx_alloc(device_ref);
  if (hw_ctx == NULL) {
    fprintf(stderr, "Failed to initialize ffmpeg drm buffer\n");
    return AV_PIX_FMT_NONE;
  }
  AVHWFramesContext* fr_ctx = (AVHWFramesContext*) hw_ctx->data;
  fr_ctx->format = AV_PIX_FMT_DRM_PRIME;
  fr_ctx->sw_format = sw_pix_fmt;
  fr_ctx->width = context->coded_width;
  fr_ctx->height = context->coded_height;
  // can not set initial_pool_size,will break ctx_init
  //fr_ctx->initial_pool_size = 1;

  if (av_hwframe_ctx_init(hw_ctx) < 0) {
    fprintf(stderr, "Failed to initialize drm frame context\n");
    av_buffer_unref(&hw_ctx);
    return AV_PIX_FMT_NONE;
  }
  context->pix_fmt = AV_PIX_FMT_DRM_PRIME;
  context->hw_device_ctx = device_ref;
  context->hw_frames_ctx = hw_ctx;
  return AV_PIX_FMT_DRM_PRIME;
}
*/

int ffmpeg_bind_drm_ctx(AVCodecContext* decoder_ctx, AVDictionary** dict) {
 // before avcode_open2()
  if(strstr(decoder_ctx->codec->name, "_v4l2") != NULL)
    av_dict_set_int(dict, "pixel_format", AV_PIX_FMT_NV12, 0);

  if (device_ref == NULL) {
    decoder_ctx->pix_fmt = AV_PIX_FMT_NV12;
    return 0;
  }

  av_dict_set(dict, "omx_pix_fmt", "nv12", 0);
/*
  decoder_ctx->get_format = drm_get_format;
*/
  return 0;
}

int ffmpeg_init_drm_hw_ctx(const char *device, const enum AVPixelFormat pixel_format) {

  sw_pix_fmt = pixel_format;
/*
  if (av_hwdevice_ctx_create(&device_ref, AV_HWDEVICE_TYPE_DRM, device == NULL ? drm_device : device, NULL, 0) != 0) {
   fprintf(stderr, "Couldn't initialize av_hwdevice_ctx\n");
  }
*/
  return 0;
}

struct Drm_Info * drm_init (const char *device, uint32_t drmformat, bool usehdr) {
  if (device != NULL)
    drm_device = device;
  current_drm_info.fd = -1;
  current_drm_info.fd = open(drm_device, O_RDWR | O_CLOEXEC);
  if (current_drm_info.fd < 0) {
    perror("Could not open /dev/dri/card0");
    return NULL;
  }
  if (drm_choose_crtc(current_drm_info.fd) < 0 || drm_choose_plane(current_drm_info.fd, drmformat) < 0) {
    return NULL;
  }
  if (drm_get_connector_hdr_props(current_drm_info.fd) == 0 && useHDR) {
    if (drm_set_connector_hdr_mode(current_drm_info.fd, true) < 0) {
      useHDR = false;
    }
  }

  return &current_drm_info;
}

void drm_close() {
  // todo....
  // restore crtc and connector to original status
  // atomic change
  if (current_drm_info.connector_id != 0) {
    drmModeSetCrtc(current_drm_info.fd, current_drm_info.crtc_id, old_drm_info.crtc_fb_id, 0, 0, &current_drm_info.connector_id, 1, &old_drm_info.crtc_mode);
  }
  close(current_drm_info.fd);
  current_drm_info.fd = -1;
}

bool drm_is_support_yuv444(int format) {
  // look current_drm_info.plane_formats[formats_index++];
  return drmIsSupportYuv444;
}

struct Drm_Info *get_drm_info(bool current) {
  return current ? &current_drm_info : &old_drm_info;
}

int get_drm_dbum_aligned(int fd, int pixfmt, int width, int height) {
  const char *device = "/dev/dri/card0";
  int mfd = -1;
  if (fd < 0) {
    mfd = open(device, O_RDWR | O_CLOEXEC);
    if (mfd < 0) {
      perror("Could not open /dev/dri/card0");
      return -1;
    }
  }
  
  int bpc;
  int frameHeightMulti;
  switch (pixfmt) {
  case AV_PIX_FMT_NV12:
  case AV_PIX_FMT_P010:
    frameHeightMulti = 2;
    bpc = pixfmt == AV_PIX_FMT_P010 ? 16 : 8;
    break;
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P:
    frameHeightMulti = 2;
    bpc = 8;
    break;
  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUVJ444P:
    frameHeightMulti = 3;
    bpc = 8;
    break;
  default:
    frameHeightMulti = 2;
    bpc = 8;
    break;
  }

  int multi = -1;
  struct drm_mode_create_dumb createBuf = {};
  createBuf.width = width;
  createBuf.height = height * frameHeightMulti;
  createBuf.bpp = bpc;
  if (drmIoctl(fd < 0 ? mfd : fd, DRM_IOCTL_MODE_CREATE_DUMB, &createBuf) < 0) {
    fprintf(stderr, "Could not create drm dumb\n");
    return -1;
  }
  // not should set multi
  if (width == createBuf.pitch)
    return -1;

  #define test_multi(w, p, m) \
            (((int)(w / m)) * m + m == p ? true : false)
  
  int maxMulti = 128;
  while (maxMulti > 2) {
    if (test_multi(width, createBuf.pitch, maxMulti))
      goto exit;
    maxMulti = maxMulti / 2;
  }

exit:
  multi = maxMulti <= 2 ? -1 : maxMulti;

  struct drm_mode_destroy_dumb destroyBuf = {0};
  destroyBuf.handle = createBuf.handle;
  drmIoctl(fd < 0 ? mfd : fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyBuf);
  if (mfd >= 0)
    close(mfd);
  return multi;
}

/*
void printf_props () {
   drmModeObjectPropertiesPtr propss = drmModeObjectGetProperties(fd,drmres->connectors[i], DRM_MODE_OBJECT_CONNECTOR);
   for (int j = 0; j < propss->count_props; j++) {
      drmModePropertyPtr propi = drmModeGetProperty(fd, propss->props[j]);
      drmModePropertyPtr propm = drmModeGetProperty(current_drm_info.fd, propi->prop_id);
      for (int z = 0; z < propm->count_enums; z++) {
         printf("33333333:%s-%s,%d\n", propi->name, propm->enums[z].name,propm->enums[z].value);
      }
      for (int z = 0; z < propm->count_values; z++) {
        printf("44444444:%s,%d\n", propi->name, propi->values[z]);
      }
      drmModeFreeProperty(propm);
      drmModeFreeProperty(propi);
    }
  drmModeFreeObjectProperties(propss);
  continue;
}
*/
