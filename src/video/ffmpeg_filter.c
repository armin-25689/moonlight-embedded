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
                                                   .color.bt709 = "bt709", .color.bt601 = "bt601" };
enum { FILTER_TONEMAP_VAAPI = 0, FILTER_SCALE_VAAPI, FILTER_VULKAN };
static const char *filter_name_list[] = { "tonemap_vaapi", "scale_vaapi", "libplacebo" };
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

/*
static inline const char* get_pix_fmt_name (AVFrame *frame) {
  enum AVPixelFormat pix_fmt = get_pix_fmt(frame);
  if (pix_fmt < 0)
    return NULL;
  else 
    return av_get_pix_fmt_name(pix_fmt);
}
*/

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

struct Color_Args {
  const char* color_primaries;
  uint16_t gbrw[8];
  uint16_t maxlight;
  uint16_t minlight;
  uint16_t maxcll;
  uint16_t maxfall;
  enum AVPixelFormat sdrfmt;
  enum AVPixelFormat hdrfmt;
  enum AVPixelFormat srcformat;
  int width;
  int height;
  bool ishdr;
  bool tosdr;
};

#define APPEND_DESC(dstdesc, fmt, ...) \
  do { \
    char iargs[MAX_FILTER_DESC_LEN] = {'\0'}; \
    snprintf(iargs, sizeof(iargs), fmt, ##__VA_ARGS__); \
    int dlen = strlen(dstdesc); \
    int alen = strlen(iargs); \
    if (dlen + alen + 1 < MAX_FILTER_DESC_LEN) { \
      if (dlen > 1) { \
        char *colon = ":"; \
        memcpy(dstdesc + dlen, colon, 2); \
        dlen++; \
      } \
      memcpy(dstdesc + dlen, iargs, strlen(iargs) + 1); \
    } \
  } while (0)

static inline int deal_filters_args (AVFrame *frame, struct Ffmpeg_Filters_Args *args, struct Color_Args *colors) {
  if (frame == NULL || hdr_metadata_ref == NULL || args == NULL || colors == NULL) {
    fprintf(stderr, "Invalied arguments.\n");
    return -1;
  }
  uint16_t p3_gbrw[8] = { 13250, 34500, 7500, 3000, 34000, 16000, 15635, 16450 };
  uint16_t bt2020_gbrw[8] = { hdr_metadata_ref[2], hdr_metadata_ref[3], hdr_metadata_ref[4], hdr_metadata_ref[5], hdr_metadata_ref[0], hdr_metadata_ref[1], hdr_metadata_ref[6], hdr_metadata_ref[7] };

  if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
    colors->ishdr = true;
  }
  colors->color_primaries = args->color_primaries == NULL ? args->color.bt2020 : args->color_primaries;
  if (colors->color_primaries == args->color.bt601 || colors->color_primaries == args->color.bt709 ||
      (colors->color_primaries == args->color.p3 && (args->action & FILTER_TONEMAP_LIGHT) == 0)) {
    if (args->action & FILTER_TONEMAP_COLOR_PRIMARIES)
      colors->tosdr = true;
  }
  if ((args->action & FILTER_TONEMAP_COLOR_PRIMARIES) &&
      (args->color_primaries == args->color.p3)) {
    memcpy(colors->gbrw, p3_gbrw, sizeof(p3_gbrw));
    // write modified hdr data to shared list
    if (colors->ishdr) {
      hdr_metadata_ref[0] = p3_gbrw[4];
      hdr_metadata_ref[1] = p3_gbrw[5];
      hdr_metadata_ref[2] = p3_gbrw[0];
      hdr_metadata_ref[3] = p3_gbrw[1];
      hdr_metadata_ref[4] = p3_gbrw[2];
      hdr_metadata_ref[5] = p3_gbrw[3];
      hdr_metadata_ref[6] = p3_gbrw[6];
      hdr_metadata_ref[7] = p3_gbrw[7];
    }
  }
  else {
    memcpy(colors->gbrw, bt2020_gbrw, sizeof(bt2020_gbrw));
  }

  if ((args->action & FILTER_TONEMAP_LIGHT) &&
      ((args->light.maxfall > 0 &&
        args->light.maxcll > 0) ||
       args->light.maxlight > 0)) {
    colors->maxlight = args->light.maxlight;
    colors->minlight = hdr_metadata_ref[9];
    if (hdr_metadata_ref[10] == 0) {
      colors->maxcll = 0;
      colors->maxfall = 0;
    }
    else {
      colors->maxcll = args->light.maxcll;
      colors->maxfall = args->light.maxfall;
    }
    // write modified hdr data to shared list
    if (hdr_metadata_ref[8] != 0) {
      if (colors->ishdr) {
        printf("Filters will tonemap light(maxcll:maxfall:maxluminance) from %d:%d:%d to %d:%d:%d.\n", hdr_metadata_ref[10], hdr_metadata_ref[11],
               hdr_metadata_ref[8], colors->maxcll, colors->maxfall, colors->maxlight);
        hdr_metadata_ref[8] = colors->maxlight;
        hdr_metadata_ref[9] = colors->minlight;
        hdr_metadata_ref[10] = colors->maxcll;
        hdr_metadata_ref[11] = colors->maxfall;
      }
    }
  }
  else {
    colors->maxlight = hdr_metadata_ref[8];
    colors->minlight = hdr_metadata_ref[9];
    colors->maxcll = hdr_metadata_ref[10];
    colors->maxfall = hdr_metadata_ref[11];
  }

  colors->hdrfmt = AV_PIX_FMT_X2RGB10LE;
  colors->sdrfmt = AV_PIX_FMT_BGRA;
  colors->srcformat = get_pix_fmt(frame);

  if ((args->action & FILTER_SCALE_SIZE) &&
      (args->video_size.width > 0 && args->video_size.height > 0)) {
    colors->width = args->video_size.width;
    colors->height = args->video_size.height;
  }
  else {
    colors->width = frame->width;
    colors->height = frame->height;
  }

  return 0;
}

static inline int generate_vaapi_desc (int action, struct Filter_Desc *filters_desc, int *filter_count, struct Color_Args *colors) {
  int count = 0;

  if (colors->ishdr &&
      ((action & FILTER_TONEMAP_LIGHT) ||
       (action & FILTER_TONEMAP_COLOR_PRIMARIES))) {
    if (hdr_metadata_ref[0] == 0) {
      fprintf(stderr, "hdr_metadata_ref is not fill correctly.\n");
      return -1;
    }

    if (colors->tosdr) {
      APPEND_DESC(filters_desc[count].desc,
                  "format=%s:primaries=%s:transfer=%s",
                  av_get_pix_fmt_name(colors->srcformat), colors->color_primaries, "iec61966-2-1");
    }
    else {
      if (action & FILTER_TONEMAP_FORCE_BT2020) {
        hdr_metadata_ref[0] = 35400;
        hdr_metadata_ref[1] = 14600;
        hdr_metadata_ref[2] = 8500;
        hdr_metadata_ref[3] = 39850;
        hdr_metadata_ref[4] = 6550;
        hdr_metadata_ref[5] = 2300;
        APPEND_DESC(filters_desc[count].desc,
                    "format=%s:primaries=%s:display=%d %d|%d %d|%d %d|%d %d|%d %d",
                    av_get_pix_fmt_name(colors->srcformat), "bt2020",
                    8500, 39850, 6550, 2300, 35400, 14600, 15635, 16450,
                    colors->minlight, colors->maxlight * 10000);
      }
      else {
        APPEND_DESC(filters_desc[count].desc,
                    "format=%s:primaries=%s:display=%d %d|%d %d|%d %d|%d %d|%d %d",
                    av_get_pix_fmt_name(colors->srcformat), colors->color_primaries,
                    colors->gbrw[0], colors->gbrw[1], colors->gbrw[2], colors->gbrw[3],
                    colors->gbrw[4], colors->gbrw[5], colors->gbrw[6], colors->gbrw[7],
                    colors->minlight, colors->maxlight * 10000);
      }
      if (colors->maxcll > 0 && colors->maxfall > 0) {
        APPEND_DESC(filters_desc[count].desc,
                    "light=%d %d",
                    colors->maxcll, colors->maxfall);
      }
    }

    filters_desc[count].name = filter_name_list[FILTER_TONEMAP_VAAPI];
    count++;
  }

  enum AVPixelFormat dstfmt = use_hdr_fmt ? colors->hdrfmt : colors->sdrfmt;
  if (action & FILTER_SCALE_FMT || colors->tosdr) {
    APPEND_DESC(filters_desc[count].desc,
                "format=%s:out_range=pc",
                av_get_pix_fmt_name(dstfmt));
  }
  if (action & FILTER_SCALE_SIZE) {
    APPEND_DESC(filters_desc[count].desc,
                "w=%d:h=%d:mode=fast",
                colors->width, colors->height);
  }
  if (strlen(filters_desc[count].desc) > 1) {
    filters_desc[count].name = filter_name_list[FILTER_SCALE_VAAPI];
    count++;
  }

  *filter_count = count;
  return 0;
}

static inline int generate_vulkan_desc (int action, struct Filter_Desc *filters_desc, int *filter_count, struct Color_Args *colors) {
  int count = 0;

  if (colors->ishdr &&
      (action & FILTER_TONEMAP_COLOR_PRIMARIES)) {
    if (hdr_metadata_ref[0] == 0) {
      fprintf(stderr, "hdr_metadata_ref is not fill correctly.\n");
      return -1;
    }

    if (colors->tosdr) {
      APPEND_DESC(filters_desc[count].desc,
                  "color_primaries=%s:color_trc=%s:tonemapping=%s:peak_detect=true:apply_filmgrain=false",
                  colors->color_primaries, "iec61966-2-1", "bt.2390");
    } else {
      if (action & FILTER_TONEMAP_FORCE_BT2020 && strcmp(colors->color_primaries, "bt2020") != 0) {
        APPEND_DESC(filters_desc[count].desc,
                    "format=%s:color_primaries=%s:color_trc=%s:range=%s:tonemapping=%s:peak_detect=true:apply_filmgrain=false",
                    "gbrpf32le", colors->color_primaries, "linear", "full", "bt.2390");
        filters_desc[count].name = filter_name_list[FILTER_VULKAN];
        count++;
      }
      APPEND_DESC(filters_desc[count].desc,
                  "color_primaries=%s:color_trc=%s:tonemapping=%s:peak_detect=%s:apply_filmgrain=false",
                  count == 0 ? colors->color_primaries : "bt2020", "smpte2084", "bt.2390", count == 0 ? "true" : "false");
    }
  }

  if ((action & FILTER_SCALE_FMT) || (action & FILTER_TONEMAP_COLOR_PRIMARIES)) {
    enum AVPixelFormat dstfmt = use_hdr_fmt ? colors->hdrfmt : colors->sdrfmt;
    APPEND_DESC(filters_desc[count].desc,
                "format=%s:range=%s",
                av_get_pix_fmt_name(dstfmt), "full");
  }

  if (action & FILTER_SCALE_SIZE) {
    APPEND_DESC(filters_desc[count].desc,
    "w=%d:h=%d:force_original_aspect_ratio=decrease",
    colors->width, colors->height);
  }

  if (strlen(filters_desc[count].desc) > 1) {
    filters_desc[count].name = filter_name_list[FILTER_VULKAN];
    count++;
  }
  else
    return -1;

  *filter_count = count;
  return 0;
}

static inline struct Filter_Desc* generate_filters_desc (AVFrame *frame, struct Ffmpeg_Filters_Args *args, int *filter_count) {
  enum AVPixelFormat format = frame->format;
  struct Color_Args colors = {0};
  if (deal_filters_args(frame, args, &colors) < 0) {
    fprintf(stderr, "Filter generator could not fill args.\n");
    return NULL;
  }
  if (filter_count == NULL) {
    fprintf(stderr, "Invalied arguments.\n");
    return NULL;
  }
  struct Filter_Desc *filters_desc = calloc(MAX_FILTER, sizeof(struct Filter_Desc));
  if (filters_desc == NULL) {
    fprintf(stderr, "Alloc filters desc mem failed.\n");
    return NULL;
  }

  switch (format) {
  case AV_PIX_FMT_VAAPI:
    if (generate_vaapi_desc(args->action, filters_desc, filter_count, &colors) == 0)
      return filters_desc;
    break;
  case AV_PIX_FMT_VULKAN:
    if (generate_vulkan_desc(args->action, filters_desc, filter_count, &colors) == 0)
      return filters_desc;
    break;
  default:
    fprintf(stderr, "Filter generator could not support this format: %d.\n", format);
    break;
  }

  free(filters_desc);
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
  if (frame->time_base.num != 0 && frame->time_base.den != 0) {
    time_base.num = frame->time_base.num;
    time_base.den = frame->time_base.den;
  }
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
  if (filter_frame->color_trc == AVCOL_TRC_SMPTE2084 || filter_frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
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
    filter_frame->color_trc = AVCOL_TRC_IEC61966_2_1;
  }

  return pass_frame_to_graph(filter_frame, src_ctx, frame, sink_ctx);
}

void ffmpeg_filter_destroy () {
  destroy_filter_graphs();
  if (filter_frame) {
    av_frame_unref(filter_frame);
    av_frame_free(&filter_frame);
  }
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
  if (action == 0)
    return (ffmpeg_filters_args.action & FILTER_FLAGS);
  if (hdr_filter_graph->graph || sdr_filter_graph->graph) {
    fprintf(stderr, "Filter Graph has created, could not change now.\n");
    return -1;
  }
  ffmpeg_filters_args.action |= action;
  return ffmpeg_filters_args.action;
}

int ffmpeg_reject_filter_action (int action) {
  if (ffmpeg_filters_args.action == 0)
    return 0;
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

void ffmpeg_filter_caculate_light (uint16_t *srcmaxlight, uint16_t *srccll, uint16_t *srcfall) {
  if (*srccll == 0 || *srcfall == 0) {
    *srccll = (uint16_t) *srcmaxlight * ffmpeg_filters_args.light.ratio_num.cll / LIGHT_CLL_DEN;
    *srcfall = (uint16_t) *srccll * ffmpeg_filters_args.light.ratio_num.fall / LIGHT_FALL_DEN;
  }
  ffmpeg_filters_args.light.maxlight = ffmpeg_filters_args.light.ratio_num.lumi == 0 ? *srcmaxlight : (uint16_t) *srcmaxlight * ffmpeg_filters_args.light.ratio_num.lumi / LIGHT_LUMI_DEN;
  ffmpeg_filters_args.light.maxcll = ffmpeg_filters_args.light.ratio_num.cll == 0 ? *srccll : (uint16_t) ffmpeg_filters_args.light.maxlight * ffmpeg_filters_args.light.ratio_num.cll / LIGHT_CLL_DEN;
  ffmpeg_filters_args.light.maxfall = ffmpeg_filters_args.light.ratio_num.fall == 0 ? *srcfall : (uint16_t) ffmpeg_filters_args.light.maxcll * ffmpeg_filters_args.light.ratio_num.fall / LIGHT_FALL_DEN;
}
