
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_drm.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>

#include "drm_base.h"

static struct Drm_Info current_drm_info = {0},old_drm_info = {0};
static const char *drm_device = "/dev/dri/card0";

struct {
  int x;
  int y;
  uint32_t width;
  uint32_t height;
} static dst_site = {0};

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
} static drm_props = {0};

struct _commit_list {
  uint32_t device_id;
  uint32_t prop_id;
  uint64_t value;
};

static int (*drmpageflip) (uint32_t fd, uint32_t crtc_id, uint32_t fb_id, uint64_t hdr_blob, uint32_t width, uint32_t height, void *data);
static int drmpageflip_legacy(uint32_t fd, uint32_t crtc_id, uint32_t fb_id, uint64_t hdr_blob, uint32_t width, uint32_t height, void *data);
static int drmpageflip_atomic(uint32_t fd, uint32_t crtc_id, uint32_t fb_id, uint64_t hdr_blob, uint32_t width, uint32_t height, void *data);

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
  // opt 0 is add, 1 is get, 2 is clear;
  struct {
    struct _commit_list *list;
    uint32_t count;
    size_t slot;
  } static commit_list = { .list = NULL, .count = 0, .slot = 10, },
           restore_list = { 0 };

  switch (opt) {
  case DRM_ADD_COMMIT:
    if (commit_list.list == NULL) commit_list.list = calloc(commit_list.slot, sizeof(struct _commit_list));
    if (commit_list.count > commit_list.slot) {
      commit_list.list = realloc(commit_list.list, sizeof(struct _commit_list) * commit_list.slot * 2);
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
    commit_list.slot = 10;
    commit_list.count = 0;
    restore_list.list = NULL;
    restore_list.count = 0;
    return 0;
  }

  #undef MAX_PROP_SLOT_NUM
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
  if (current_drm_info.have_atomic) {
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
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 && conn->encoder_id != 0) {
      if (index > MAX_CONNECTOR)
        continue;
      connSize[index] = conn->mmWidth * conn->mmHeight;
      conns[index++] = conn->connector_id;
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
        current_drm_info.connector_type = conn->connector_type;
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
    fprintf(stderr, "Not found connector or encoder for drm.\n");
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

static int drm_get_plane (struct Drm_Info *drm_info, uint32_t format) {
  int format_site;

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
    if (!plane)
      continue;

    int formats_index = 0;
    format_site = -1;
    memset(drm_info->plane_formats, 0, sizeof(drm_info->plane_formats));
    for (int j = 0; j < plane->count_formats; j++) {
      switch (plane->formats[j]) {
      case DRM_FORMAT_XYUV8888:
      case DRM_FORMAT_XVYU2101010:
      case DRM_FORMAT_YUV420:
      case DRM_FORMAT_YUV444:
      case DRM_FORMAT_NV12:
      case DRM_FORMAT_P010:
      case DRM_FORMAT_XRGB8888:
      case DRM_FORMAT_XRGB2101010:
        drm_info->plane_formats[formats_index++] = plane->formats[j];
        break;
      }
      if (format == plane->formats[j]) {
        format_site = formats_index - 1;
      }
      if (formats_index >= NEEDED_DRM_FORMAT_NUM)
        break;
    }

    if (formats_index == 0) {
      drmModeFreePlane(plane);
      continue;
    }

    if ((plane->possible_crtcs & (1 << drm_info->crtc_index))) {
      drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drm_info->fd,res->planes[i], DRM_MODE_OBJECT_PLANE);
      if (!props) {
        drmModeFreePlane(plane);
        continue;
      }

      for (int j = 0; j < props->count_props; j++) {
        drmModePropertyPtr prop = drmModeGetProperty(drm_info->fd, props->props[j]);
        if (!prop)
          continue;
        if (strcmp(prop->name, "type") == 0 && (props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY || props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY)) {
          drm_info->plane_id = plane->plane_id;
          drmModeFreeProperty(prop);
          break;
        }
        drmModeFreeProperty(prop);
      }
      drmModeFreeObjectProperties(props);
    }
    drmModeFreePlane(plane);

    if (format_site != -1)
      break;
  }
  drmModeFreePlaneResources(res);

  if (drm_info->plane_id == 0) {
    fprintf(stderr, "Could not get plane info\n");
    return -1;
  }

  if (drm_props.snap.props_num == 0) {
#define CONUM 6
#define CRNUM 3
#define PNUM 14
#define CNUM 9
#define NUMS 23
    uint32_t ids[] = { drm_info->connector_id, drm_info->connector_id, drm_info->connector_id, drm_info->connector_id,
                       drm_info->connector_id, drm_info->connector_id,  
                       drm_info->crtc_id, drm_info->crtc_id, drm_info->crtc_id, 
                       drm_info->plane_id, drm_info->plane_id,  
                       drm_info->plane_id, drm_info->plane_id, drm_info->plane_id, drm_info->plane_id,  
                       drm_info->plane_id, drm_info->plane_id, drm_info->plane_id, drm_info->plane_id,  
                       drm_info->plane_id, drm_info->plane_id, drm_info->plane_id, drm_info->plane_id,
                     };
    const char *names[] = { "CRTC_ID", "HDR_OUTPUT_METADATA", "Colorspace", "max bpc", "Broadcast RGB", "allm_enable",
                             "MODE_ID", "ACTIVE", "VRR_ENABLED",
                             "FB_ID", "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H", "SRC_X", "SRC_Y", "SRC_W", "SRC_H", "rotation", "CRTC_ID", "COLOR_ENCODING", "COLOR_RANGE", "EOTF"
                          };
    uint32_t *props_list[] = { &drm_info->conn_crtc_prop_id, &drm_info->conn_hdr_metadata_prop_id,
                               &drm_info->conn_colorspace_prop_id, &drm_info->conn_max_bpc_prop_id,
                               &drm_info->conn_broadcast_rgb_prop_id, &drm_info->conn_allm_prop_id, 
                               &drm_info->crtc_prop_mode_id, &drm_info->crtc_prop_active,
                               &drm_info->crtc_vrr_prop_id, &drm_info->plane_fb_id_prop_id,
                               &drm_info->plane_crtc_x_prop_id, &drm_info->plane_crtc_y_prop_id, 
                               &drm_info->plane_crtc_w_prop_id, &drm_info->plane_crtc_h_prop_id, 
                               &drm_info->plane_src_x_prop_id, &drm_info->plane_src_y_prop_id, 
                               &drm_info->plane_src_w_prop_id, &drm_info->plane_src_h_prop_id, 
                               &drm_info->plane_rotation_prop_id, &drm_info->plane_crtc_prop_id,
                               &drm_info->plane_color_encoding_prop_id, &drm_info->plane_color_range_prop_id,
                               &drm_info->plane_eotf_prop_id,
                             };
    uint64_t tmp_value[NUMS] = {0};
    uint64_t *values_list[] = { &tmp_value[0], &tmp_value[1], &tmp_value[2], &tmp_value[3], &tmp_value[4], &tmp_value[5], &tmp_value[6], &tmp_value[7], &tmp_value[8], &tmp_value[9],
                                &tmp_value[10], &tmp_value[11], &tmp_value[12], &tmp_value[13], &tmp_value[14], &tmp_value[15], &tmp_value[16], &tmp_value[17], &tmp_value[18], &tmp_value[19],
                                &tmp_value[20], &tmp_value[20], &tmp_value[21],
                              };

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
    drm_props.snap.ids = calloc(NUMS, sizeof(uint32_t));
    drm_props.snap.props = calloc(NUMS, sizeof(uint32_t));
    drm_props.snap.props_value = calloc(NUMS, sizeof(uint64_t));
    if (drm_props.snap.props != NULL && drm_props.snap.props_value != NULL) {
      for (int i = 0; i < NUMS; i++) {
        if (*props_list[i] != 0) {
          if (*props_list[i] == drm_info->plane_fb_id_prop_id && *values_list[i] == 0 ) {
            break;
          }
          if (*props_list[i] == drm_info->plane_color_encoding_prop_id || *props_list[i] == drm_info->plane_color_range_prop_id ||
              *props_list[i] == drm_info->plane_eotf_prop_id) {
            continue;
          }
          drm_props.snap.ids[add_count] = ids[i];
          drm_props.snap.props[add_count] = *props_list[i];
          drm_props.snap.props_value[add_count] = *values_list[i];
          if (*props_list[i] == drm_info->conn_hdr_metadata_prop_id)
            drm_props.snap.props_value[add_count] = 0;
          if (*props_list[i] == drm_info->crtc_prop_mode_id) {
            drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(drm_info->fd, *values_list[i]);
            drmModeModeInfo *mode = (drmModeModeInfo *)blob->data;
            drmModeCreatePropertyBlob(drm_info->fd, mode, sizeof(*mode), &old_drm_info.crtc_mode_blob_id);
            drm_props.snap.props_value[add_count] = (uint64_t)old_drm_info.crtc_mode_blob_id;
            drmModeFreePropertyBlob(blob);
          }
          add_count++;
        }
      }
    }
    drm_props.snap.props_num = add_count;
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
  const char *colorspace_name[3] = { "Default", "BT2020_RGB", "BT2020_YCC" };
  drm_get_prop_enum (drm_info->fd, colorspace_name, 3, drm_info->conn_colorspace_prop_id, drm_info->conn_colorspace_values);
  const char *broadcast_rgb_name[3] = { "Automatic", "Full", "Limited 16:235" };
  drm_get_prop_enum (drm_info->fd, broadcast_rgb_name, 3, drm_info->conn_broadcast_rgb_prop_id, drm_info->conn_broadcast_rgb_prop_values);

  if (format_site < 0)
    fprintf(stderr, "No matched plane format!\n");

  return format_site;
}

static int drm_choose_plane (int fd, uint32_t format) {
  return drm_get_plane (&current_drm_info, format);
}

static int drm_get_connector_hdr_props (int fd) {
  if (current_drm_info.conn_hdr_metadata_prop_id == 0 || current_drm_info.plane_color_encoding_prop_id == 0) {
    fprintf(stderr, "Could not get hdr property for connector\n");
    return -1;
  }

  return 0;
}

static int drm_set_connector_hdr_mode (int fd, uint32_t flags) {
  int ret = -1;

  if (current_drm_info.conn_colorspace_prop_id != 0) {
    uint32_t list[3] = { 16, 12, 10 };
    uint64_t values[] = { 0, (current_drm_info.conn_colorspace_values[D2020YCC] > 0) ? current_drm_info.conn_colorspace_values[D2020YCC] : current_drm_info.conn_colorspace_values[D2020RGB] };
    uint32_t ids[] = { current_drm_info.connector_id, current_drm_info.connector_id };
    uint32_t props[] = { current_drm_info.conn_max_bpc_prop_id, current_drm_info.conn_colorspace_prop_id };

    switch (current_drm_info.connector_type) {
    case DRM_MODE_CONNECTOR_HDMIA:
    case DRM_MODE_CONNECTOR_HDMIB:
    case DRM_MODE_CONNECTOR_eDP:
    case DRM_MODE_CONNECTOR_DisplayPort:
      for (int i = 0; i < 3; i++) {
        values[0] = list[i];
        fprintf(stderr, "Try enabled %lu-bit HDMI Deep Color: ", values[0] * 3);
        ret = drm_set_props(current_drm_info.fd, ids, props, values, 2, flags | DRM_MODE_ATOMIC_ALLOW_MODESET, DRM_MODE_OBJECT_CONNECTOR, NULL) * ret;
        if (ret == 0) {
          printf("Has enabled %lu-bit HDMI Deep Color.\n", values[0] * 3);
          if (current_drm_info.have_atomic) {
            drm_opt_commit (DRM_ADD_COMMIT, NULL, ids[0], props[0], values[0]);
            drm_opt_commit (DRM_ADD_COMMIT, NULL, ids[1], props[1], values[1]);
          } else {
            drm_set_props(current_drm_info.fd, ids, props, values, 2, DRM_MODE_ATOMIC_ALLOW_MODESET, DRM_MODE_OBJECT_CONNECTOR, NULL);
          }
          break;
        }
      }
  
      if (ret != 0) {
        uint64_t default_color[1] = { current_drm_info.conn_colorspace_values[DEFAULTCOLOR] };
        drm_set_props(current_drm_info.fd, ids, &props[1], default_color, 1, DRM_MODE_ATOMIC_TEST_ONLY, DRM_MODE_OBJECT_CONNECTOR, NULL);
        fprintf(stderr, "Could not set hdr property for connector\n");
      }
      break;
/*
      ret = drm_set_props(current_drm_info.fd, &ids[1], &props[1], &values[1], 1, DRM_MODE_ATOMIC_ALLOW_MODESET, DRM_MODE_OBJECT_CONNECTOR, NULL);
      break;
*/
    }
  }

  return ret;
}

struct Drm_Info * drm_init (const char *device, uint32_t drmformat, bool usehdr) {
  if (device != NULL)
    drm_device = device;
  if (access("/dev/dri/card0", F_OK) == -1) {
    fprintf(stderr, "No /dev/dri/card0 device.\n");
    return NULL;
  }
  current_drm_info.fd = -1;
  current_drm_info.fd = open(drm_device, O_RDWR | O_CLOEXEC);
  if (current_drm_info.fd < 0) {
    perror("Could not open /dev/dri/card0");
    return NULL;
  }

  current_drm_info.have_atomic = drmSetClientCap(current_drm_info.fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0 ? 1 : 0;

  if (drm_choose_crtc(current_drm_info.fd) < 0) {
    goto exit;
  }

  if (drmformat > 0 && drm_choose_plane(current_drm_info.fd, drmformat) < 0) {
    goto exit;
  }

  if (usehdr) {
    if (drm_get_connector_hdr_props(current_drm_info.fd) < 0)
      goto exit;
    if (drm_set_connector_hdr_mode(current_drm_info.fd, DRM_MODE_ATOMIC_TEST_ONLY) < 0)
      goto exit;
  }

  if (current_drm_info.have_atomic) {
    drmpageflip = &drmpageflip_atomic;
  }
  else {
    drmpageflip = &drmpageflip_legacy;
  }

  return &current_drm_info;

exit:
  close(current_drm_info.fd);
  current_drm_info.fd = -1;
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

void drm_close() {
  drm_opt_commit(DRM_CLEAR_LIST, NULL, 0, 0, 0);
  drm_clear_snap(&drm_props.snap);
  if (current_drm_info.crtc_mode_blob_id != 0)
    drmModeDestroyPropertyBlob(current_drm_info.fd, current_drm_info.crtc_mode_blob_id);
  if (old_drm_info.crtc_mode_blob_id != 0)
    drmModeDestroyPropertyBlob(current_drm_info.fd, old_drm_info.crtc_mode_blob_id);
  close(current_drm_info.fd);
  memset(&current_drm_info, 0, sizeof(current_drm_info));
  current_drm_info.fd = -1;
}

void drm_restore_display() {
  if (current_drm_info.connector_id != 0) {
    if (current_drm_info.have_atomic && drm_props.snap.props_num > 0) {
      if (drm_set_props(current_drm_info.fd, drm_props.snap.ids, drm_props.snap.props, drm_props.snap.props_value, drm_props.snap.props_num, DRM_MODE_ATOMIC_ALLOW_MODESET, 0, NULL) < 0) {
       perror("Restore display failed: ");
      }
    }
    else {
      drmModeSetCrtc(current_drm_info.fd, current_drm_info.crtc_id, old_drm_info.crtc_fb_id, 0, 0, &current_drm_info.connector_id, 1, &old_drm_info.crtc_mode);
    }
  }
}

int translate_format_to_drm(int format, int *bpp, int *heightmulti, int *planenum) {
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
  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUVJ444P:
    *bpp = 8;
    *heightmulti = 3;
    *planenum = 3;
    return DRM_FORMAT_YUV444;
  case AV_PIX_FMT_VUYX:
    *bpp = 32;
    *heightmulti = 1;
    *planenum = 1;
    return DRM_FORMAT_XYUV8888;
  case AV_PIX_FMT_XV30:
    *bpp = 32;
    *heightmulti = 1;
    *planenum = 1;
    return DRM_FORMAT_XVYU2101010;
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
static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void *data) {
  int *done = data;
  *done = 1;
}

static drmEventContext evctx = {
  .version = DRM_EVENT_CONTEXT_VERSION,
  .page_flip_handler = page_flip_handler,
};

static int drmpageflip_legacy(uint32_t fd, uint32_t crtc_id, uint32_t fb_id, uint64_t hdr_blob, uint32_t width, uint32_t height, void *data) {
  return drmModePageFlip(fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, data);
}

static int drmpageflip_atomic(uint32_t fd, uint32_t crtc_id, uint32_t fb_id, uint64_t hdr_data, uint32_t width, uint32_t height, void *data) {
  int ret = -1;
  uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT;
  // | DRM_MODE_ATOMIC_NONBLOCK;
  drmModeAtomicReq *req = drmModeAtomicAlloc();

  if (!req)
    return -1;

  if (drm_opt_commit (DRM_APPLY_COMMIT, req, 0, 0, 0) > 0) flags = (flags & ~DRM_MODE_ATOMIC_NONBLOCK) | DRM_MODE_ATOMIC_ALLOW_MODESET;
  if (fb_id > 0)
    drmModeAtomicAddProperty(req, current_drm_info.plane_id, current_drm_info.plane_fb_id_prop_id, fb_id);
  drmModeAtomicAddProperty(req, current_drm_info.plane_id, current_drm_info.plane_crtc_prop_id, crtc_id);
  drmModeAtomicAddProperty(req, current_drm_info.plane_id, current_drm_info.plane_src_x_prop_id, 0 << 16);
  drmModeAtomicAddProperty(req, current_drm_info.plane_id, current_drm_info.plane_src_y_prop_id, 0 << 16);
  drmModeAtomicAddProperty(req, current_drm_info.plane_id, current_drm_info.plane_src_w_prop_id, width << 16);
  drmModeAtomicAddProperty(req, current_drm_info.plane_id, current_drm_info.plane_src_h_prop_id, height << 16);
  drmModeAtomicAddProperty(req, current_drm_info.plane_id, current_drm_info.plane_crtc_x_prop_id, dst_site.x);
  drmModeAtomicAddProperty(req, current_drm_info.plane_id, current_drm_info.plane_crtc_y_prop_id, dst_site.y);
  drmModeAtomicAddProperty(req, current_drm_info.plane_id, current_drm_info.plane_crtc_w_prop_id, dst_site.width);
  drmModeAtomicAddProperty(req, current_drm_info.plane_id, current_drm_info.plane_crtc_h_prop_id, dst_site.height);

  ret = drmModeAtomicCommit(fd, req, flags, data);
  drmModeAtomicFree(req);
  if (ret < 0)
    perror("Drm cannot atomic page flip: ");

  return ret;
}

int drm_flip_buffer(uint32_t fd, uint32_t crtc_id, uint32_t fb_id, uint64_t hdr_blob, uint32_t width, uint32_t height) {
  int done = 0;
  struct pollfd pfd = { .fd = fd, .events = POLLIN };

  int res = drmpageflip(fd, crtc_id, fb_id, hdr_blob, width, height, &done);
  if (res < 0) {
    fprintf(stderr, "drmModePageFlip() failed: %d", res);
    return -1;
  }

  while (!done && (poll(&pfd, 1, 100)) > 0) {
    drmHandleEvent(fd, &evctx);
  }

  return 0;
}

int drm_set_display(int fd, uint32_t crtc_id, uint32_t src_width, uint32_t src_height, uint32_t crtc_w, uint32_t crtc_h, uint32_t *connector_id, uint32_t connector_num, drmModeModeInfoPtr connModePtr, uint32_t fb_id) {
  if (current_drm_info.have_atomic) {
    dst_site.width = crtc_w;
    dst_site.height = crtc_h;
    convert_display(&src_width, &src_height, &dst_site.width, &dst_site.height, &dst_site.x, &dst_site.y);

    if (current_drm_info.crtc_mode_blob_id == 0) {
      if (drmModeCreatePropertyBlob(fd, connModePtr, sizeof(*connModePtr), &current_drm_info.crtc_mode_blob_id) != 0) {
        perror("Cannot create blob for mode: ");
        return -1;
      }
    }
    
    drm_opt_commit (DRM_ADD_COMMIT, NULL, *connector_id, current_drm_info.conn_crtc_prop_id, crtc_id);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, current_drm_info.crtc_id, current_drm_info.crtc_prop_mode_id, current_drm_info.crtc_mode_blob_id);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, current_drm_info.crtc_id, current_drm_info.crtc_prop_active, 1);

    return 0;
  } else {
    if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, connector_id, connector_num, connModePtr) < 0) {
      fprintf(stderr, "Could not set fb to drm crtc.\n");
      return -1;
    }
  }

  return 0;
}

int drm_choose_color_config (enum DrmColorSpace colorspace, bool fullRange) {
  uint32_t ids[] = { current_drm_info.plane_id, current_drm_info.plane_id };
  uint32_t props[] = { current_drm_info.plane_color_encoding_prop_id, current_drm_info.plane_color_range_prop_id };
  uint64_t values[] = { current_drm_info.plane_color_encoding_prop_values[colorspace], current_drm_info.plane_color_range_prop_values[fullRange ? 1 : 0] };

  if (current_drm_info.have_atomic) {
    drm_opt_commit (DRM_ADD_COMMIT, NULL, ids[0], props[0], values[0]);
    drm_opt_commit (DRM_ADD_COMMIT, NULL, ids[1], props[1], values[1]);
  } else {
    if (drm_set_props(current_drm_info.fd, ids, props, values, 2, 0, DRM_MODE_OBJECT_PLANE, NULL) < 0) {
      perror("Set plane color space and range failed.");
      return -1;
    }
  }

  return 0;
}

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
