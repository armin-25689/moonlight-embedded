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

enum DrmBroadcastRgb { AUTORGB = 0, FULLRGB, LIMITEDRGB };
enum DrmColorspace { DEFAULTCOLOR = 0, D2020RGB, D2020YCC };
enum DrmColorSpace { DBT601 = 0, DBT709, DBT2020 };
enum DrmColorRange { LIMITED_RANGE = 0, FULL_RANGE }; 
enum DrmCommitOpt { DRM_ADD_COMMIT = 0, DRM_APPLY_COMMIT, DRM_CLEAR_LIST };

struct Drm_Info {
  int fd;
  int width;
  int height;
  uint32_t conn_num;
  uint32_t connectors[MAX_CONNECTOR];
  uint32_t connector_id;
  uint32_t connector_type;
  uint32_t conn_hdr_metadata_prop_id;
  uint32_t conn_colorspace_prop_id;
  uint64_t conn_colorspace_values[3];
  uint32_t conn_max_bpc_prop_id;
  uint32_t conn_crtc_prop_id;
  uint32_t conn_broadcast_rgb_prop_id;
  uint64_t conn_broadcast_rgb_prop_values[3];
  uint32_t conn_allm_prop_id;
  uint32_t encoder_id;
  uint32_t crtc_id;
  uint32_t crtc_fb_id;
  uint32_t crtc_prop_mode_id;
  uint32_t crtc_prop_active;
  uint32_t crtc_vrr_prop_id;
  int crtc_index;
  drmModeModeInfo crtc_mode;
  uint32_t crtc_mode_blob_id;
  uint32_t plane_id;
  uint32_t plane_fb_id_prop_id;
  uint32_t plane_formats[NEEDED_DRM_FORMAT_NUM];
  uint32_t plane_color_encoding_prop_id;
  uint32_t plane_color_range_prop_id;
  uint64_t plane_color_encoding_prop_values[3];
  uint64_t plane_color_range_prop_values[3];
  uint32_t plane_crtc_prop_id;
  uint32_t plane_crtc_x_prop_id;
  uint32_t plane_crtc_y_prop_id;
  uint32_t plane_crtc_w_prop_id;
  uint32_t plane_crtc_h_prop_id;
  uint32_t plane_src_x_prop_id;
  uint32_t plane_src_y_prop_id;
  uint32_t plane_src_w_prop_id;
  uint32_t plane_src_h_prop_id;
  uint32_t plane_eotf_prop_id;
  uint32_t plane_rotation_prop_id;
  uint32_t have_atomic;
  uint32_t have_plane;
};

struct _drm_buf {
  uint32_t fd[4];
  uint32_t width[4];
  uint32_t height[4];
  uint32_t format[4];
  uint32_t pitch[4];
  uint32_t offset[4];
  uint64_t modifiers[4];
  uint32_t handle[4];
  uint32_t fb_id;
  void *data;
};

// return NULL is  failed
struct Drm_Info *drm_init (const char *device, uint32_t drmformat, bool usehdr);
void drm_close();
void drm_restore_display();

void convert_display (const uint32_t *src_w, const uint32_t *src_h, uint32_t *dst_w, uint32_t *dst_h, int *dst_x, int *dst_y);
int get_drm_dbum_aligned (int fd, int pixfmt, int width, int height);
int drm_flip_buffer (uint32_t fd, uint32_t crtc_id, uint32_t fb_id, uint64_t hdr_data, uint32_t width, uint32_t height);
int drm_get_plane_info (struct Drm_Info *drm_info, uint32_t format);
int translate_format_to_drm(int format, int *bpp, int *heightmulti, int *planenum);
int drm_set_display(int fd, uint32_t crtc_id, uint32_t src_w, uint32_t src_h, uint32_t crtc_w, uint32_t crtc_h, uint32_t *connector_id, uint32_t connector_num, drmModeModeInfoPtr connModePtr, uint32_t fb_id);
int drm_choose_color_config (enum DrmColorSpace colorspace, bool fullRange);
int drm_apply_hdr_metadata(int fd, uint32_t conn_id, uint32_t hdr_metadata_prop_id, struct hdr_output_metadata *data);
int drm_opt_commit (enum DrmCommitOpt opt, drmModeAtomicReq *req, uint32_t device_id, uint32_t prop_id, uint64_t value);
