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

#include <libavcodec/avcodec.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "egl.h"
#include "ffmpeg.h"
#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif
#include "display.h"
#include "video.h"
#include "render.h"

#ifdef HAVE_X11
#include "../input/x11.h"
#endif

#include "../input/evdev.h"
#include "../loop.h"
#include "../util.h"

#define X11_VDPAU_ACCELERATION ENABLE_HARDWARE_ACCELERATION_1
#define X11_VAAPI_ACCELERATION ENABLE_HARDWARE_ACCELERATION_2
#define SLICES_PER_FRAME 4
#define WAYLAND_WINDOW 0x20
#define X11_WINDOW 0x40
#define GBM_WINDOW 0x80

static bool isTenBit;
static bool firstDraw = true;
static bool done = false;

static void* ffmpeg_buffer = NULL;
static size_t ffmpeg_buffer_size = 0;
static AVFrame *frames[MAX_FB_NUM] = {0};

static void *display = NULL;
static void *window = NULL;

static int current_frame[3] = {0}, next_frame[3] = {0};
static int pipefd[2];
static int windowpipefd[2];

static int display_width = 0, display_height = 0;
static int frame_width, frame_height, screen_width, screen_height;
static const char *quitRequest = QUITCODE;

static struct DISPLAY_CALLBACK *disPtr = NULL;
static struct DISPLAY_CALLBACK *displayCallbacksPtr[] = {
#ifdef HAVE_WAYLAND
                                                     &display_callback_wayland,
#endif
#ifdef HAVE_X11
                                                     &display_callback_x11,
#endif
#ifdef HAVE_GBM
                                                     &display_callback_gbm,
#endif
};
static struct RENDER_CALLBACK *renderPtr = NULL;
static struct RENDER_CALLBACK *renderCallbacksPtr[] = {
  &egl_render,
#ifdef HAVE_X11
  &x11_render,
#endif
};

struct Multi_Thread {
  bool created;
  pthread_t decoder_id;
  pthread_t render_id;
  pthread_cond_t cond;
  pthread_mutex_t mutex;
  void* (*frame_handler)(void *data);
  void* (*decoder_handler)(void *data);
  sem_t render_sem;
};
static struct Multi_Thread threads = {0};

typedef struct Setupargs {
  int videoFormat;
  int width;
  int height;
  int avc_flags;
  int buffer_count;
  int thread_count;
  int drFlags;
}SetupArgs;
static SetupArgs ffmpegArgs;

static void clear_threads() {
  if (threads.created) {
    pthread_cancel(threads.decoder_id);
    pthread_cancel(threads.render_id);
    done = true;
    memset(frames, 0, sizeof(frames));
    LiWakeWaitForVideoFrame();
    pthread_cond_signal(&threads.cond);
    if (threads.render_id)
      pthread_join(threads.render_id, NULL);
    if (threads.decoder_id)
      pthread_join(threads.decoder_id, NULL);
    pthread_cond_destroy(&threads.cond);
    pthread_mutex_destroy(&threads.mutex);
    sem_destroy(&threads.render_sem);
    threads.created = false;
  }
}

static int window_op_handle (int pipefd, void *data) {
  char *opCode = NULL;

  while (read(pipefd, &opCode, sizeof(char *)) > 0);
  if (strcmp(opCode, QUITCODE) == 0) {
    return LOOP_RETURN;
#ifdef HAVE_WAYLAND
  } else if (strcmp(opCode, GRABCODE) == 0) {
    disPtr->display_change_cursor("hide");
  } else if (strcmp(opCode, UNGRABCODE) == 0) {
    disPtr->display_change_cursor("display");
#endif
  }

  return LOOP_OK;
}

static inline void draw_frame (AVFrame* frame, int *res) {

  int imageNum = 0;
  if (renderPtr->decoder_type == VAAPI && renderPtr->render_type == EGL_RENDER) {
    imageNum = vaapi_export_egl_images(frame, renderPtr->data, renderPtr->extension_support, renderPtr->images[current_frame[1]].images.image_data, &renderPtr->images[current_frame[1]].images.descriptor);
    if (imageNum < 1) {
      *res = LOOP_RETURN;
      return;
    }
  }
  else {
    renderPtr->images[current_frame[1]].frame_data = frame->data;
  }

  if (firstDraw) {
     firstDraw = false;
     if (isYUV444 && (!(frame->linesize[0] == frame->linesize[2] && frame->linesize[1] == frame->linesize[0]))) {
       fprintf(stderr, "There is not yuv444 format. Please try remove -yuv444 option to draw video!\n");
       *res = LOOP_RETURN;
       return;
    }

    struct Render_Config config = {0};
    config.color_space = ffmpeg_get_frame_colorspace(frame);
    config.full_color_range = ffmpeg_is_frame_full_range(frame);
    ffmpeg_get_plane_info(frame, &config.pix_fmt, &config.plane_nums, &config.yuv_order);
    config.image_nums = imageNum;
    if (strcmp(disPtr->name, "gbm") ==0)
      config.vsync = true;
    if (renderPtr->render_sync_config != NULL) {
      renderPtr->render_sync_config(&config);
    }
  }

  current_frame[2] = renderPtr->render_draw(renderPtr->images[current_frame[1]]);
  if (current_frame[2] < 0) {
    *res = LOOP_RETURN;
    current_frame[2] = 0;
    return;
  }

  if (renderPtr->decoder_type == VAAPI && renderPtr->render_type == EGL_RENDER) {
    vaapi_free_egl_images(renderPtr->data, renderPtr->images[current_frame[1]].images.image_data, renderPtr->images[current_frame[1]].images.descriptor);
  }

  *res = LOOP_OK;
}

static int frame_handle (int pipefd, void *data) {
  AVFrame* frame = NULL;
  while (read(pipefd, &frame, sizeof(void*)) > 0);
  if (frame) {
    int res;
    draw_frame(frame, &res);
    if (res != LOOP_RETURN) {
      disPtr->display_put_to_screen(display_width, display_height, 0);
    }
    return res;
  }

  if (renderPtr->decoder_type == VAAPI && renderPtr->render_type == EGL_RENDER) {
    for (int i = 0; i < MAX_FB_NUM; i++) {
      vaapi_free_egl_images(renderPtr->data, renderPtr->images[i].images.image_data, renderPtr->images[i].images.descriptor);
    }
  }

  return LOOP_OK;
}

static void* frame_handler (void *data) {

  while (!done) {
    current_frame[1] = next_frame[1];
    next_frame[1] = (current_frame[1] + 1) % MAX_FB_NUM;
    sem_wait(&threads.render_sem);
    int frameNums = 0;
    sem_getvalue(&threads.render_sem, &frameNums);
    if (frameNums > 0) {
      sem_trywait(&threads.render_sem);
      pthread_mutex_lock(&threads.mutex);
      frames[current_frame[1]] = NULL;
      current_frame[1] = next_frame[1];
      next_frame[1] = (current_frame[1] + 1) % MAX_FB_NUM;
      pthread_cond_signal(&threads.cond);
      pthread_mutex_unlock(&threads.mutex);
    }
    if (frames[current_frame[1]]) {
      int res;
      draw_frame(frames[current_frame[1]], &res);
      if (res == LOOP_RETURN) {
        done = true;
        continue;
      }
      disPtr->display_put_to_screen(display_width, display_height, current_frame[2]);
      pthread_mutex_lock(&threads.mutex);
      frames[current_frame[1]] = NULL;
      pthread_cond_signal(&threads.cond);
      pthread_mutex_unlock(&threads.mutex);
    }
  }

  write(windowpipefd[1], &quitRequest, sizeof(char *));
  if (renderPtr->decoder_type == VAAPI && renderPtr->render_type == EGL_RENDER) {
    for (int i = 0; i < MAX_FB_NUM; i++) {
      vaapi_free_egl_images(renderPtr->data, renderPtr->images[i].images.image_data, renderPtr->images[i].images.descriptor);
    }
  }
  return NULL;
}

// declare funtion here
int x11_submit_decode_unit(PDECODE_UNIT decodeUnit);
static void* decoder_thread(void *data) {

  while (!done) {
    VIDEO_FRAME_HANDLE handle;
    PDECODE_UNIT du;
    if (!LiWaitForNextVideoFrame(&handle, &du)) {
      continue;
    }

    // blocking in x11_submit_decode_unit();
    int status = x11_submit_decode_unit(du);
    LiCompleteVideoFrame(handle, status);
    if (status == DR_OK) {
      sem_post(&threads.render_sem);
    }

    if (LiGetPendingVideoFrames() > 13) {
      fprintf(stderr, "WARNING: Use -bitrate N options to reduce bitrate for avoding lag!\n");
    }
  }

  sem_post(&threads.render_sem);
  return NULL;
}

int x11_init(bool vdpau, bool vaapi) {
  int res = 0;
  const char *displayDevice;

  int disIndex = 0;
  struct DISPLAY_CALLBACK *bestDisplay[3] = {0};
  struct RENDER_CALLBACK *bestRender[3] = {0};
  for (int i = 0; i < (sizeof(displayCallbacksPtr) / sizeof(displayCallbacksPtr[0])); i++) {
    disPtr = displayCallbacksPtr[i];
    display = disPtr->display_get_display(&displayDevice);
    if (!display)
      continue;

    struct Render_Init_Info renderParas = {0};
    renderParas.display = display;
    renderParas.egl_platform = disPtr->egl_platform;
    renderParas.format = disPtr->format;

    for (int j = 0; j < (sizeof(renderCallbacksPtr) / sizeof(renderCallbacksPtr[0])); j++) {
      // init render,such as egl/drm
      renderPtr = renderCallbacksPtr[j];
      if ((disPtr->renders & renderPtr->render_type) == 0) {
        renderPtr = NULL;
        continue;
      }
      if (renderPtr->render_create(&renderParas) < 0) {
        renderPtr->render_destroy();
        renderPtr = NULL;
        continue;
      }

      if (disIndex < 3) {
        bestDisplay[disIndex] = disPtr;
        bestRender[disIndex] = renderPtr;
        disIndex++;
      }

      if (vaapi) {
        bool directRenderSupport = false;
        if (renderPtr->is_hardaccel_support) {
    #ifdef HAVE_VAAPI
          int drm_fd = -1;
          void *dis;
          if (strcmp(renderPtr->name, "x11") != 0) {
            char drmNode[64] = {'\0'};
            drm_fd = get_drm_render_fd(drmNode);
            dis = (void *)&drm_fd;
          }
          else {
            dis = display;
          }
          if (!vaapi_validate_test(disPtr->name, renderPtr->name, dis, &directRenderSupport)) {
            renderPtr->is_hardaccel_support = false;
          }
          if (strcmp(renderPtr->name, "x11") != 0) {
            close(drm_fd);
          }
          else if (directRenderSupport) {
            renderPtr->is_hardaccel_support = true;
          }
    #endif
        }

        if (!renderPtr->is_hardaccel_support) {
          renderPtr->render_destroy();
          renderPtr = NULL;
          continue;
        }
      }
      break;
    }

    if (renderPtr != NULL)
      break;

    disPtr->display_close_display();
    disPtr = NULL;
  }

  if (disPtr == NULL || renderPtr == NULL) {
    disPtr = bestDisplay[0];
    renderPtr = bestRender[0];
    display = disPtr->display_get_display(&displayDevice);
    struct Render_Init_Info renderParas = {0};
    renderParas.display = display;
    renderParas.egl_platform = disPtr->egl_platform;
    renderParas.format = disPtr->format;
    renderPtr->render_create(&renderParas);
  }

  #ifdef HAVE_VAAPI
  if (renderPtr->is_hardaccel_support && vaapi) {
    if (vaapi_init_lib(displayDevice) != -1) {
      supportedVideoFormat = vaapi_supported_video_format();
      res = INIT_VAAPI;
    }
  }
  #endif

  // yuv444 is always supported by software decoder
  supportedVideoFormat = software_supported_video_format();

  res = res != 0 ? res : INIT_EGL;

  return res;
}

int x11_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  ffmpegArgs.drFlags = drFlags;

  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);

  if (disPtr->display_setup(width, height, drFlags) == -1)
    return -1;
  disPtr->display_get_resolution(&screen_width, &screen_height);
  window = disPtr->display_get_window();
  printf("Based %s window\n", disPtr->name);

  if (drFlags & DISPLAY_FULLSCREEN) {
    display_width = screen_width;
    display_height = screen_height;
  } else {
    display_width = width;
    display_height = height;
  }
  frame_width = width;
  frame_height = height;

  int avc_flags;
  if (drFlags & X11_VDPAU_ACCELERATION) {
    avc_flags = VDPAU_ACCELERATION;
  }
  else if (drFlags & X11_VAAPI_ACCELERATION) {
    avc_flags = VAAPI_ACCELERATION;
  }
  else {
    avc_flags = SLICE_THREADING;
  }

  if (ffmpeg_init(videoFormat, frame_width, frame_height, avc_flags, MAX_FB_NUM, SLICES_PER_FRAME) < 0) {
    fprintf(stderr, "Couldn't initialize video decoding\n");
    return -1;
  }
  ffmpegArgs.videoFormat = videoFormat;
  ffmpegArgs.width = frame_width;
  ffmpegArgs.height = frame_height;
  ffmpegArgs.avc_flags = avc_flags;
  ffmpegArgs.buffer_count = MAX_FB_NUM;
  ffmpegArgs.thread_count = SLICES_PER_FRAME;

  isTenBit = videoFormat & VIDEO_FORMAT_MASK_10BIT;

  struct Render_Init_Info renderParas = {0};
  renderParas.window = window;
  renderParas.frame_width = frame_width;
  renderParas.frame_height = frame_height;
  renderParas.screen_width = screen_width;
  renderParas.screen_height = screen_height;
  renderParas.is_full_screen = drFlags & DISPLAY_FULLSCREEN;
  renderParas.is_yuv444 = isYUV444;
  renderPtr->decoder_type = ffmpeg_decoder;
  if (renderPtr->display_name != NULL) {
    renderPtr->display_name = disPtr->name;
  }
  if (strcmp(disPtr->name, "gbm") ==0)
    renderParas.use_display_buffer = true;
  if (renderPtr->render_init != NULL) {
    if (renderPtr->render_init(&renderParas) < 0) {
      return -1;
    }
  }

  if (pipe(windowpipefd) == -1) {
    fprintf(stderr, "Can't create communication channel between threads\n");
    return -2;
  }
  loop_add_fd(windowpipefd[0], &window_op_handle, EPOLLIN);
  fcntl(windowpipefd[0], F_SETFL, O_NONBLOCK);

  if (!(CAPABILITY_DIRECT_SUBMIT & decoder_callbacks_x11.capabilities) ||
      !(CAPABILITY_DIRECT_SUBMIT & decoder_callbacks_x11_vaapi.capabilities)) {
    threads.created = true;
    threads.frame_handler = frame_handler;
    threads.decoder_handler = decoder_thread;
    pthread_mutex_init(&threads.mutex, NULL);
    pthread_cond_init(&threads.cond, NULL);
    sem_init(&threads.render_sem, 0, 0);
    if (pthread_create(&threads.render_id, NULL, threads.frame_handler, &pipefd[0]) != 0 ||
        pthread_create(&threads.decoder_id, NULL, threads.decoder_handler, &pipefd[0]) != 0) {
      clear_threads();
      fprintf(stderr, "Error: Cannot create decoder/render/dislpay thread! Please try again or try direct submit mode.\n");
      return -1;
    }
  }

  if (!threads.created) {
    if (pipe(pipefd) == -1) {
      fprintf(stderr, "Can't create communication channel between threads\n");
      return -2;
    }
    loop_add_fd(pipefd[0], &frame_handle, EPOLLIN);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
  }

  evdev_trans_op_fd(windowpipefd[1]);

  disPtr->display_setup_post((void *)&windowpipefd[1]);

  firstDraw = true;

  return 0;
}

int x11_setup_vdpau(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  return x11_setup(videoFormat, width, height, redrawRate, context, drFlags | X11_VDPAU_ACCELERATION);
}

int x11_setup_vaapi(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  return x11_setup(videoFormat, width, height, redrawRate, context, drFlags | X11_VAAPI_ACCELERATION);
}

void x11_cleanup() {
  clear_threads();
  if (windowpipefd[1] > 0) {
    loop_remove_fd(windowpipefd[0]);
    close(windowpipefd[1]);
    close(windowpipefd[0]);
  }
  if (pipefd[1] > 0) {
    loop_remove_fd(pipefd[0]);
    close(pipefd[1]);
    close(pipefd[0]);
  }
  renderPtr->render_destroy();
  ffmpeg_destroy();
  disPtr->display_close_display();
  disPtr = NULL;
  renderPtr = NULL;
}

int x11_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  PLENTRY entry = decodeUnit->bufferList;
  int length = 0;

  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);

  while (entry != NULL) {
    memcpy(ffmpeg_buffer+length, entry->data, entry->length);
    length += entry->length;
    entry = entry->next;
  }

  int err = ffmpeg_decode2(ffmpeg_buffer, length, decodeUnit->frameType == FRAME_TYPE_IDR ? AV_PKT_FLAG_KEY : 0);

  if (err < 0) {
    write(windowpipefd[1], &quitRequest, sizeof(char *));
    return DR_NEED_IDR;
  }

  AVFrame* frame = ffmpeg_get_frame(true);

  if (frame != NULL) {
    if (threads.created) {
      current_frame[0] = next_frame[0];
      next_frame[0] = (current_frame[0] + 1) % MAX_FB_NUM;

      pthread_mutex_lock(&threads.mutex);
      while (frames[current_frame[0]] != NULL) {
        if (done) {
          return DR_OK;
        }
        pthread_cond_wait(&threads.cond, &threads.mutex);
      }
      frames[current_frame[0]] = frame;
      pthread_mutex_unlock(&threads.mutex);
    }
    else {
      write(pipefd[1], &frame, sizeof(void*));
    }

    return DR_OK;
  }

  return DR_NEED_IDR;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11 = {
  .setup = x11_setup,
  .cleanup = x11_cleanup,
  .submitDecodeUnit = x11_submit_decode_unit,
  .capabilities = CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME) | CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC | CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1 | CAPABILITY_DIRECT_SUBMIT,
};

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vdpau = {
  .setup = x11_setup_vdpau,
  .cleanup = x11_cleanup,
  .submitDecodeUnit = x11_submit_decode_unit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vaapi = {
  .setup = x11_setup_vaapi,
  .cleanup = x11_cleanup,
  .submitDecodeUnit = x11_submit_decode_unit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};
