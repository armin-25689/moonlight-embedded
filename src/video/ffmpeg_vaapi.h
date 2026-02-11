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

#include "render.h"

int vaapi_init_lib(const char *device);
int vaapi_init(AVCodecContext* decoder_ctx);
bool vaapi_validate_test(char *displayName, char *renderName, void *nativeDisplay);
int vaapi_supported_video_format(void);
void *vaapi_get_descriptors_ptr(void);
void vaapi_free_render_images(void **renderImages, void *descriptor, void(*render_unmap_buffer)(void* *image, int planes));
ssize_t vaapi_export_render_images(AVFrame *frame, struct Render_Image *image, void *descriptor, int render_type,
                                int(*render_map_buffer)(struct Source_Buffer_Info *buffer,
                                                        int planes, int composeOrSeperate,
                                                        void** image, int index),
                                void(*render_unmap_buffer)(void **image, int planes));
ssize_t software_store_frame (AVFrame *frame, struct Render_Image *image, void *descriptor, int render_type,
                                     int(*render_map_buffer)(struct Source_Buffer_Info *buffer,
                                                             int planes, int composeOrSeperate,
                                                             void* *image, int index),
                                     void(*render_unmap_buffer)(void **image, int planes));
