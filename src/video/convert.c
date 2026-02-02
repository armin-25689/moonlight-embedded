#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#if defined(HAVE_LIBYUV)
#include <libyuv.h>
#else
#include <libswscale/swscale.h>
#endif

#include <Limelight.h>

#include "convert.h"
#include "ffmpeg.h"

#define MAX_DATA_BUFFER 4
#define DST_FRAMES_NUM 1

static int width, height;
static int planes = 0, last_dst_fmt = -1, multi = 1, last_src_color = -1;
struct _config_color {
  int isfull;
  int colorspace;
} static color_config = {0};

static int (*convert_function)(AVFrame * src_frame, uint8_t *dst_buffer[4], uint32_t pitch[4], int dst_fmt);

#if !defined(HAVE_LIBYUV)
static AVFrame **dst_frames = NULL;
static uint8_t *buffer_ptr[DST_FRAMES_NUM][MAX_DATA_BUFFER] = {0};
static struct SwsContext *sws_ctx = NULL;

static void sws_destroy() {
  if (sws_ctx != NULL)
    sws_freeContext(sws_ctx);
  sws_ctx = NULL;
}

static int sws_init(int src_fmt) {
  sws_ctx = sws_alloc_context();
  if (sws_ctx == NULL) {
    fprintf(stderr, "ffmpeg: Cannot alloc sws context\n");
    return -1;
  }

  if (sws_isSupportedOutput(src_fmt) <= 0) {
    fprintf(stderr, "ffmpeg: sws scale formati(from %s) is not supported!\n", av_get_pix_fmt_name(src_fmt));
    sws_destroy();
    return -1;
  }

  return 0;
}

static int sws_convert_frame(AVFrame *src_frame, AVFrame *dst_frame) {
  int res = sws_scale_frame(sws_ctx, dst_frame, src_frame);
  return res;
}

#else
static int (*yuv_convert) (AVFrame * src_frame, uint8_t *dst_buffer[MAX_DATA_BUFFER], uint32_t pitch[MAX_DATA_BUFFER]);
static int (*libyuv_410_to_ar30) (const uint16_t* src_y, int src_stride_y,
                                  const uint16_t* src_u, int src_stride_u,
                                  const uint16_t* src_v, int src_stride_v,
                                  uint8_t* dst_argb, int dst_stride_argb,
                                  int width, int height);
static int i410_to_ar30 (const uint16_t* src_y, int src_stride_y,
                         const uint16_t* src_u, int src_stride_u,
                         const uint16_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I410ToAR30Matrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvI601Constants, width, height);
}
static int j410_to_ar30 (const uint16_t* src_y, int src_stride_y,
                         const uint16_t* src_u, int src_stride_u,
                         const uint16_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I410ToAR30Matrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvJPEGConstants, width, height);
}
static int h410_to_ar30 (const uint16_t* src_y, int src_stride_y,
                         const uint16_t* src_u, int src_stride_u,
                         const uint16_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I410ToAR30Matrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvH709Constants, width, height);
}
static int f410_to_ar30 (const uint16_t* src_y, int src_stride_y,
                         const uint16_t* src_u, int src_stride_u,
                         const uint16_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I410ToAR30Matrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvF709Constants, width, height);
}
static int v410_to_ar30 (const uint16_t* src_y, int src_stride_y,
                         const uint16_t* src_u, int src_stride_u,
                         const uint16_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I410ToAR30Matrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvV2020Constants, width, height);
}
static int u410_to_ar30 (const uint16_t* src_y, int src_stride_y,
                         const uint16_t* src_u, int src_stride_u,
                         const uint16_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I410ToAR30Matrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuv2020Constants, width, height);
}
static int (*libyuv_010_to_ar30) (const uint16_t* src_y, int src_stride_y,
                                  const uint16_t* src_u, int src_stride_u,
                                  const uint16_t* src_v, int src_stride_v,
                                  uint8_t* dst_argb, int dst_stride_argb,
                                  int width, int height);
static int j010_to_ar30 (const uint16_t* src_y, int src_stride_y,
                         const uint16_t* src_u, int src_stride_u,
                         const uint16_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I010ToAR30Matrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvJPEGConstants, width, height);
}
static int f010_to_ar30 (const uint16_t* src_y, int src_stride_y,
                         const uint16_t* src_u, int src_stride_u,
                         const uint16_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I010ToAR30Matrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvF709Constants, width, height);
}
static int v010_to_ar30 (const uint16_t* src_y, int src_stride_y,
                         const uint16_t* src_u, int src_stride_u,
                         const uint16_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I010ToAR30Matrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvV2020Constants, width, height);
}
static int u010_to_ar30 (const uint16_t* src_y, int src_stride_y,
                         const uint16_t* src_u, int src_stride_u,
                         const uint16_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I010ToAR30Matrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuv2020Constants, width, height);
}
static int (*libyuv_444_to_argb) (const uint8_t* src_y, int src_stride_y,
                                  const uint8_t* src_u, int src_stride_u,
                                  const uint8_t* src_v, int src_stride_v,
                                  uint8_t* dst_argb, int dst_stride_argb,
                                  int width, int height);
static int f444_to_argb (const uint8_t* src_y, int src_stride_y,
                         const uint8_t* src_u, int src_stride_u,
                         const uint8_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I444ToARGBMatrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvF709Constants, width, height);
}
static int v444_to_argb (const uint8_t* src_y, int src_stride_y,
                         const uint8_t* src_u, int src_stride_u,
                         const uint8_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I444ToARGBMatrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvV2020Constants, width, height);
}
static int (*libyuv_420_to_argb) (const uint8_t* src_y, int src_stride_y,
                                  const uint8_t* src_u, int src_stride_u,
                                  const uint8_t* src_v, int src_stride_v,
                                  uint8_t* dst_argb, int dst_stride_argb,
                                  int width, int height);
static int f420_to_argb (const uint8_t* src_y, int src_stride_y,
                         const uint8_t* src_u, int src_stride_u,
                         const uint8_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I420ToARGBMatrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvF709Constants, width, height);
}                        
static int v420_to_argb (const uint8_t* src_y, int src_stride_y,
                         const uint8_t* src_u, int src_stride_u,
                         const uint8_t* src_v, int src_stride_v,
                         uint8_t* dst_argb, int dst_stride_argb,
                         int width, int height) {
  return I420ToARGBMatrix(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v,
                          dst_argb, dst_stride_argb, &kYuvV2020Constants, width, height);
}                        
static int yuv_444_to_argb (AVFrame * src_frame, uint8_t *dst_buffer[MAX_DATA_BUFFER], uint32_t pitch[MAX_DATA_BUFFER]) {
  return libyuv_444_to_argb(src_frame->data[0], src_frame->linesize[0],
                            src_frame->data[1], src_frame->linesize[1],
                            src_frame->data[2], src_frame->linesize[2],
                            dst_buffer[0], pitch[0],
                            src_frame->width, src_frame->height);
}
static int yuv_420_to_argb (AVFrame * src_frame, uint8_t *dst_buffer[MAX_DATA_BUFFER], uint32_t pitch[MAX_DATA_BUFFER]) {
  return libyuv_420_to_argb(src_frame->data[0], src_frame->linesize[0],
                            src_frame->data[1], src_frame->linesize[1],
                            src_frame->data[2], src_frame->linesize[2],
                            dst_buffer[0], pitch[0],
                            src_frame->width, src_frame->height);
}
static int yuv_010_to_ar30 (AVFrame * src_frame, uint8_t *dst_buffer[MAX_DATA_BUFFER], uint32_t pitch[MAX_DATA_BUFFER]) {
  return libyuv_010_to_ar30((uint16_t *)src_frame->data[0], src_frame->linesize[0] >> 1,
                            (uint16_t *)src_frame->data[1], src_frame->linesize[1] >> 1,
                            (uint16_t *)src_frame->data[2], src_frame->linesize[2] >> 1,
                            dst_buffer[0], pitch[0],
                            src_frame->width, src_frame->height);
}
static int yuv_410_to_ar30 (AVFrame * src_frame, uint8_t *dst_buffer[MAX_DATA_BUFFER], uint32_t pitch[MAX_DATA_BUFFER]) {
  return libyuv_410_to_ar30((uint16_t *)src_frame->data[0], src_frame->linesize[0] >> 1,
                            (uint16_t *)src_frame->data[1], src_frame->linesize[1] >> 1,
                            (uint16_t *)src_frame->data[2], src_frame->linesize[2] >> 1,
                            dst_buffer[0], pitch[0],
                            src_frame->width, src_frame->height);
}
static int yuv_010_to_p010 (AVFrame * src_frame, uint8_t *dst_buffer[MAX_DATA_BUFFER], uint32_t pitch[MAX_DATA_BUFFER]) {
  return I010ToP010((uint16_t *)src_frame->data[0], src_frame->linesize[0] >> 1,
                    (uint16_t *)src_frame->data[1], src_frame->linesize[1] >> 1,
                    (uint16_t *)src_frame->data[2], src_frame->linesize[2] >> 1,
                    (uint16_t *)dst_buffer[0], pitch[0] >> 1,
                    (uint16_t *)dst_buffer[1], pitch[1] >> 1,
                    src_frame->width, src_frame->height);
}
static int yuv_420_to_nv12 (AVFrame * src_frame, uint8_t *dst_buffer[MAX_DATA_BUFFER], uint32_t pitch[MAX_DATA_BUFFER]) {
  return I420ToNV12(src_frame->data[0], src_frame->linesize[0],
                    src_frame->data[1], src_frame->linesize[1],
                    src_frame->data[2], src_frame->linesize[2],
                    dst_buffer[0], pitch[0],
                    dst_buffer[1], pitch[1],
                    src_frame->width, src_frame->height);
}

static int yuv_choose_function (AVFrame *src_frame, int dst_fmt) {
  color_config.isfull = ffmpeg_is_frame_full_range(src_frame);
  color_config.colorspace = ffmpeg_get_frame_colorspace(src_frame);
  if (color_config.isfull == 0 && color_config.colorspace == COLORSPACE_REC_601) {
    libyuv_444_to_argb = &I444ToARGB;
    libyuv_420_to_argb = &I420ToARGB;
    libyuv_010_to_ar30 = &I010ToAR30;
    libyuv_410_to_ar30 = &i410_to_ar30;
  } else if (color_config.isfull == 1 && color_config.colorspace == COLORSPACE_REC_601) {
    libyuv_444_to_argb = &J444ToARGB;
    libyuv_420_to_argb = &J420ToARGB;
    libyuv_010_to_ar30 = &j010_to_ar30;
    libyuv_410_to_ar30 = &j410_to_ar30;
  } else if (color_config.isfull == 0 && color_config.colorspace == COLORSPACE_REC_709) {
    libyuv_444_to_argb = &H444ToARGB;
    libyuv_420_to_argb = &H420ToARGB;
    libyuv_010_to_ar30 = &H010ToAR30;
    libyuv_410_to_ar30 = &h410_to_ar30;
  } else if (color_config.isfull == 1 && color_config.colorspace == COLORSPACE_REC_709) {
    libyuv_444_to_argb = &f444_to_argb;
    libyuv_420_to_argb = &f420_to_argb;
    libyuv_010_to_ar30 = &f010_to_ar30;
    libyuv_410_to_ar30 = &f410_to_ar30;
  } else if (color_config.isfull == 1 && color_config.colorspace == COLORSPACE_REC_2020) {
    libyuv_444_to_argb = &v444_to_argb;
    libyuv_420_to_argb = &v420_to_argb;
    libyuv_010_to_ar30 = &v010_to_ar30;
    libyuv_410_to_ar30 = &v410_to_ar30;
  } else {
    libyuv_444_to_argb = &U444ToARGB;
    libyuv_420_to_argb = &U420ToARGB;
    libyuv_010_to_ar30 = &u010_to_ar30;
    libyuv_410_to_ar30 = &u410_to_ar30;
  }

  switch (src_frame->format) {
  case AV_PIX_FMT_YUV420P10:
    if (dst_fmt == AV_PIX_FMT_P010) {
      yuv_convert = &yuv_010_to_p010;
    } else {
      yuv_convert = &yuv_010_to_ar30;
    }
    break;
  case AV_PIX_FMT_YUV444P10:
      yuv_convert = &yuv_410_to_ar30;
    break;
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P:
    if (dst_fmt == AV_PIX_FMT_BGR0) {
      yuv_convert = &yuv_420_to_argb;
    } else {
      yuv_convert = &yuv_420_to_nv12;
    }
    break;
  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUVJ444P:
      yuv_convert = &yuv_444_to_argb;
    break;
  default:
    fprintf(stderr, "LIBYUV: Do not support this format: src_format-%d, dst_format-%d.\n", src_frame->format, dst_fmt);
    return -1;
  }

  return 0;
}
#endif

int convert_init(AVFrame *src_frame, int display_width, int display_height) {
  width = display_width;
  height = display_height;
  convert_function = NULL;
#if !defined(HAVE_LIBYUV)
  int src_fmt = src_frame->format;
  sws_init(src_fmt);
#endif

  return 0;
}

static int convert_frame_another(AVFrame * src_frame, uint8_t *dst_buffer[4], uint32_t pitch[4], int dst_fmt) {
#if !defined(HAVE_LIBYUV)
  if (dst_frames == NULL) {
    dst_frames = ffmpeg_alloc_frames(DST_FRAMES_NUM, dst_fmt, width, height, 0, true);
    if (dst_frames == NULL) {
      convert_destroy();
      return -1;
    }
  }

  int i = 0;
  //for (int i = 0; i < DST_FRAMES_NUM; i++) {
    for (int k = 0; k < MAX_DATA_BUFFER; k++) {
      buffer_ptr[i][k] = dst_frames[i]->data[k];
      dst_frames[i]->data[k] = dst_buffer[k];
    }
  //}

  if (sws_convert_frame(src_frame, dst_frames[i]) < 0) {
    fprintf(stderr, "sws convert failed.\n");
    return -1;
  }

  //for (int i = 0; i < DST_FRAMES_NUM; i++) {
    for (int k = 0; k < MAX_DATA_BUFFER; k++) {
      dst_frames[i]->data[k] = buffer_ptr[i][k];
    }
  //}
#else
  yuv_convert(src_frame, dst_buffer, pitch);
#endif

  return 0;
}

static int convert_frame_copy(AVFrame * src_frame, uint8_t *dst_buffer[4], uint32_t pitch[4], int dst_fmt) {
  for (int i = 0; i < planes; i++) {
    memcpy(dst_buffer[i], src_frame->data[i], pitch[i] * (i == 0 ? src_frame->height : ((int)(src_frame->height / multi))));
  }
  return 0;
}

int convert_frame(AVFrame * src_frame, uint8_t *dst_buffer[4], uint32_t pitch[4], int dst_fmt) {
  if (last_src_color != src_frame->colorspace) {
    planes = av_pix_fmt_count_planes(src_frame->format);
    last_dst_fmt = dst_fmt;
    last_src_color = src_frame->colorspace;
    switch (src_frame->format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV420P10:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_P010:
      multi = 2;
      break;
    default:
      multi = 1;
      break;
    }
    if (src_frame->format == dst_fmt)
      convert_function = &convert_frame_copy;
    else
      convert_function = &convert_frame_another;

    #if defined(HAVE_LIBYUV)
    if (yuv_choose_function(src_frame, dst_fmt) < 0)
      return -1;
    #endif
  }

  return convert_function(src_frame, dst_buffer, pitch, dst_fmt);
}

void convert_destroy() {
#if !defined(HAVE_LIBYUV)
  if (dst_frames != NULL) {
    ffmpeg_free_frames(dst_frames, DST_FRAMES_NUM);
    dst_frames = NULL;
  }
  sws_destroy();
#endif
}
