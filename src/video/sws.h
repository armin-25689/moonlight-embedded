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

struct SwsContext* sws_init(int src_w, int src_h, int src_fmt, int dst_w, int dst_h, int dst_fmt, int threads_count);
int convert_frame_to_packet_format(struct SwsContext *sws_ctx, AVFrame *dst_frame, AVFrame *src_frame, uint8_t *data, int data_size, int pitch);
