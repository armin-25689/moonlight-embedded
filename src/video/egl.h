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
#include <libavcodec/avcodec.h>

void egl_init(void *native_display, int frame_width, int frame_height, int screen_width, int screen_height, int dcFlag);
void egl_draw(AVFrame* frame, uint8_t* image[3]);
void egl_draw_frame(AVFrame* frame);
void egl_destroy();
