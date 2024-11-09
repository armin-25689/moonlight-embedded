/*
 * This file is part of Moonlight Embedded.
 * Copy from moonlight-qt
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

#include <libswscale/swscale.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
/*
dst_frame->format = AV_PIX_FMT_0RGB;
av_frame_alloc();
av_frame_free();
*/
static void free_data(void *data, uint8_t *image) {};

struct SwsContext* sws_init(int src_w, int src_h, int src_fmt, int dst_w, int dst_h, int dst_fmt, int threads_count) {
  struct SwsContext *sws_ctx = sws_alloc_context();
  if (sws_ctx == NULL) {
    fprintf(stderr, "Cannot alloc sws context\n");
    return NULL;
  }

  AVDictionary *ops = NULL;
  av_dict_set_int(&ops, "srcw", src_w, 0);
  av_dict_set_int(&ops, "srch", src_h, 0);
  av_dict_set_int(&ops, "src_format", src_fmt, 0);
  av_dict_set_int(&ops, "dstw", dst_w, 0);
  av_dict_set_int(&ops, "dsth", dst_h, 0);
  av_dict_set_int(&ops, "dst_format", dst_fmt, 0);
  av_dict_set_int(&ops, "threads", threads_count, 0);

  int res = av_opt_set_dict(sws_ctx, &ops);
  av_dict_free(&ops);
  if (res < 0) {
    fprintf(stderr, "Cannot config sws context\n");
    goto failed;
  }

  res = sws_init_context(sws_ctx, NULL, NULL);
  if (res < 0) {
    fprintf(stderr, "Cannot init sws context\n");
    goto failed;
  }

  return sws_ctx;

failed:
  sws_freeContext(sws_ctx);
  return NULL;
}

// must one plane format
int convert_frame_to_packet_format(struct SwsContext *sws_ctx, AVFrame *dst_frame, AVFrame *src_frame, uint8_t *data, int data_size, int pitch) {
  dst_frame->buf[0] = av_buffer_create(data, data_size, &free_data, NULL, 0);
  dst_frame->data[0] = data;
  dst_frame->linesize[0] = pitch;
  int res = sws_scale_frame(sws_ctx, src_frame, dst_frame);
  av_buffer_unref(&dst_frame->buf[0]);
  return res;
}

void sws_destroy(struct SwsContext *sws_ctx) {
  sws_freeContext(sws_ctx);
}
