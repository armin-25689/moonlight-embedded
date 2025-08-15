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
// lack conditions
#include "drm_base.h"
#include "drm_base_ffmpeg.h"

// General decoder and renderer state
static AVPacket* pkt;
static const AVCodec* decoder;
static AVCodecContext* decoder_ctx;
static AVFrame** dec_frames;

static int dec_frames_cnt;
static int current_frame, next_frame;

int supportedVideoFormat = 0;
bool supportedHDR = false;
bool wantHdr = false;
bool wantYuv444 = false;
bool isYUV444 = false;
bool useHdr = false;
enum decoders ffmpeg_decoder;

#define BYTES_PER_PIXEL 4

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
    printf("No HDR support.\n");
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

    if (videoFormat & VIDEO_FORMAT_MASK_YUV444)
      decoder_ctx->pix_fmt = AV_PIX_FMT_YUV444P;
    else
      decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    AVDictionary* dict = NULL;
    #ifdef HAVE_VAAPI
    if (ffmpeg_decoder == VAAPI) {
      vaapi_init(decoder_ctx);
    }
    #endif
    // need conditions111111111111111111
    if (render == DRM_RENDER) {
      ffmpeg_bind_drm_ctx(decoder_ctx, &dict);
    }

    AVDictionary* *dictPtr = NULL;
    if(dict)
      dictPtr = &dict;
    int err = avcodec_open2(decoder_ctx, decoder, dictPtr);
    if (err < 0) {
      printf("Couldn't open codec: %s\n", decoder->name);
      avcodec_free_context(&decoder_ctx);
      continue;
    }

    if (dictPtr != NULL)
      av_dict_free(&dict);

    break;
  }

  if (decoder == NULL) {
    printf("Couldn't find decoder\n");
    return -1;
  }

  printf("Using FFmpeg decoder: %s\n", decoder->name);
  if (ffmpeg_decoder == SOFTWARE && ((videoFormat & VIDEO_FORMAT_MASK_H264) == 0 || (videoFormat & VIDEO_FORMAT_MASK_YUV444)))
      
    printf("WARNING: Try use -bitrate N options to reduce bitrate for avoding lag!\n");

  dec_frames_cnt = buffer_count;
  dec_frames = malloc(buffer_count * sizeof(AVFrame*));
  if (dec_frames == NULL) {
    fprintf(stderr, "Couldn't allocate frames");
    return -1;
  }

  int widthMulti = -1;
  if (ffmpeg_decoder == SOFTWARE) {
    if (render == DRM_RENDER) {
// condition111111111111111111111111
      widthMulti = get_drm_dbum_aligned(-1, decoder_ctx->pix_fmt, width, height);
    }
    else {
      widthMulti = isYUV444 ? 64 : 128;
    }
  }

  for (int i = 0; i < buffer_count; i++) {
    dec_frames[i] = av_frame_alloc();
    if (dec_frames[i] == NULL) {
      fprintf(stderr, "Couldn't allocate frame");
      return -1;
    }
    if (widthMulti > 0) {
      if (width % widthMulti != 0) {
        dec_frames[i]->format = decoder_ctx->pix_fmt;
        dec_frames[i]->width = decoder_ctx->width;
        dec_frames[i]->height = decoder_ctx->height;
        // glteximage2d need 64 type least
        if (av_frame_get_buffer(dec_frames[i], widthMulti) < 0) {
          fprintf(stderr, "Couldn't allocate frame buffer");
          return -1;
        }
      }
    }
  }

  return 0;
}

// This function must be called after
// decoding is finished
void ffmpeg_destroy(void) {
  av_packet_free(&pkt);
  if (decoder_ctx) {
    avcodec_free_context(&decoder_ctx);
  }
  if (dec_frames) {
    for (int i = 0; i < dec_frames_cnt; i++) {
      if (dec_frames[i])
        av_frame_free(&dec_frames[i]);
    }
    free(dec_frames);
    dec_frames = NULL;
  }
  decoder_ctx = NULL;
  decoder = NULL;
}

AVFrame* ffmpeg_get_frame(bool native_frame) {
  int err = avcodec_receive_frame(decoder_ctx, dec_frames[next_frame]);

  if (err == 0) {
    av_packet_unref(pkt);
    current_frame = next_frame;
    next_frame = (current_frame+1) % dec_frames_cnt;

    if (ffmpeg_decoder == SOFTWARE || native_frame)
      return dec_frames[current_frame];
  } else if (err != AVERROR(EAGAIN)) {
    char errorstring[512];
    av_strerror(err, errorstring, sizeof(errorstring));
    fprintf(stderr, "Receive failed - %d/%s\n", err, errorstring);
  }
  return NULL;
}

// packets must be decoded in order
// indata must be inlen + AV_INPUT_BUFFER_PADDING_SIZE in length
static inline int ffmpeg_decode_packet(unsigned char* indata, int inlen, int flags) {
  int err;

  pkt->data = indata;
  pkt->size = inlen;
  pkt->flags = flags;

  err = avcodec_send_packet(decoder_ctx, pkt);
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
    //*pix_fmt = decoder_ctx->pix_fmt;
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
  format |= VIDEO_FORMAT_H264;
  format |= VIDEO_FORMAT_H264_HIGH8_444;
  format |= VIDEO_FORMAT_H265_REXT8_444;
  format |= VIDEO_FORMAT_H265_REXT10_444;
  format |= VIDEO_FORMAT_H265_MAIN10;
  format |= VIDEO_FORMAT_H265;
  format |= VIDEO_FORMAT_AV1_HIGH8_444;
  format |= VIDEO_FORMAT_AV1_HIGH10_444;
  format |= VIDEO_FORMAT_AV1_MAIN8;
  format |= VIDEO_FORMAT_AV1_MAIN10;
  return format;
}
