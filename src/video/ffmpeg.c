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
#include <libavutil/pixdesc.h>
#include <libavutil/mastering_display_metadata.h>

#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>

#include <Limelight.h>
#include "video_internal.h"
#include "ffmpeg.h"
#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif
#ifdef HAVE_FFMPEGFILTER
#include "ffmpeg_filter.h"
#endif

// General decoder and renderer state
static AVPacket* pkt;
static const AVCodec* decoder;
static AVCodecContext* decoder_ctx;
static AVFrame** dec_frames;
static int dec_frames_cnt;

int supportedVideoFormat = 0;
bool supportedHDR = false;
bool wantHdr = false;
bool wantYuv444 = false;
bool isYUV444 = false;
bool useHdr = false;
enum decoders ffmpeg_decoder;
uint16_t ffmpeg_hdr_metadata[12] = {0};

#define BYTES_PER_PIXEL 4

static int (*ffmpeg_get_frame_function) (AVFrame *frame, bool native_frame);

static inline int ffmpeg_attach_hdr10_metadata (AVFrame *frame) {
  if (av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) == NULL) {

    SS_HDR_METADATA data;
    if (LiGetHdrMetadata(&data)) {
      AVMasteringDisplayMetadata *mastering = av_mastering_display_metadata_create_side_data(frame);
      if (mastering == NULL) {
        fprintf(stderr, "Cannot get metadata ptr from frame.\n");
        return -1;
      }
      mastering->display_primaries[0][0] = av_make_q(data.displayPrimaries[0].x, 50000);
      mastering->display_primaries[0][1] = av_make_q(data.displayPrimaries[0].y, 50000);
      mastering->display_primaries[1][0] = av_make_q(data.displayPrimaries[1].x, 50000);
      mastering->display_primaries[1][1] = av_make_q(data.displayPrimaries[1].y, 50000);
      mastering->display_primaries[2][0] = av_make_q(data.displayPrimaries[2].x, 50000);
      mastering->display_primaries[2][1] = av_make_q(data.displayPrimaries[2].y, 50000);

      mastering->white_point[0] = av_make_q(data.whitePoint.x, 50000);
      mastering->white_point[1] = av_make_q(data.whitePoint.y, 50000);

      mastering->min_luminance = av_make_q(data.minDisplayLuminance, 10000);
      mastering->max_luminance = av_make_q(data.maxDisplayLuminance, 1);

      mastering->has_luminance = data.maxDisplayLuminance != 0 ? 1 : 0;
      mastering->has_primaries = data.displayPrimaries[0].x != 0 ? 1 : 0;

      if (data.maxContentLightLevel > 0 && data.maxFrameAverageLightLevel > 0) {
        AVContentLightMetadata *light = av_content_light_metadata_create_side_data(frame);
        if (light == NULL) {
          fprintf(stderr, "Cannot get light ptr from frame.\n");
          return -1;
        }
        light->MaxCLL = data.maxContentLightLevel;
        light->MaxFALL = data.maxFrameAverageLightLevel;
        if (ffmpeg_hdr_metadata[10] == 0) {
          ffmpeg_hdr_metadata[10] = light->MaxCLL;
          ffmpeg_hdr_metadata[11] = light->MaxFALL;
        }
/*
        light->MaxCLL = data.maxContentLightLevel > 0 ? data.maxContentLightLevel : data.maxDisplayLuminance;
        light->MaxFALL = data.maxFrameAverageLightLevel > 0 ? data.maxFrameAverageLightLevel : (int)(light->MaxCLL / 5);
        light->MaxFALL = light->MaxFALL < 100 ? 100 : light->MaxFALL;
*/
      }

      if (ffmpeg_hdr_metadata[0] == 0) {
        int index = 0;
        ffmpeg_hdr_metadata[index++] = data.displayPrimaries[0].x;
        ffmpeg_hdr_metadata[index++] = data.displayPrimaries[0].y;
        ffmpeg_hdr_metadata[index++] = data.displayPrimaries[1].x;
        ffmpeg_hdr_metadata[index++] = data.displayPrimaries[1].y;
        ffmpeg_hdr_metadata[index++] = data.displayPrimaries[2].x;
        ffmpeg_hdr_metadata[index++] = data.displayPrimaries[2].y;
        ffmpeg_hdr_metadata[index++] = data.whitePoint.x;
        ffmpeg_hdr_metadata[index++] = data.whitePoint.y;
        ffmpeg_hdr_metadata[index++] = data.maxDisplayLuminance;
        ffmpeg_hdr_metadata[index++] = data.minDisplayLuminance;
      }
    }
  }

  return 0;
}

static inline void ffmpeg_detach_hdr10_metadata (AVFrame *frame) {
  av_frame_remove_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
  av_frame_remove_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
  return;
}

static int ffmpeg_get_frame_from_decoder(AVFrame *frame, bool native_frame) {
  int err = avcodec_receive_frame(decoder_ctx, frame);
  if (err == 0) {
    if (frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL)
      ffmpeg_attach_hdr10_metadata(frame);
    else
      ffmpeg_detach_hdr10_metadata(frame);
    if (ffmpeg_decoder == SOFTWARE || native_frame)
      return 0;
  } else if (err == AVERROR(EAGAIN)) {
    return 1;
  }
  char errorstring[512];
  av_strerror(err, errorstring, sizeof(errorstring));
  fprintf(stderr, "Receive failed - %d/%s\n", err, errorstring);

  return -1;
}

static int ffmpeg_get_frame_from_filter(AVFrame *frame, bool native_frame) {
#ifdef HAVE_FFMPEGFILTER
  return ffmpeg_filte_frame(frame, decoder_ctx, &ffmpeg_get_frame_from_decoder);
#else
  return -1;
#endif
}

static int ffmpeg_get_frame_chooser (AVFrame *frame, bool native_frame) {
  static int times = 0;
  char *errdesc = NULL;

  times++;
  if (times == 1)
    goto chooser_exit;

  if (frame == NULL) {
    errdesc = "NULL frame";
    goto chooser_exit;
  }

#ifdef HAVE_FFMPEGFILTER
  if (ffmpeg_modify_filter_action(0) > 0 && ffmpeg_decoder != SOFTWARE) {
    int err = ffmpeg_init_filter(frame, decoder_ctx, useHdr, ffmpeg_hdr_metadata, &ffmpeg_get_frame_from_decoder);
    if (err >= 0) {
      ffmpeg_get_frame_function = &ffmpeg_get_frame_from_filter;
      return 0;
    }
    errdesc = "Inital ffmpeg filter and test frame failed";
  }
  else
#endif
  {
    int err = ffmpeg_get_frame_from_decoder(frame, native_frame);
    if (err == 0 && frame->format >= 0) {
      ffmpeg_get_frame_function = &ffmpeg_get_frame_from_decoder;
      return 0;
    }
    errdesc = "Decode frame failed much times";
  }

chooser_exit:
  if (times >= 4) {
    fprintf(stderr, "FFMPEG ERROR: %s.\n", errdesc);
    return -1;
  }

  return 1;
}

// This function must be called before
// any other decoding functions
int ffmpeg_init(int videoFormat, int width, int height, int perf_lvl, int buffer_count, int thread_count) {
  // Initialize the avcodec library and register codecs
  av_log_set_level(AV_LOG_QUIET);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,10,100)
  avcodec_register_all();
#endif

  pkt = av_packet_alloc();
  if (pkt == NULL) {
    printf("Couldn't allocate packet\n");
    return -1;
  }

  int render = perf_lvl & RENDER_MASK;
  ffmpeg_decoder = perf_lvl & VAAPI_ACCELERATION ? VAAPI : SOFTWARE;
  if (wantYuv444 && !(videoFormat & VIDEO_FORMAT_MASK_YUV444)) {
    if (supportedVideoFormat) {
      printf("WARENING: Could not start yuv444 stream because of server support, fallback to yuv420 format,please try '-codec hevc -yuv444' option\n");
    }
    else {
      printf("WARNING: Could not start yuv444 stream because of client support, fallback to yuv420 format\n");
    }
  }
  if (wantHdr && !(videoFormat & VIDEO_FORMAT_MASK_10BIT)) {
    printf("WARNING: No HDR support.\n");
  }

  if (videoFormat & VIDEO_FORMAT_MASK_YUV444) {
    isYUV444 = true;
  }
  if (videoFormat & VIDEO_FORMAT_MASK_10BIT) {
    useHdr = true;
  }

  for (int try = 0; try < 6; try++) {
    if (videoFormat & VIDEO_FORMAT_MASK_H265) {
      if (ffmpeg_decoder == SOFTWARE) {
        if (try == 0) decoder = avcodec_find_decoder_by_name("hevc_nvv4l2"); // Tegra
        if (try == 1) decoder = avcodec_find_decoder_by_name("hevc_nvmpi"); // Tegra
        if (try == 2) decoder = avcodec_find_decoder_by_name("hevc_omx"); // VisionFive
        if (try == 3) decoder = avcodec_find_decoder_by_name("hevc_v4l2m2m"); // Stateful V4L2
      }
      if (try == 4) decoder = avcodec_find_decoder_by_name("hevc"); // Software and hwaccel
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
      if (ffmpeg_decoder == SOFTWARE) {
        if (try == 0) decoder = avcodec_find_decoder_by_name("libdav1d");
      }
      if (try == 1) decoder = avcodec_find_decoder_by_name("av1"); // Hwaccel
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_H264) {
      if (ffmpeg_decoder == SOFTWARE) {
        if (try == 0) decoder = avcodec_find_decoder_by_name("h264_nvv4l2"); // Tegra
        if (try == 1) decoder = avcodec_find_decoder_by_name("h264_nvmpi"); // Tegra
        if (try == 2) decoder = avcodec_find_decoder_by_name("h264_omx"); // VisionFive
        if (try == 3) decoder = avcodec_find_decoder_by_name("h264_v4l2m2m"); // Stateful V4L2
      }
      if (try == 4) decoder = avcodec_find_decoder_by_name("h264"); // Software and hwaccel
    } else {
      printf("Video format not supported\n");
      return -1;
    }

    // Skip this decoder if it isn't compiled into FFmpeg
    if (!decoder) {
      continue;
    }

    decoder_ctx = avcodec_alloc_context3(decoder);
    if (decoder_ctx == NULL) {
      printf("Couldn't allocate context\n");
      return -1;
    }

    // Use low delay decoding
    decoder_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // Allow display of corrupt frames and frames missing references
    decoder_ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    decoder_ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;

    // Report decoding errors to allow us to request a key frame
    decoder_ctx->err_recognition = AV_EF_EXPLODE;

    if (perf_lvl & SLICE_THREADING) {
      decoder_ctx->thread_type = FF_THREAD_SLICE;
      decoder_ctx->thread_count = thread_count;
    } else {
      decoder_ctx->thread_count = 1;
    }

    decoder_ctx->width = width;
    decoder_ctx->height = height;

    if (isYUV444) {
      if (useHdr)
        decoder_ctx->pix_fmt = AV_PIX_FMT_YUV444P10;
      else
        decoder_ctx->pix_fmt = AV_PIX_FMT_YUV444P;
    }
    else {
      if (useHdr)
        decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P10;
      else
        decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    #ifdef HAVE_VAAPI
    if (ffmpeg_decoder == VAAPI) {
      vaapi_init(decoder_ctx);
    }
    #endif

    AVDictionary* *dictPtr = NULL;
    int err = avcodec_open2(decoder_ctx, decoder, dictPtr);
    if (err < 0) {
      printf("Couldn't open codec: %s\n", decoder->name);
      avcodec_free_context(&decoder_ctx);
      continue;
    }

    break;
  }

  if (decoder == NULL) {
    printf("Couldn't find decoder\n");
    return -1;
  }

  printf("Using FFmpeg decoder: %s\n", decoder->name);
  if (ffmpeg_decoder == SOFTWARE && ((videoFormat & VIDEO_FORMAT_MASK_H264) == 0 || (videoFormat & VIDEO_FORMAT_MASK_YUV444)))
      
    printf("WARNING: Try use -bitrate N options to reduce bitrate for avoding lag!\n");

  // glteximage2d need 64 type align to render.just for egl with cpu render
  int widthMulti = 1;
  if (ffmpeg_decoder == SOFTWARE && render == EGL_RENDER) {
    widthMulti = isYUV444 ? 64 : 128;
  }
  dec_frames_cnt = buffer_count;
  dec_frames = ffmpeg_alloc_frames(dec_frames_cnt, decoder_ctx->pix_fmt, width, height, widthMulti, (width % widthMulti != 0) ? true : false);
  if (dec_frames == NULL)
    return -1;

  ffmpeg_get_frame_function = &ffmpeg_get_frame_chooser;

  return 0;
}

void ffmpeg_free_frames(AVFrame **frames, int frame_count) {
  if (frames == NULL) return;
  for ( int i = 0; i < frame_count; i++) {
    if (frames[i]) {
      av_frame_unref(frames[i]);
      av_frame_free(&frames[i]);
      frames[i] = NULL;
    }
  }
  free(frames);
}

AVFrame **ffmpeg_alloc_frames(int dec_frames_cnt, enum AVPixelFormat pix_fmt, int width, int height, int align, bool need_alloc_buffer) {
  AVFrame **dec_frames = malloc(dec_frames_cnt * sizeof(AVFrame*));
  if (dec_frames == NULL) {
    fprintf(stderr, "Couldn't allocate frames");
    return NULL;
  }

  for (int i = 0; i < dec_frames_cnt; i++) {
    dec_frames[i] = av_frame_alloc();
    if (dec_frames[i] == NULL) {
      fprintf(stderr, "Couldn't allocate frame");
      ffmpeg_free_frames(dec_frames, i);
      return NULL;
    }
    if (need_alloc_buffer) {
      dec_frames[i]->format = pix_fmt;
      dec_frames[i]->width = width;
      dec_frames[i]->height = height;
      if (av_frame_get_buffer(dec_frames[i], align) < 0) {
        fprintf(stderr, "Couldn't allocate frame buffer");
        ffmpeg_free_frames(dec_frames, i + 1);
        return NULL;
      }
    }
  }

  return dec_frames;
}


void ffmpeg_stop_decoder () {
  AVFrame *frame = av_frame_alloc();
  avcodec_send_packet(decoder_ctx, NULL);
  while (avcodec_receive_frame(decoder_ctx, frame) != AVERROR_EOF);
  av_frame_free(&frame);
#ifdef HAVE_FFMPEGFILTER
  ffmpeg_filter_stop_filte();
#endif
  return;
}

// This function must be called after
// decoding is finished
void ffmpeg_destroy(void) {
  ffmpeg_stop_decoder();
  if (decoder_ctx) {
    avcodec_free_context(&decoder_ctx);
  }
#ifdef HAVE_FFMPEGFILTER
  ffmpeg_filter_destroy();
#endif
  av_packet_free(&pkt);
  if (dec_frames) {
    ffmpeg_free_frames(dec_frames, dec_frames_cnt);
    dec_frames = NULL;
  }
#ifdef HAVE_VAAPI
  vaapi_destroy();
#endif
  decoder_ctx = NULL;
  decoder = NULL;
}

int ffmpeg_get_frame(AVFrame *frame, bool native_frame) {
  return ffmpeg_get_frame_function(frame, native_frame);
}

// packets must be decoded in order
// indata must be inlen + AV_INPUT_BUFFER_PADDING_SIZE in length
static inline int ffmpeg_decode_packet(unsigned char* indata, int inlen, int flags) {
  int err;

  pkt->data = indata;
  pkt->size = inlen;
  pkt->flags = flags;

  err = avcodec_send_packet(decoder_ctx, pkt);
  av_packet_unref(pkt);
  if (err < 0) {
    char errorstring[512];
    av_strerror(err, errorstring, sizeof(errorstring));
    fprintf(stderr, "Decode failed - %s\n", errorstring);
  }

  return err < 0 ? err : 0;
}

int ffmpeg_decode(unsigned char* indata, int inlen) {
  return ffmpeg_decode_packet(indata, inlen, 0);
}

int ffmpeg_decode2(unsigned char* indata, int inlen, int flags) {
  return ffmpeg_decode_packet(indata, inlen, flags);
}

int ffmpeg_is_frame_full_range(const AVFrame* frame) {
  return frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
}

int ffmpeg_get_frame_colorspace(const AVFrame* frame) {
  switch (frame->colorspace) {
  case AVCOL_SPC_SMPTE170M:
  case AVCOL_SPC_BT470BG:
    return COLORSPACE_REC_601;
  case AVCOL_SPC_BT709:
    return COLORSPACE_REC_709;
  case AVCOL_SPC_BT2020_NCL:
  case AVCOL_SPC_BT2020_CL:
    return COLORSPACE_REC_2020;
  default:
    return COLORSPACE_REC_601;
  }
}

void ffmpeg_get_plane_info (const AVFrame *frame, enum AVPixelFormat *pix_fmt, int *plane_num, enum PixelFormatOrder *plane_order) {
  if (frame->hw_frames_ctx) {
    AVHWFramesContext *fr_ctx = (AVHWFramesContext *)frame->hw_frames_ctx->data;
    if (fr_ctx) {
       *pix_fmt = fr_ctx->sw_format;
    }
  }
  else {
    *pix_fmt = frame->format;
  }
  printf("Try to use pixel format: %s \n", av_get_pix_fmt_name(*pix_fmt));

  int planes = av_pix_fmt_count_planes(*pix_fmt);
  *plane_num = planes <= 0 ? 4 : planes;
  switch (*pix_fmt) {
  case AV_PIX_FMT_VUYX:
    *plane_order = VUYX_ORDER;
    break;
  case AV_PIX_FMT_XV30:
  case AV_PIX_FMT_XV36:
    *plane_order = XVYU_ORDER;
    break;
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUV444P:
  case AV_PIX_FMT_YUVJ420P:
  case AV_PIX_FMT_YUVJ444P:
  case AV_PIX_FMT_YUV444P10:
    *plane_order = YUVX_ORDER;
    break;
  case AV_PIX_FMT_NV12:
  case AV_PIX_FMT_NV16:
  case AV_PIX_FMT_NV24:
  case AV_PIX_FMT_P010:
  case AV_PIX_FMT_P016:
    *plane_order = YUVX_ORDER;
    break;
  default:
    *plane_order = YUVX_ORDER;
    break;
  }
  return;
}

int software_supported_video_format() {
  int format = 0;
  format |= VIDEO_FORMAT_MASK_H264;
  format |= VIDEO_FORMAT_MASK_H265;
  format |= VIDEO_FORMAT_MASK_AV1;
  format |= VIDEO_FORMAT_MASK_10BIT;
  format |= VIDEO_FORMAT_MASK_YUV444;
  return format;
}

AVFrame ** ffmpeg_get_frames() {
  return dec_frames;
}

int ffmpeg_need_filter(int action) {
#ifdef HAVE_FFMPEGFILTER
  return ffmpeg_modify_filter_action(action);
#else
  return 0;
#endif
}

int ffmpeg_remove_filter(int action) {
#ifdef HAVE_FFMPEGFILTER
  return ffmpeg_reject_filter_action(action);
#else
  return 0;
#endif
}

bool ffmpeg_has_hdr_metadata(AVFrame *frame) {
  return av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) == NULL ? false : true;
}
