/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
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

#include <xf86drmMode.h>

#define NEEDED_DRM_FORMAT_NUM 9
#define MAX_CONNECTOR 5
// just list here
//static uint32_t wanted_drm_formats[NEEDED_DRM_FORMAT_NUM] = { DRM_FORMAT_YUV444, DRM_FORMAT_YUV420, DRM_FORMAT_Y410, DRM_FORMAT_AYUV, DRM_FORMAT_P010, DRM_FORMAT_NV12, 0, 0};

struct Hdr_Metadata_Infoframe {
  uint8_t eotf;
  uint8_t metadata_type;

  struct {
    uint16_t x, y;
  } display_primaries[3];

  struct {
    uint16_t x, y;
  } white_point;

  uint16_t max_display_mastering_luminance;
  uint16_t min_display_mastering_luminance;

  uint16_t max_cll;
  uint16_t max_fall;
};
struct Hdr_Output_Metadata {
  uint32_t metadata_type;

  union {
    struct Hdr_Metadata_Infoframe hdmi_metadata_type1;
  };
};
struct Drm_Info {
  int fd;
  int width;
  int height;
  uint32_t conn_num;
  uint32_t connectors[MAX_CONNECTOR];
  uint32_t connector_id;
  uint32_t encoder_id;
  uint32_t crtc_id;
  uint32_t crtc_fb_id;
  int crtc_index;
  drmModeModeInfo crtc_mode;
  uint32_t plane_id;
  uint32_t plane_formats[NEEDED_DRM_FORMAT_NUM];
  uint32_t plane_color_encoding_prop_id;
  uint32_t plane_color_range_prop_id;
  uint32_t conn_hdr_metadata_prop_id;
  uint32_t conn_colorspace_prop_id;
  uint32_t conn_max_bpc_prop_id;
};

struct Soft_Mapping_Info {
  uint32_t handle;
  uint32_t pitch;
  uint64_t size;
  uint8_t* mapping;
  int primeFd;
};

// return NULL is  failed
struct Drm_Info *drm_init (const char *device, uint32_t drmformat, bool usehdr);
void drm_close();
bool drm_is_support_yuv444 (int format);
// true is current info,false is old info.
struct Drm_Info *get_drm_info(bool current);

void convert_display (int *src_w, int *src_h, int *dst_w, int *dst_h, int *dst_x, int *dst_y);
int get_drm_dbum_aligned(int fd, int pixfmt, int width, int height);
