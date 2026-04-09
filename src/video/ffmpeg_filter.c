/*
 * This file is part of Moonlight Embedded.
 *
 * Based on Moonlight Pc implementation
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

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>

#include <stdlib.h>
#include <stdbool.h>

#include <Limelight.h>
#include "video_internal.h"
#include "video.h"

#define MAX_FILTER 10
#define MAX_FILTER_DESC_LEN 255

struct Ffmpeg_Filters_Args ffmpeg_filters_args = { .color.p3 = "smpte432", .color.bt2020 = "bt2020",
                                                   .color.bt709 = "bt709", .color.bt601 = "bt601",
                                                   .pix_fmt = -1 };
enum { FILTER_TONEMAP_VAAPI = 0, FILTER_SCALE_VAAPI };
static const char *filter_name_list[] = { "tonemap_vaapi", "scale_vaapi" };
// General decoder and renderer state
static AVFrame *filter_frame = NULL;
struct Filter_Property {
  AVFilterContext *src_ctx;
  AVFilterContext *sink_ctx;
  AVFilterGraph *graph;
};
static struct Filter_Property filter_graphs[2] = {0};
struct Filter_Property *hdr_filter_graph = &filter_graphs[0];
struct Filter_Property *sdr_filter_graph = &filter_graphs[1];
struct Filter_Desc {
  const char* name;
  char desc[MAX_FILTER_DESC_LEN];
  int index;
};
static bool use_hdr_fmt = false;
static uint16_t *hdr_metadata_ref = NULL;

static inline void destroy_filter_graphs () {
  if (hdr_filter_graph->graph) {
    avfilter_graph_free(&hdr_filter_graph->graph);
  }
  if (sdr_filter_graph->graph) {
    avfilter_graph_free(&sdr_filter_graph->graph);
  }
  memset(filter_graphs, 0, sizeof(filter_graphs));

  return;
}

static inline enum AVPixelFormat get_pix_fmt (AVFrame *frame) {
  enum AVPixelFormat pix_fmt = -1;
  if (frame == NULL)
    return pix_fmt;
  if (frame->hw_frames_ctx) {
    AVHWFramesContext *fr_ctx = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    if (fr_ctx) {
       pix_fmt = fr_ctx->sw_format;
    }
  }
  else {
    pix_fmt = frame->format;
  }
  return pix_fmt;
}

static inline const char* get_pix_fmt_name (AVFrame *frame) {
  enum AVPixelFormat pix_fmt = get_pix_fmt(frame);
  if (pix_fmt < 0)
    return NULL;
  else 
    return av_get_pix_fmt_name(pix_fmt);
}

static inline AVFilterContext* get_filter (const char* filtername, char* filterdesc, AVFilterGraph *graph, AVBufferRef *device_ctx) {
  if (filtername == NULL || filterdesc == NULL || graph == NULL) {
    return NULL;
  }
  const AVFilter *name = avfilter_get_by_name(filtername);
  if (name == NULL) {
    fprintf(stderr, "Create %s failed.\n", filtername);
    return NULL;
  }

  AVFilterContext *ctx =  avfilter_graph_alloc_filter(graph, name, filtername);
  if (ctx == NULL) {
    fprintf(stderr, "Create %s context failed.\n", filtername);
    return NULL;
  }
  if (device_ctx)
    ctx->hw_device_ctx = av_buffer_ref(device_ctx);

  int err = avfilter_init_str(ctx, filterdesc);
  if (err < 0) {
    fprintf(stderr, "Create filter %s failed: %d.\n", filtername, err);
    return NULL;
  }

  return ctx;
}

static inline struct Filter_Desc* generate_vaapi_desc (AVFrame *frame, struct Ffmpeg_Filters_Args *args, int *filter_count) {
  if (frame == NULL || hdr_metadata_ref == NULL || args == NULL || filter_count == NULL) {
    fprintf(stderr, "Invalied arguments.\n");
    return NULL;
  }
  char tonemap[MAX_FILTER_DESC_LEN] = {'\0'};
  int p3_gbrw[8] = { 13250, 34500, 7500, 3000, 34000, 16000, 15635, 16450 };
  int bt2020_gbrw[8] = { hdr_metadata_ref[2], hdr_metadata_ref[3], hdr_metadata_ref[4], hdr_metadata_ref[5], hdr_metadata_ref[0], hdr_metadata_ref[1], hdr_metadata_ref[6], hdr_metadata_ref[7] };

  int *gbrw = NULL;
  int minlight, maxlight, maxcll, maxfall;
  if ((args->action & FILTER_TONEMAP_COLOR_PRIMARIES) &&
      (args->color_primaries == args->color.p3)) {
    gbrw = p3_gbrw;
    // write modified hdr data to shared list
    hdr_metadata_ref[0] = p3_gbrw[4];
    hdr_metadata_ref[1] = p3_gbrw[5];
    hdr_metadata_ref[2] = p3_gbrw[0];
    hdr_metadata_ref[3] = p3_gbrw[1];
    hdr_metadata_ref[4] = p3_gbrw[2];
    hdr_metadata_ref[5] = p3_gbrw[3];
    hdr_metadata_ref[6] = p3_gbrw[6];
    hdr_metadata_ref[7] = p3_gbrw[7];
  }
  else {
    gbrw = bt2020_gbrw;
  }
  if ((args->action & FILTER_TONEMAP_LIGHT) &&
      (args->light.maxlight >= 100 &&
       args->light.middlelight >= 50)) {
    maxlight = args->light.maxlight;
    minlight = args->light.minlight;
    if (hdr_metadata_ref[10] == 0) {
      maxcll = 0;
      maxfall = 0;
    }
    else {
      maxcll = args->light.maxlight;
      maxfall = args->light.middlelight;
    }
    // write modified hdr data to shared list
    hdr_metadata_ref[8] = maxlight;
    hdr_metadata_ref[9] = minlight;
    hdr_metadata_ref[10] = maxcll;
    hdr_metadata_ref[11] = maxfall;
  }
  else {
    maxlight = hdr_metadata_ref[8];
    minlight = hdr_metadata_ref[9];
    maxcll = hdr_metadata_ref[10];
    maxfall = hdr_metadata_ref[11];
  }
    
  if (args->color_primaries == args->color.bt601 || args->color_primaries == args->color.bt709) {
    snprintf(tonemap, sizeof(tonemap),
             "format=%s:primaries=%s:transfer=%s:matrix=%s", 
             get_pix_fmt_name(frame), args->color_primaries, 
             args->color_primaries == args->color.bt601 ? "bt601" : "bt709",
             args->color_primaries == args->color.bt601 ? "bt601" : "bt709");
  } else {
    snprintf(tonemap, sizeof(tonemap),
             "format=%s:primaries=%s:transfer=%s:matrix=%s:display=%d %d|%d %d|%d %d|%d %d|%d %d", 
             get_pix_fmt_name(frame), args->color_primaries, "smpte2084",
             frame->colorspace == AVCOL_SPC_BT2020_NCL ? "bt2020nc" : "bt2020",
             gbrw[0], gbrw[1], gbrw[2], gbrw[3], gbrw[4], gbrw[5], gbrw[6], gbrw[7],
             minlight, maxlight * 10000);
    if (maxcll > 0 && maxfall > 0) {
      char lightargs[30] = {'\0'};
      int len = strlen(tonemap);
      snprintf(lightargs, sizeof(lightargs),
               ":light=%d %d", maxcll, maxfall);
      memcpy(tonemap + len, lightargs, strlen(lightargs) + 1);
    }
  }

  char scale[MAX_FILTER_DESC_LEN] = {'\0'};
  int width, height, format;
  if ((args->action & FILTER_SCALE_SIZE) &&
      (args->video_size.width > 0 && args->video_size.height > 0)) {
    width = args->video_size.width;
    height = args->video_size.height;
  }
  else {
    width = frame->width;
    height = frame->height;
  }
  if (args->action & FILTER_SCALE_FMT) {
    if (use_hdr_fmt) {
      format = AV_PIX_FMT_X2RGB10LE;
    }
    else {
      format = AV_PIX_FMT_BGRA;
    }
    args->pix_fmt = format;
  }
  else {
      format = AV_PIX_FMT_NONE;
  }
  snprintf(scale, sizeof(scale),
           "w=%d:h=%d:format=%s:mode=fast",
           width, height, format == AV_PIX_FMT_NONE ? get_pix_fmt_name(frame) : av_get_pix_fmt_name(format));

  struct Filter_Desc *filters_desc = calloc(MAX_FILTER, sizeof(struct Filter_Desc));
  if (filters_desc == NULL) {
    fprintf(stderr, "Alloc filters desc mem failed.\n");
    return NULL;
  }

  int count = 0;
  if (filter_frame->colorspace == AVCOL_SPC_BT2020_NCL || filter_frame->colorspace == AVCOL_SPC_BT2020_CL) {
    if ((args->action & FILTER_TONEMAP_COLOR_PRIMARIES) ||
        (args->action & FILTER_TONEMAP_LIGHT)) {
      if (hdr_metadata_ref[0] == 0) {
        fprintf(stderr, "hdr_metadata_ref is not fill correctly.\n");
        return NULL;
      }

      filters_desc[count].name = filter_name_list[FILTER_TONEMAP_VAAPI];
      int len = strlen(tonemap);
      memcpy(filters_desc[count].desc, tonemap, len + 1);
      count++;
    }
  }
  if ((args->action & FILTER_SCALE_FMT) ||
      (args->action & FILTER_SCALE_SIZE)) {
    filters_desc[count].name = filter_name_list[FILTER_SCALE_VAAPI];
    int len = strlen(scale);
    memcpy(filters_desc[count].desc, scale, len + 1);
    count++;
  }

  *filter_count = count;
  return filters_desc;
}

static inline struct Filter_Desc* generate_filters_desc (AVFrame *frame, struct Ffmpeg_Filters_Args *args, int *filter_count) {
  enum AVPixelFormat format = frame->format;
  switch (format) {
  case AV_PIX_FMT_VAAPI:
    return generate_vaapi_desc(frame, args, filter_count);
  default:
    fprintf(stderr, "Filter generator could not support this format: %d.\n", format);
    return NULL;
  }

  return NULL;
}

static inline int ffmpeg_create_filter_graph(AVFrame *frame, AVCodecContext *decoder_ctx, struct Filter_Property *filter_props) {
  int err = -1;

  if (filter_props->graph) return 0;

  if (frame->hw_frames_ctx) {
    if (decoder_ctx->hw_frames_ctx == NULL || decoder_ctx->hw_device_ctx == NULL) {
      fprintf(stderr, "Cannot get hw_frames_ctx from decoder context.\n");
      return err;
    }
  }
  else {
    fprintf(stderr, "Create filter need hw context.\n");
    return err;
  }

  struct Filter_Desc* fdesc = NULL;
  const AVFilter *src = avfilter_get_by_name("buffer");
  const AVFilter *sink = avfilter_get_by_name("buffersink");
  AVFilterGraph *graph = avfilter_graph_alloc();
  AVFilterContext *src_ctx = avfilter_graph_alloc_filter(graph, src, "in");
  AVFilterContext *sink_ctx = NULL;
  AVBufferSrcParameters *para = av_buffersrc_parameters_alloc();
  if (src_ctx == NULL || src == NULL || sink == NULL || graph == NULL) {
    fprintf(stderr, "Get src|src_ctx|sink|graph|ref|frame failed.\n");
    goto filter_clear;
  }

  if (para == NULL) {
    fprintf(stderr, "Alloc buffersrc parameters failed.\n");
    goto filter_clear;
  }
  AVRational time_base = { .num = 1, .den = 300 };
  para->width = frame->width;
  para->height = frame->height;
  para->format = frame->format;
  para->time_base = time_base;
  para->sample_aspect_ratio = frame->sample_aspect_ratio;
  para->color_space = frame->colorspace;
  para->color_range = frame->color_range;
  if (frame->hw_frames_ctx) {
    if (av_buffer_replace(&para->hw_frames_ctx, frame->hw_frames_ctx) < 0) {
      fprintf(stderr, "Replace buffersrc hw_frames_ctx failed.\n");
      goto filter_clear;
    }
  }
  if (av_buffersrc_parameters_set(src_ctx, para) < 0) {
    fprintf(stderr, "Set buffer source failed.\n");
    goto filter_clear;
  }

  err = avfilter_init_dict(src_ctx, NULL);
  if (err < 0) {
    fprintf(stderr, "Init buffersrc context failed: %d.\n", err);
    goto filter_clear;
  }

  avfilter_graph_create_filter(&sink_ctx, sink, "out", NULL, NULL, graph);
  if (src_ctx == NULL || sink_ctx == NULL) {
    fprintf(stderr, "Create src|sink context failed.\n");
    goto filter_clear;
  }

  AVFilterContext *last_ctx = src_ctx;
  int filter_count = 0;
  fdesc = generate_filters_desc(frame, &ffmpeg_filters_args, &filter_count);
  for (int i = 0; i < filter_count; i++) {
    AVFilterContext* fctx = get_filter(fdesc[i].name, fdesc[i].desc, graph, decoder_ctx->hw_device_ctx);
    if (fctx == NULL) {
      fprintf(stderr, "Create filter context failed: %s(%s).\n", fdesc[i].name, fdesc[i].desc);
      goto filter_clear;
    }
    err = avfilter_link(last_ctx, 0, fctx, 0);
    if (err < 0) {
      fprintf(stderr, "Link graph failed: %d.\n", err);
      goto filter_clear;
    }
    last_ctx = fctx;
    printf("Filters graph has linked %s(%s).\n", fdesc[i].name, fdesc[i].desc);
  }
  err = avfilter_link(last_ctx, 0, sink_ctx, 0);
  if (err < 0) {
    fprintf(stderr, "Link graph failed: %d.\n", err);
    goto filter_clear;
  }

  avfilter_graph_set_auto_convert(graph, AVFILTER_AUTO_CONVERT_NONE);
  err = avfilter_graph_config(graph, NULL);
  if (err < 0) {
    fprintf(stderr, "Config graph failed: %d .\n", err);
    fprintf(stderr, "Initailize filter failed.\n");
    goto filter_clear;
  }

  filter_props->graph = graph;
  filter_props->src_ctx = src_ctx;
  filter_props->sink_ctx = sink_ctx;

filter_clear:

  if (para)
    av_freep(&para);
  if (fdesc)
    free(fdesc);
  if (filter_props->graph == NULL) {
    avfilter_graph_free(&graph);
  }

  return err;
}

static inline int pass_frame_to_graph (AVFrame *inframe, AVFilterContext *src_ctx, AVFrame *outframe, AVFilterContext *sink_ctx) {
  av_frame_unref(outframe);
  int err;
  static int srcflags = AV_BUFFERSRC_FLAG_NO_CHECK_FORMAT | AV_BUFFERSRC_FLAG_PUSH | AV_BUFFERSRC_FLAG_KEEP_REF;
  err = av_buffersrc_add_frame_flags(src_ctx, inframe, srcflags);
  if (err < 0) {
    fprintf(stderr, "Add frame to buffersrc failed: %d.\n", err);
    return err;
  }
  static int sinkflags = AV_BUFFERSINK_FLAG_NO_REQUEST;
  err = av_buffersink_get_frame_flags(sink_ctx, outframe, sinkflags);
  if (err < 0) {
    if (inframe != NULL)
      fprintf(stderr, "Get frame from buffersink failed.\n");
  }
  av_frame_unref(inframe);
  return err;
}

static inline int ffmpeg_get_filte_frame(AVFrame *frame, AVCodecContext *decoder_ctx) {
  int err = -1;
  AVFilterContext *src_ctx, *sink_ctx;
  if (filter_frame->colorspace == AVCOL_SPC_BT2020_NCL || filter_frame->colorspace == AVCOL_SPC_BT2020_CL) {
    if (hdr_filter_graph->graph == NULL) {
      if (ffmpeg_create_filter_graph(filter_frame, decoder_ctx, hdr_filter_graph) < 0) {
        fprintf(stderr, "Create hdr filter graph failed.\n");
        return err;
      }
    }
    src_ctx = hdr_filter_graph->src_ctx;
    sink_ctx = hdr_filter_graph->sink_ctx;
  }
  else {
    if (sdr_filter_graph->graph == NULL) {
      if (ffmpeg_create_filter_graph(filter_frame, decoder_ctx, sdr_filter_graph) < 0) {
        fprintf(stderr, "Create sdr filter graph failed.\n");
        return err;
      }
    }
    src_ctx = sdr_filter_graph->src_ctx;
    sink_ctx = sdr_filter_graph->sink_ctx;
  }

  return pass_frame_to_graph(filter_frame, src_ctx, frame, sink_ctx);
}

void ffmpeg_filter_destroy () {
  destroy_filter_graphs();
  if (filter_frame)
    av_frame_free(&filter_frame);
  filter_frame = NULL;
  return;
}

int ffmpeg_init_filter(AVFrame *frame, AVCodecContext *decoder_ctx, bool usehdr, uint16_t *hdr_metadata, int (*decode_frame) (AVFrame *frame, bool native)) {
  use_hdr_fmt = usehdr;
  hdr_metadata_ref = hdr_metadata;
  if (decoder_ctx == NULL || frame == NULL || decode_frame == NULL || hdr_metadata == NULL) {
    fprintf(stderr, "Invalied arguments for ffmpeg_init_filter().\n");
    return -1;
  }
  ffmpeg_filter_destroy();
  if (filter_frame == NULL) {
    filter_frame = av_frame_alloc();
    if (filter_frame) {
      filter_frame->width = decoder_ctx->width;
      filter_frame->height = decoder_ctx->height;
      filter_frame->format = decoder_ctx->pix_fmt;
    }
  }
  if (filter_frame == NULL) {
    fprintf(stderr, "Alloc frame failed.\n");
    return -1;
  }
  int err = decode_frame(filter_frame, true);
  if (err == 0) {
    if (filter_frame->format < 0)
      return -1;
    return ffmpeg_get_filte_frame(frame, decoder_ctx);
  }
  return -1;
}

int ffmpeg_filte_frame(AVFrame *frame, AVCodecContext *decoder_ctx, int (*decode_frame) (AVFrame *frame, bool native)) {
  int err = decode_frame(filter_frame, true);
  if (err == 0) {
    return ffmpeg_get_filte_frame(frame, decoder_ctx);
  }
  return err;
}

int ffmpeg_modify_filter_action (int action) {
  if (hdr_filter_graph->graph || sdr_filter_graph->graph) {
    fprintf(stderr, "Filter Graph has created, could not change now.\n");
    return -1;
  }
  ffmpeg_filters_args.action |= action;
  return ffmpeg_filters_args.action;
}

int ffmpeg_reject_filter_action (int action) {
  if (hdr_filter_graph->graph || sdr_filter_graph->graph) {
    fprintf(stderr, "Filter Graph has created, could not change now.\n");
    return -1;
  }
  ffmpeg_filters_args.action &= ~action;
  return ffmpeg_filters_args.action;
}

void ffmpeg_filter_stop_filte () {
  AVFrame *frame = av_frame_alloc();
  if (hdr_filter_graph->graph) {
    pass_frame_to_graph (NULL, hdr_filter_graph->src_ctx, frame, hdr_filter_graph->sink_ctx);
    av_frame_unref(frame);
  }
  if (sdr_filter_graph->graph) {
    pass_frame_to_graph (NULL, sdr_filter_graph->src_ctx, frame, sdr_filter_graph->sink_ctx);
    av_frame_unref(frame);
  }
  av_frame_free(&frame);
  destroy_filter_graphs();
}
