
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm_fourcc.h>
#include <sys/timespec.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>

#include "drm_base.h"

static const char *drm_device = "/dev/dri/card0";
static uint8_t have_atomic = 0;

struct _props_ptr {
  uint32_t **ids;
  uint32_t **props;
  uint64_t **props_value;
  uint32_t props_num;
};
struct _props {
  uint32_t *ids;
  uint32_t *props;
  uint64_t *props_value;
  uint32_t props_num;
};

struct _snap_props {
  struct _props snap;
};

struct _commit_list {
  uint32_t device_id;
  uint32_t prop_id;
  uint64_t value;
};

static int (*drmpageflip) (uint32_t fd, struct _pageflip_prop *props, uint32_t fb_id, uint32_t flags, void *data);
static int drmpageflip_legacy(uint32_t fd, struct _pageflip_prop *props, uint32_t fb_id, uint32_t flags, void *data);
static int drmpageflip_atomic(uint32_t fd, struct _pageflip_prop *props, uint32_t fb_id, uint32_t flags, void *data);
static int (*drm_add_fb_func) (int fd, uint32_t width, uint32_t height, uint32_t pixel_format, const uint32_t bo_handles[4], const uint32_t pitches[4], const uint32_t offsets[4], const uint64_t modifier[4], uint32_t *buf_id, uint32_t flags);
static int drm_add_fb_legacy (int fd, uint32_t width, uint32_t height, uint32_t pixel_format, const uint32_t bo_handles[4], const uint32_t pitches[4], const uint32_t offsets[4], const uint64_t modifier[4], uint32_t *buf_id, uint32_t flags) {
  return drmModeAddFB2(fd, width, height, pixel_format, bo_handles, pitches, offsets, buf_id, (flags &= ~DRM_MODE_FB_MODIFIERS));
}

void convert_display (const uint32_t *src_w, const uint32_t *src_h, uint32_t *dst_w, uint32_t *dst_h, int *dst_x, int *dst_y) {
  uint32_t dstW = ceilf((float)(*dst_h) * (*src_w) / (*src_h));
  uint32_t dstH = ceilf((float)(*dst_w) * (*src_h) / (*src_w));

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

int drm_opt_commit (enum DrmCommitOpt opt, void *data, uint32_t device_id, uint32_t prop_id, uint64_t value) {
  #define MAX_PROP_SLOT_NUM 99
  #define INITIAL_SLOT 20
  // opt 0 is add, 1 is get, 2 is clear;
  struct {
    struct _commit_list *list;
    uint32_t count;
    size_t slot;
  } static commit_list = { .list = NULL, .count = 0, .slot = INITIAL_SLOT, },
           restore_list = { 0 };

  switch (opt) {
  case DRM_ADD_COMMIT:
    if (commit_list.list == NULL) commit_list.list = calloc(commit_list.slot, sizeof(struct _commit_list));
    if (commit_list.count >= commit_list.slot) {
      struct _commit_list *tmplist = realloc(commit_list.list, sizeof(struct _commit_list) * commit_list.slot * 2);
      if (tmplist == NULL)
        return -1;
      commit_list.list = tmplist;
      commit_list.slot = commit_list.slot * 2;
    }

    if (device_id ==0 || prop_id == 0) return -1;

    for (int i = 0; i < commit_list.count; i++) {
      if (commit_list.list[i].prop_id == prop_id && commit_list.list[i].device_id == device_id) {
        commit_list.list[i].value = value;
        return i;
      }
    }

    commit_list.list[commit_list.count].device_id = device_id;
    commit_list.list[commit_list.count].prop_id = prop_id;
    commit_list.list[commit_list.count].value = value;

    commit_list.count++;

    return commit_list.count; 
  case DRM_APPLY_COMMIT:
    if (commit_list.count == 0 || data == NULL) return -1;
    if (restore_list.list == NULL)
      restore_list.list = calloc(MAX_PROP_SLOT_NUM, sizeof(struct _commit_list));
    drmModeAtomicReq *req = (drmModeAtomicReq *) data;
    int count = commit_list.count;
    for (int i = 0; i < commit_list.count; i++) {
      drmModeAtomicAddProperty(req, commit_list.list[i].device_id, commit_list.list[i].prop_id, commit_list.list[i].value);

      int found = -1;
      for (int k = 0; k < restore_list.count; k++) {
        if (restore_list.list[k].prop_id == commit_list.list[i].prop_id &&
            restore_list.list[k].device_id == commit_list.list[i].device_id) {
          restore_list.list[k].value = commit_list.list[i].value;
          found = k;
          break;
        }
      }
      if (found < 0) {
        restore_list.list[restore_list.count].device_id = commit_list.list[i].device_id;
        restore_list.list[restore_list.count].prop_id = commit_list.list[i].prop_id;
        restore_list.list[restore_list.count].value = commit_list.list[i].value;
        restore_list.count++;
      }
    }

    commit_list.count = 0;
    return count;
  case DRM_RESTORE_COMMIT:
    if (restore_list.count == 0) return -1;
    for (int i = 0; i < restore_list.count; i++) {
      drm_opt_commit (DRM_ADD_COMMIT, NULL, restore_list.list[i].device_id, restore_list.list[i].prop_id, restore_list.list[i].value);
    }
    return restore_list.count;
  case DRM_CLEAR_LIST:
    if (commit_list.list == NULL) return 0;
    if (restore_list.list != NULL) free(restore_list.list);
    free(commit_list.list);
    commit_list.list = NULL;
    commit_list.slot = INITIAL_SLOT;
    commit_list.count = 0;
    restore_list.list = NULL;
    restore_list.count = 0;
    return 0;
  }

  #undef MAX_PROP_SLOT_NUM
  #undef INITIAL_SLOT
  return -1;
}

static inline void drm_get_prop_enum (int fd, const char** names, uint32_t count, uint32_t prop_id, uint64_t *values) {
  drmModePropertyPtr prop = drmModeGetProperty(fd, prop_id);
  if (prop) {
    for (int i = 0; i < count; i++) {
      for (int k = 0; k < prop->count_enums; k++) {
        if (strcmp(names[i], prop->enums[k].name) == 0) {
          values[i] = prop->enums[k].value;
          break;
        }
      }
    }
    drmModeFreeProperty(prop);
  }

  return;
}

static int drm_get_props (int fd, uint32_t object_id, uint32_t object_type, const char **name, struct _props_ptr *store, int num) {
  int ret = 0;

  drmModeObjectProperties *props = drmModeObjectGetProperties(fd, object_id, object_type);
  if (props == NULL) {
    perror("Could not get drm object properties: ");
    return -1;
  }

  for ( int i = 0; i < props->count_props; i++) {
    drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
    if (!prop)
      continue;
    for (int j = 0; j < num; j++) {
      if (strcmp(name[j], prop->name) == 0) {
        *store->props[j] = prop->prop_id;
        *store->props_value[j] = props->prop_values[i];
        ret++;
      }
    }
    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(props);

  store->props_num = ret;

  return ret;
}

static int drm_set_props (int fd, uint32_t *ids, uint32_t *prop_id, uint64_t *prop_value, int props_num, uint32_t flags, uint32_t object_type, void *data) {
  int ret = 0;
  if (have_atomic) {
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req)
      return -1;
    for (int i = 0; i < props_num; i++) {
      drmModeAtomicAddProperty(req, ids[i], prop_id[i], prop_value[i]);
    }
    ret = drmModeAtomicCommit(fd, req, flags, data);
    if (ret < 0)
      perror("Drm Set property failed: ");
    drmModeAtomicFree(req);
  } else {
    for (int i = 0; i < props_num; i++) {
      int res = drmModeObjectSetProperty(fd, ids[i], object_type, prop_id[i], prop_value[i]);
      if (res < 0)
        perror("Drm Set property failed: ");
      ret += res;
    }
  }

  return ret;
}

static void drm_clear_snap (struct _props *prop_copy) {
  if (prop_copy->ids)
    free(prop_copy->ids);
  if (prop_copy->props)
    free(prop_copy->props);
  if (prop_copy->props_value)
    free(prop_copy->props_value);
  memset(prop_copy, 0, sizeof(struct _props));

  return;
}

/*
static int drm_snap_status (int fd, uint32_t object_id, uint32_t object_type, struct _props *prop_copy) {
  drmModeObjectProperties *props = drmModeObjectGetProperties(fd, object_id, object_type);
  if (props == NULL) {
    perror("Could not get drm object properties: ");
    return -1;
  }
  prop_copy->props_num = props->count_props;
  prop_copy->props = malloc(props->count_props * sizeof(uint32_t));
  prop_copy->props_value = malloc(props->count_props * sizeof(uint64_t));
  if (prop_copy->props == NULL || prop_copy->props_value == NULL) {
    perror("Could not alloc memory for connector props: ");
    drmModeFreeObjectProperties(props);
    prop_copy->props_num = 0;
    return -1;
  }
  for (int i = 0; i < props->count_props; i++) {
    prop_copy->props[i] = props->props[i];
    prop_copy->props_value[i] = props->prop_values[i];
  }
  drmModeFreeObjectProperties(props);

  return 0;
}
*/

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
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 && conn->encoder_id != 0 && index < MAX_CONNECTOR) {
      connSize[index] = conn->mmWidth * conn->mmHeight;
      conns[index++] = conn->connector_id;
    }
    drmModeFreeConnector(conn);
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

static int drm_choose_crtc (struct Drm_Info *info) {
  int fd = info->fd;
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
      memcpy(&info->crtc_mode,&conn->modes[0],sizeof(conn->modes[0]));
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
        info->crtc_index = k;
        info->connector_id = conn->connector_id;
        info->connector_type = conn->connector_type;
        info->encoder_id = enc->encoder_id;
        info->crtc_id = enc->crtc_id;
        goto found_crtc;
      }
      drmModeFreeEncoder(enc);
    }
    drmModeFreeConnector(conn);
  }

 found_crtc:
  drmModeFreeResources(drmres);
    
  if (info->connector_id == 0 || info->encoder_id == 0 || info->crtc_id == 0) {
    fprintf(stderr, "Not found connector or encoder for drm.\n");
    return -1;
  }

  info->width = info->crtc_mode.hdisplay;
  info->height = info->crtc_mode.vdisplay;
  drmModeCrtc* crtc = drmModeGetCrtc(fd, info->crtc_id);
  memcpy(&info->old_info.crtc_mode,&crtc->mode,sizeof(crtc->mode));
  info->old_info.fb_id = crtc->buffer_id;
  drmModeFreeCrtc(crtc);
  uint32_t vrr_prop;
  uint32_t *sprops = &vrr_prop;
  uint64_t *svalues = &info->conn_vrr_capable_value;
  const char *snames[] = { "vrr_capable" };
  struct _props_ptr stores = { .props = &sprops, .props_value = &svalues, .props_num = 0 };
  drm_get_props(fd, info->connector_id, DRM_MODE_OBJECT_CONNECTOR, snames, &stores, 1);
  if (info->width <= 0 || info->height <= 0) {
    fprintf(stderr, "Could not get width and height from crtc\n");
    return -1;
  }

  return 0;
}

static int drm_get_plane (struct Drm_Info *drm_info, uint32_t format) {
  int format_site = -1;

  if (!drm_info->have_plane) {
    drm_info->have_plane = drmSetClientCap(drm_info->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0 ? 1 : 0;
  }
  if (!drm_info->have_plane) {
    fprintf(stderr, "DRM:Client not support plane.\n");
    return -1;
  }

  drmModePlaneRes* res = drmModeGetPlaneResources(drm_info->fd);
  if (!res) {
    fprintf(stderr, "Could not get res for plane\n");
    return -1;
  }

  for (int  i = 0; i < res->count_planes; i++) {
    drmModePlane* plane = drmModeGetPlane(drm_info->fd, res->planes[i]);
    if (!plane || (plane->possible_crtcs & (1 << drm_info->crtc_index)) == 0)
      continue;

    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drm_info->fd,res->planes[i], DRM_MODE_OBJECT_PLANE);
    if (props) {
      for (int j = 0; j < props->count_props; j++) {
        drmModePropertyPtr prop = drmModeGetProperty(drm_info->fd, props->props[j]);
        if (!prop)
          continue;

        if (strcmp(prop->name, "type") == 0 && (props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY || props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY)) {
          int formats_index = 0;
          format_site = -1;
          memset(drm_info->plane_formats, 0, sizeof(drm_info->plane_formats));
          for (int k = 0; k < plane->count_formats; k++) {
            switch (plane->formats[k]) {
            case DRM_FORMAT_XYUV8888:
            case DRM_FORMAT_XVYU2101010:
            case DRM_FORMAT_YUV420:
            case DRM_FORMAT_YUV444:
            case DRM_FORMAT_Q410:
            case DRM_FORMAT_NV12:
            #ifdef DRM_FORMAT_NV24
            case DRM_FORMAT_NV24:
            #endif
            #ifdef DRM_FORMAT_NV30
            case DRM_FORMAT_NV15:
            case DRM_FORMAT_NV30:
            #endif
            case DRM_FORMAT_P010:
            case DRM_FORMAT_ARGB8888:
            case DRM_FORMAT_XRGB8888:
            case DRM_FORMAT_XRGB2101010:
              drm_info->plane_formats[formats_index++] = plane->formats[k];
              break;
            }
            if (format == plane->formats[k]) {
              int findex = formats_index == 0 ? 0 : formats_index - 1;
              if (drm_info->plane_formats[findex] != format) {
                drm_info->plane_formats[formats_index++] = plane->formats[k];
              }
              format_site = formats_index - 1;
            }
            if (formats_index >= NEEDED_DRM_FORMAT_NUM)
              break;
          }

          if (formats_index > 0) {
            drm_info->plane_id = plane->plane_id;
            drmModeFreeProperty(prop);
            drmModeFreeObjectProperties(props);
            drmModeFreePlane(plane);
            drmModeFreePlaneResources(res);
            goto found_plane;
          }
        }
        drmModeFreeProperty(prop);
      }
      drmModeFreeObjectProperties(props);
    }
    drmModeFreePlane(plane);
  }
  drmModeFreePlaneResources(res);

  fprintf(stderr, "Could not get plane info\n");
  return -1;

found_plane:

  if (drm_info->old_info.props_list == NULL) {
    struct _snap_props *drm_props = calloc(1, sizeof(struct _snap_props));
    if (drm_props == NULL) {
      fprintf(stderr, "Alloc drm snapshot props failed.\n");
      return -1;
    }
    drm_info->old_info.props_list = drm_props;
#define CONUM 6
#define CRNUM 4
#define PNUM 14
#define CNUM (CONUM + CRNUM)
#define NUMS (CNUM + PNUM)

    const char *names[] = { "CRTC_ID", "HDR_OUTPUT_METADATA", "Colorspace", "max bpc", "Broadcast RGB", "allm_enable",
                             "MODE_ID", "ACTIVE", "VRR_ENABLED", "GAMMA_LUT",
                             "FB_ID", "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H", "SRC_X", "SRC_Y", "SRC_W", "SRC_H", "rotation", "CRTC_ID", "COLOR_ENCODING", "COLOR_RANGE", "EOTF"
                          };
    uint32_t *props_list[] = { &drm_info->conn_crtc_prop_id, &drm_info->conn_hdr_metadata_prop_id,
                               &drm_info->conn_colorspace_prop_id, &drm_info->conn_max_bpc_prop_id,
                               &drm_info->conn_broadcast_rgb_prop_id, &drm_info->conn_allm_prop_id, 
                               &drm_info->crtc_prop_mode_id, &drm_info->crtc_prop_active,
                               &drm_info->crtc_vrr_prop_id, &drm_info->crtc_gammalut_prop_id,
                               &drm_info->plane_fb_id_prop_id,
                               &drm_info->plane_crtc_x_prop_id, &drm_info->plane_crtc_y_prop_id, 
                               &drm_info->plane_crtc_w_prop_id, &drm_info->plane_crtc_h_prop_id, 
                               &drm_info->plane_src_x_prop_id, &drm_info->plane_src_y_prop_id, 
                               &drm_info->plane_src_w_prop_id, &drm_info->plane_src_h_prop_id, 
                               &drm_info->plane_rotation_prop_id, &drm_info->plane_crtc_prop_id,
                               &drm_info->plane_color_encoding_prop_id, &drm_info->plane_color_range_prop_id,
                               &drm_info->plane_eotf_prop_id,
                             };
    uint64_t tmp_value[NUMS] = {0};
    uint64_t *values_list[NUMS];
    uint32_t ids[NUMS];
    for (int i = 0; i < NUMS; i++) {
      values_list[i] = &tmp_value[i];
      ids[i] = (i < CONUM) ? drm_info->connector_id : ((i < CNUM) ? drm_info->crtc_id : drm_info->plane_id);
    }

    const char **connnames = &names[0], **crtcnames = &names[CONUM], **pnames = &names[CNUM];
    uint32_t **co_props_list = &props_list[0], **cr_props_list = &props_list[CONUM], **p_props_list = &props_list[CNUM];
    uint64_t **co_values_list = &values_list[0], **cr_values_list = &values_list[CONUM], **p_values_list = &values_list[CNUM]; 

    struct _props_ptr co_stores = { .props = co_props_list, .props_value = co_values_list, .props_num = 0 };
    struct _props_ptr cr_stores = { .props = cr_props_list, .props_value = cr_values_list, .props_num = 0 };
    struct _props_ptr p_stores = { .props = p_props_list, .props_value = p_values_list, .props_num = 0 };

    drm_get_props(drm_info->fd, drm_info->connector_id, DRM_MODE_OBJECT_CONNECTOR, connnames, &co_stores, CONUM);
    drm_get_props(drm_info->fd, drm_info->crtc_id, DRM_MODE_OBJECT_CRTC, crtcnames, &cr_stores, CRNUM);
    drm_get_props(drm_info->fd, drm_info->plane_id, DRM_MODE_OBJECT_PLANE, pnames, &p_stores, PNUM);

    int add_count = 0;
    drm_props->snap.ids = calloc(NUMS, sizeof(uint32_t));
    drm_props->snap.props = calloc(NUMS, sizeof(uint32_t));
    drm_props->snap.props_value = calloc(NUMS, sizeof(uint64_t));
    if (drm_props->snap.props != NULL && drm_props->snap.props_value != NULL) {
      for (int i = 0; i < NUMS; i++) {
        if (*props_list[i] != 0) {
          if (*props_list[i] == drm_info->plane_fb_id_prop_id && *values_list[i] == 0 ) {
            break;
          }
          if (*props_list[i] == drm_info->plane_color_encoding_prop_id || *props_list[i] == drm_info->plane_color_range_prop_id ||
              *props_list[i] == drm_info->plane_eotf_prop_id) {
            continue;
          }
          drm_props->snap.ids[add_count] = ids[i];
          drm_props->snap.props[add_count] = *props_list[i];
          drm_props->snap.props_value[add_count] = *values_list[i];
          if (*props_list[i] == drm_info->conn_hdr_metadata_prop_id ||
              *props_list[i] == drm_info->crtc_gammalut_prop_id)
            drm_props->snap.props_value[add_count] = 0;
          if (*props_list[i] == drm_info->crtc_prop_mode_id) {
            drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(drm_info->fd, *values_list[i]);
            drmModeModeInfo *mode = (drmModeModeInfo *)blob->data;
            drmModeCreatePropertyBlob(drm_info->fd, mode, sizeof(*mode), &drm_info->old_info.crtc_mode_blob_id);
            drm_props->snap.props_value[add_count] = (uint64_t)drm_info->old_info.crtc_mode_blob_id;
            drmModeFreePropertyBlob(blob);
          }
          add_count++;
        }
      }
    }
    drm_props->snap.props_num = add_count;
#undef CONUM
#undef CRNUM
#undef PNUM
#undef CNUM
#undef NUMS
  }

  const char *color_space_name[3] = { "ITU-R BT.601 YCbCr", "ITU-R BT.709 YCbCr", "ITU-R BT.2020 YCbCr" };
  drm_get_prop_enum (drm_info->fd, color_space_name, 3, drm_info->plane_color_encoding_prop_id, drm_info->plane_color_encoding_prop_values);
  const char *colorange_name[3] = { "YCbCr limited range", "YCbCr full range", "nonono"};
  drm_get_prop_enum (drm_info->fd, colorange_name, 3, drm_info->plane_color_range_prop_id, drm_info->plane_color_range_prop_values);
  const char *colorspace_name[] = { "Default", "BT2020_RGB", "BT2020_YCC", "BT601_YCC", "BT709_YCC", "DCI-P3_RGB_D65", "SMPTE_170M_YCC" };
  drm_get_prop_enum (drm_info->fd, colorspace_name, 7, drm_info->conn_colorspace_prop_id, drm_info->conn_colorspace_values);
  const char *broadcast_rgb_name[3] = { "Automatic", "Full", "Limited 16:235" };
  drm_get_prop_enum (drm_info->fd, broadcast_rgb_name, 3, drm_info->conn_broadcast_rgb_prop_id, drm_info->conn_broadcast_rgb_prop_values);
  if (drm_info->conn_colorspace_values[D2020YCC] == 0) drm_info->conn_colorspace_values[D2020YCC] = drm_info->conn_colorspace_values[D2020RGB];
  if (drm_info->conn_colorspace_values[D601YCC] == 0 && drm_info->conn_colorspace_values[SMPTE170YCC] == 0) drm_info->conn_colorspace_values[D601YCC] = drm_info->conn_colorspace_values[DEFAULTCOLOR];
  if (drm_info->conn_colorspace_values[D709YCC] == 0) drm_info->conn_colorspace_values[D709YCC] = drm_info->conn_colorspace_values[DEFAULTCOLOR];
  if (drm_info->conn_colorspace_values[D601YCC] > 0 && drm_set_props(drm_info->fd, &drm_info->connector_id, &drm_info->conn_colorspace_prop_id, &drm_info->conn_colorspace_values[D601YCC], 1, DRM_MODE_ATOMIC_TEST_ONLY, DRM_MODE_OBJECT_CONNECTOR, NULL) < 0)
    drm_info->conn_colorspace_values[D601YCC] = drm_info->conn_colorspace_values[DEFAULTCOLOR];
  if (drm_info->conn_colorspace_values[SMPTE170YCC] > 0 && drm_info->conn_colorspace_values[D601YCC] == 0) {
    if (drm_set_props(drm_info->fd, &drm_info->connector_id, &drm_info->conn_colorspace_prop_id, &drm_info->conn_colorspace_values[SMPTE170YCC], 1, DRM_MODE_ATOMIC_TEST_ONLY, DRM_MODE_OBJECT_CONNECTOR, NULL) >= 0)
    drm_info->conn_colorspace_values[D601YCC] = drm_info->conn_colorspace_values[SMPTE170YCC];
  }

  if (format_site < 0)
    fprintf(stderr, "No matched plane format!\n");

  return format_site;
}

static int drm_get_connector_hdr_props (struct Drm_Info *info) {
  if (info->conn_hdr_metadata_prop_id == 0 || info->plane_color_encoding_prop_id == 0) {
    fprintf(stderr, "Could not get hdr property for connector\n");
    return -1;
  }

  return 0;
}

static int drm_set_connector_hdr_mode (struct Drm_Info *info) {
  int ret = -1;

  if (info->conn_colorspace_prop_id != 0) {
    uint32_t list[3] = { 16, 12, 10 };
    uint64_t values[] = { 0, (info->conn_colorspace_values[D2020YCC] > 0) ? info->conn_colorspace_values[D2020YCC] : info->conn_colorspace_values[D2020RGB] };
    uint32_t ids[] = { info->connector_id, info->connector_id };
    uint32_t props[] = { info->conn_max_bpc_prop_id, info->conn_colorspace_prop_id };

    switch (info->connector_type) {
    case DRM_MODE_CONNECTOR_HDMIA:
    case DRM_MODE_CONNECTOR_HDMIB:
    case DRM_MODE_CONNECTOR_eDP:
    case DRM_MODE_CONNECTOR_DisplayPort:
    case DRM_MODE_CONNECTOR_USB:
      for (int i = 0; i < 3; i++) {
        values[0] = list[i];
        fprintf(stderr, "Try enabled %lu-bit HDMI Deep Color: ", values[0] * 3);
        ret = drm_set_props(info->fd, ids, props, values, 2, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, DRM_MODE_OBJECT_CONNECTOR, NULL) * ret;
        if (ret == 0) {
          printf("Has enabled %lu-bit HDMI Deep Color.\n", values[0] * 3);
          if (info->have_atomic) {
            drm_opt_commit (DRM_ADD_COMMIT, NULL, ids[0], props[0], values[0]);
            drm_opt_commit (DRM_ADD_COMMIT, NULL, ids[1], props[1], values[1]);
          }
          break;
        }
      }
  
      if (ret != 0) {
        if (!info->have_atomic) {
          uint64_t default_color[1] = { info->conn_colorspace_values[DEFAULTCOLOR] };
          drm_set_props(info->fd, ids, &props[1], default_color, 1, DRM_MODE_ATOMIC_TEST_ONLY, DRM_MODE_OBJECT_CONNECTOR, NULL);
        }
        fprintf(stderr, "Could not set hdr property for connector\n");
      }
      break;
    }
  }

  return ret;
}

struct Drm_Info * drm_init (const char *device, uint32_t drmformat, bool usehdr) {
  if (device != NULL)
    drm_device = device;
  if (access(drm_device, F_OK) == -1) {
    fprintf(stderr, "No %s device.\n", drm_device);
    return NULL;
  }
  int fd = open(drm_device, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    perror("Could not open drm_device: ");
    return NULL;
  }
  struct Drm_Info *drm_info = calloc(1, sizeof(struct Drm_Info));
  if (drm_info == NULL) {
    perror("Alloc drm_info ptr failed. ");
    close(fd);
    return NULL;
  }

  have_atomic = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0 ? 1 : 0;
  drm_add_fb_func = &drmModeAddFB2WithModifiers;
  uint64_t has_cap = 1;
  drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &has_cap);
  if (has_cap == 0)
    drm_add_fb_func = &drm_add_fb_legacy;

  drm_info->fd = fd;
  drm_info->have_atomic = have_atomic;

  if (drm_choose_crtc(drm_info) < 0) {
    goto exit;
  }

  if (drmformat > 0 && drm_get_plane(drm_info, drmformat) < 0) {
    goto exit;
  }

  if (usehdr) {
    if (drm_get_connector_hdr_props(drm_info) < 0)
      goto exit;
    if (drm_set_connector_hdr_mode(drm_info) < 0)
      goto exit;
  }

  if (have_atomic) {
    drmpageflip = &drmpageflip_atomic;
  }
  else {
    drmpageflip = &drmpageflip_legacy;
  }

  return drm_info;

exit:
  drm_close(&drm_info);
  return NULL;
}

int drm_get_plane_info (struct Drm_Info *drm_info, uint32_t format) {
  for (int formats_index = 0; formats_index < NEEDED_DRM_FORMAT_NUM; formats_index++) {
    if (drm_info->plane_formats[formats_index] == format) {
      return formats_index;
    }
  }

  return drm_get_plane(drm_info, format);
}

void drm_close(struct Drm_Info **drm_info_ptr) {
  if (drm_info_ptr == NULL) return;
  struct Drm_Info *info = *drm_info_ptr;
  if (info == NULL || info->fd < 0) return;
  drm_opt_commit(DRM_CLEAR_LIST, NULL, 0, 0, 0);
  if (info->old_info.props_list) {
    struct _snap_props *drm_props = info->old_info.props_list;
    drm_clear_snap(&drm_props->snap);
    free(info->old_info.props_list);
  }
  if (info->crtc_mode_blob_id != 0)
    drmModeDestroyPropertyBlob(info->fd, info->crtc_mode_blob_id);
  if (info->old_info.crtc_mode_blob_id != 0)
    drmModeDestroyPropertyBlob(info->fd, info->old_info.crtc_mode_blob_id);
  close(info->fd);
  memset(info, 0, sizeof(struct Drm_Info));
  free(info);
  *drm_info_ptr = NULL;
}

void drm_restore_display(struct Drm_Info *info) {
  if (info->old_info.props_list != NULL) {
    struct _snap_props *drm_props = info->old_info.props_list;
    if (info->have_atomic && drm_props->snap.props_num > 0) {
      if (drm_set_props(info->fd, drm_props->snap.ids, drm_props->snap.props, drm_props->snap.props_value, drm_props->snap.props_num, DRM_MODE_ATOMIC_ALLOW_MODESET, 0, NULL) < 0) {
       perror("Restore display failed: ");
      }
    }
    else {
      drmModeSetCrtc(info->fd, info->crtc_id, info->old_info.fb_id, 0, 0, &info->connector_id, 1, &info->old_info.crtc_mode);
    }
  }
}

uint32_t translate_format_to_drm(int format, int *bpp, int *heightmulti, int *planenum) {
  switch (format) {
  case AV_PIX_FMT_X2RGB10LE:
    *bpp = 32;
    *heightmulti = 1;
    *planenum = 1;
    return DRM_FORMAT_XRGB2101010;
  case AV_PIX_FMT_BGR0:
    *bpp = 32;
    *heightmulti = 1;
    *planenum = 1;
    return DRM_FORMAT_XRGB8888;
  case AV_PIX_FMT_BGRA:
    *bpp = 32;
    *heightmulti = 1;
    *planenum = 1;
    return DRM_FORMAT_ARGB8888;
  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUVJ444P:
    *bpp = 8;
    *heightmulti = 3;
    *planenum = 3;
    return DRM_FORMAT_YUV444;
  case AV_PIX_FMT_YUV444P10:
  case AV_PIX_FMT_YUV444P16:
    *bpp = 16;
    *heightmulti = 3;
    *planenum = 3;
    return DRM_FORMAT_Q410;
  case AV_PIX_FMT_VUYX:
  case AV_PIX_FMT_VUYA:
    *bpp = 32;
    *heightmulti = 1;
    *planenum = 1;
    return DRM_FORMAT_XYUV8888;
  case AV_PIX_FMT_XV30:
    *bpp = 32;
    *heightmulti = 1;
    *planenum = 1;
    return DRM_FORMAT_XVYU2101010;
  case AV_PIX_FMT_XV36:
    *bpp = 64;
    *heightmulti = 1;
    *planenum = 1;
    return DRM_FORMAT_XVYU12_16161616;
  case AV_PIX_FMT_XV48:
    *bpp = 64;
    *heightmulti = 1;
    *planenum = 1;
    return DRM_FORMAT_XVYU16161616;
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P:
    *bpp = 8;
    *heightmulti = 2;
    *planenum = 3;
    return DRM_FORMAT_YUV420;
  case AV_PIX_FMT_P010:
    *bpp = 16;
    *heightmulti = 2;
    *planenum = 2;
    return DRM_FORMAT_P010;
  case AV_PIX_FMT_NV12:
    *bpp = 8;
    *heightmulti = 2;
    *planenum = 2;
    return DRM_FORMAT_NV12;
  case AV_PIX_FMT_NV24:
    *bpp = 16;
    *heightmulti = 2;
    *planenum = 2;
    return DRM_FORMAT_NV24;
#if defined(DRM_FORMAT_P410)
  case AV_PIX_FMT_P410:
    *bpp = 32;
    *heightmulti = 2;
    *planenum = 2;
    return DRM_FORMAT_P410;
#endif
  }
  return 0;
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
  int planenum;
  translate_format_to_drm(pixfmt, &bpc, &frameHeightMulti, &planenum);

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
  #undef test_multi

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
static void page_flip_handler(int fd, unsigned int seq, unsigned int sec,
                              unsigned int usec, unsigned int crtc_id, void *data) {
  struct _drm_pageflip_feedback *feedback = data;
  if (crtc_id == feedback->props->crtc_id) {
    feedback->done = 1;
    feedback->tv_sec = sec;
    feedback->tv_nsec = usec * 1000ULL;
    feedback->seq = seq;
  }
  return;
}

static drmEventContext evctx = {
  .version = DRM_EVENT_CONTEXT_VERSION,
  .page_flip_handler2 = page_flip_handler,
};

static int drmpageflip_legacy(uint32_t fd, struct _pageflip_prop *props, uint32_t fb_id, uint32_t flags, void *data) {
  return drmModePageFlip(fd, props->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, data);
}

static int drmpageflip_atomic(uint32_t fd, struct _pageflip_prop *props, uint32_t fb_id, uint32_t flags, void *data) {
  int ret = -1;
  uint32_t cflags = flags | DRM_MODE_PAGE_FLIP_EVENT;

  if (fb_id == 0)
    return -1;

  drmModeAtomicReq *req = drmModeAtomicAlloc();

  if (!req)
    return -1;

  if (drm_opt_commit (DRM_APPLY_COMMIT, req, 0, 0, 0) > 0) {
    cflags = (cflags & ~(DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_ASYNC)) | DRM_MODE_ATOMIC_ALLOW_MODESET;
    ((struct _drm_pageflip_feedback *)data)->timeout_sec = 6;
  }

  drmModeAtomicAddProperty(req, props->plane_id, props->plane_fb_prop, fb_id);

  ret = drmModeAtomicCommit(fd, req, cflags, data);
  if (ret < 0) {
    ret = -errno;
    perror("Drm cannot atomic page flip: ");
  }
  drmModeAtomicFree(req);

  return ret;
}

int drm_flip_buffer(uint32_t fd, uint32_t fb_id, uint64_t flags, struct _drm_pageflip_feedback *feedback) {
  if (feedback == NULL || feedback->props == NULL) {
    fprintf(stderr, "DRM: feedback is NULL.\n");
    return -1;
  }

  int res = drmpageflip(fd, feedback->props, fb_id, flags, feedback);
  if (res < 0) {
    fprintf(stderr, "drmModePageFlip() failed: %d\n", res);
    return res;
  }

  struct pollfd pfd = { .fd = fd, .events = POLLIN };
  struct timespec wait = {0};
  wait.tv_sec = feedback->timeout_sec;
  wait.tv_nsec = feedback->timeout_nsec;
  while ((ppoll(&pfd, 1, &wait, NULL)) > 0) {
    drmHandleEvent(fd, &evctx);
    if (feedback->done != 0) {
      return 0;
    }
  }

  return -EAGAIN;
}

int drm_set_display(struct Drm_Info *info, uint32_t src_width, uint32_t src_height, drmModeModeInfoPtr connModePtr, uint32_t fb_id) {
  if (info->have_atomic) {
    int dst_site_x = 0;
    int dst_site_y = 0;
    uint32_t dst_site_width = info->width;
    uint32_t dst_site_height = info->height;
    convert_display(&src_width, &src_height, &dst_site_width, &dst_site_height, &dst_site_x, &dst_site_y);

    if (info->crtc_mode_blob_id == 0) {
      if (drmModeCreatePropertyBlob(info->fd, connModePtr, sizeof(*connModePtr), &info->crtc_mode_blob_id) != 0) {
        perror("Cannot create blob for mode: ");
        return -1;
      }
    }
    
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->connector_id, info->conn_crtc_prop_id, info->crtc_id);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->crtc_id, info->crtc_prop_active, 1);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->crtc_id, info->crtc_prop_mode_id, info->crtc_mode_blob_id);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->crtc_id, info->crtc_gammalut_prop_id, 0);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->plane_id, info->plane_crtc_prop_id, info->crtc_id);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->plane_id, info->plane_src_x_prop_id, 0 << 16);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->plane_id, info->plane_src_y_prop_id, 0 << 16);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->plane_id, info->plane_src_w_prop_id, src_width << 16);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->plane_id, info->plane_src_h_prop_id, src_height << 16);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->plane_id, info->plane_crtc_x_prop_id, dst_site_x);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->plane_id, info->plane_crtc_y_prop_id, dst_site_y);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->plane_id, info->plane_crtc_w_prop_id, dst_site_width);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, info->plane_id, info->plane_crtc_h_prop_id, dst_site_height);

    return 0;
  } else {
    int ret = drmModeSetCrtc(info->fd, info->crtc_id, fb_id, 0, 0, &info->connector_id, 1, connModePtr);
    if (ret < 0) {
      fprintf(stderr, "Could not set fb to drm crtc.\n");
      return ret;
    }
  }

  return 0;
}

int drm_choose_color_config (struct Drm_Info *info, enum DrmColorSpace colorspace, bool fullRange) {
  uint32_t ids[] = { info->plane_id, info->plane_id };
  uint32_t props[] = { info->plane_color_encoding_prop_id, info->plane_color_range_prop_id };
  uint64_t values[] = { info->plane_color_encoding_prop_values[colorspace], info->plane_color_range_prop_values[fullRange ? 1 : 0] };

  if (info->have_atomic) {
    drm_opt_commit (DRM_ADD_COMMIT, NULL, ids[0], props[0], values[0]);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, ids[1], props[1], values[1]);
  } else {
    if (drm_set_props(info->fd, ids, props, values, 2, 0, DRM_MODE_OBJECT_PLANE, NULL) < 0) {
      perror("Set plane color space and range failed.");
      return -1;
    }
  }

  return 0;
}

/*
int drm_apply_hdr_metadata(int fd, uint32_t conn_id, uint32_t hdr_metadata_prop_id, struct hdr_output_metadata *data) {
  uint32_t blob_id;
  if (drmModeCreatePropertyBlob(fd, data, sizeof(struct hdr_output_metadata), &blob_id) < 0) {
    perror("Failed to create hdr metadata blob: ");
    return -1;
  }

  uint64_t blob[1] = { (uint64_t)blob_id };
  if (drm_set_props(fd, &conn_id, &hdr_metadata_prop_id, blob, 1, DRM_MODE_ATOMIC_ALLOW_MODESET, DRM_MODE_OBJECT_CONNECTOR, NULL) < 0) {
    perror("Failed to set hdr metadata blob: ");
    drmModeDestroyPropertyBlob(fd, blob_id);
    return -1;
  }

  drmModeDestroyPropertyBlob(fd, blob_id);
  return 0;
}
*/

int drm_add_fb (int fd, uint32_t width, uint32_t height, uint32_t pixel_format, const uint32_t bo_handles[4], const uint32_t pitches[4], const uint32_t offsets[4], const uint64_t modifier[4], uint32_t *buf_id, uint32_t flags) {
  return drm_add_fb_func(fd, width, height, pixel_format, bo_handles, pitches, offsets, modifier, buf_id, flags);
}
