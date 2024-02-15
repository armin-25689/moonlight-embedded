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

#include <EGL/egl.h>
#include <va/va.h>
#include <X11/Xlib.h>
#include <stdbool.h>

int vaapi_init_lib(const char *device);
int vaapi_init(AVCodecContext* decoder_ctx);
void vaapi_queue(AVFrame* dec_frame, Window win, int width, int height);
bool test_vaapi_queue(AVFrame* dec_frame, Window win, int width, int height);
void freeEGLImages(EGLDisplay dpy, EGLImage images[4]);
ssize_t exportEGLImages(AVFrame *frame, EGLDisplay dpy, bool eglIsSupportExtDmaBufMod,
                        EGLImage images[4]);
bool canExportSurfaceHandle(bool isTenBit);
bool isVaapiCanDirectRender();
bool isFrameFullRange(const AVFrame* frame);
int getFrameColorspace(const AVFrame* frame);
void *get_display_from_vaapi(bool isXDisplay);
