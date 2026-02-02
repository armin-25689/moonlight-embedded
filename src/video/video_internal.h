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

#pragma once

#include <stdbool.h>

enum PixelFormatOrder { VUYX_ORDER = 0, XVYU_ORDER, YUVX_ORDER };
enum decoders {SOFTWARE = 0, VDPAU, VAAPI};
#define EGL_RENDER 0x0100
#define X11_RENDER 0x0200
#define DRM_RENDER 0x0400
#define WAYLAND_RENDER 0x0800
#define RENDER_MASK (EGL_RENDER | X11_RENDER | DRM_RENDER | WAYLAND_RENDER)
// argument for render_map_buffer
#define COMPOSE_PLANE 0
#define SEPERATE_PLANE 1

#define MAX_FB_NUM 3
#define MAX_PLANE_NUM 4

#define GET_FB_NEXT(nowindex, maxnum) ((nowindex) >= (maxnum - 1) ? 0 : (nowindex + 1))
#define MV_FB_MEM_SIMPLE(object, size) \
    for (int var = size -1; var > 0; var--) object[var] = object[var - 1];
#define MV_FB_MEM(object, size) \
    memmove(object + 1, object, size);
#define MV_FB_MEM_LOOP(object, size, tmptype) \
  do { \
    tmptype = object[size - 1]; \
    for (int var = size -1; var > 0; var--) object[var] = object[var - 1]; \
    object[0] = tmptype; \
  } while (0)

#define VLIST_CREATE(name, maxslot) \
  typedef struct {                  \
    uint32_t last;                  \
    void* frame[maxslot];           \
    void* data[maxslot];            \
  } VLIST_##name

#define VLIST_INIT(name, maxslot) \
  static VLIST_##name name##_vlist = { 0 }; \
  static const int name##_vlist_max_slot = (maxslot);

#define VLIST_ADD(name, object, vdata) \
  do { \
  if (name##_vlist.last < name##_vlist_max_slot) { \
    MV_FB_MEM_SIMPLE(name##_vlist.frame, name##_vlist_max_slot); \
    MV_FB_MEM_SIMPLE(name##_vlist.data, name##_vlist_max_slot); \
    name##_vlist.frame[0] = object; \
    name##_vlist.data[0] = vdata; \
    name##_vlist.last++; \
  } \
  } while (0)

#define VLIST_NUM(name) name##_vlist.last

#define VLIST_DEL(name) \
  do { \
  if (name##_vlist.last > 0) { \
    name##_vlist.last--; \
    name##_vlist.frame[name##_vlist.last] = NULL; \
    name##_vlist.data[name##_vlist.last] = NULL;  \
  } \
  } while (0)

#define VLIST_GET_FRAME(name) ((name##_vlist.last) > 0 ? (name##_vlist.frame[((name##_vlist.last) - 1)]) : NULL)
#define VLIST_GET_DATA(name) ((name##_vlist.last) > 0 ? (name##_vlist.data[((name##_vlist.last) - 1)]) : NULL)

struct Source_Buffer_Info {
  int fd[4];
  uint32_t width[4];
  uint32_t height[4];
  uint32_t format[4];
  uint32_t stride[4];
  uint32_t offset[4];
  uint64_t modifiers[4];
  void *data;
};

extern bool isYUV444;
extern bool useHdr;
