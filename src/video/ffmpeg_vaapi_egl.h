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

enum PixelFormatOrder { VUYX_ORDER = 0, XVYU_ORDER, YUVX_ORDER };

void vaapi_free_egl_images(EGLDisplay dpy, EGLImage images[4]);
ssize_t vaapi_export_egl_images(AVFrame *frame, EGLDisplay dpy, bool eglIsSupportExtDmaBufMod,
                        EGLImage images[4]);
int vaapi_get_plane_info(enum AVPixelFormat **pix_fmt, int *plane_num, enum PixelFormatOrder *plane_order);
