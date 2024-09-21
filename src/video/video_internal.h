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
enum PixelFormatOrder { VUYX_ORDER = 0, XVYU_ORDER, YUVX_ORDER };
enum decoders {SOFTWARE, VDPAU, VAAPI};
#define EGL_RENDER 0x0100
#define X11_RENDER 0x0200
#define DRM_RENDER 0x0400
#define VULKAN_RENDER 0x0800
#define MAX_FB_NUM 3

struct Source_Buffer_Info {
  uint32_t fd;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t stride;
  uint32_t offset;
  void *data;
};

extern bool isYUV444;
extern bool useHdr;
