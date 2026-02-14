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

#include "convert.h"
#include "ffmpeg.h"
#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif
#include "display.h"
#include "video.h"
#include "render.h"

#include "../input/evdev.h"
#include "../platform.h"
#include "../config.h"
#include "../loop.h"
#include "../util.h"

#define X11_VDPAU_ACCELERATION ENABLE_HARDWARE_ACCELERATION_1
#define X11_VAAPI_ACCELERATION ENABLE_HARDWARE_ACCELERATION_2
#define SLICES_PER_FRAME 4
#define WAYLAND_WINDOW 0x20
#define X11_WINDOW 0x40
#define GBM_WINDOW 0x80

VLIST_CREATE(decoder, MAX_FB_NUM);
VLIST_CREATE(render, MAX_FB_NUM);
VLIST_CREATE(display, MAX_FB_NUM);
VLIST_INIT(decoder, MAX_FB_NUM);
VLIST_INIT(render, MAX_FB_NUM);
VLIST_INIT(display, MAX_FB_NUM);

static bool isTenBit;
static bool firstDraw = true;

static void* ffmpeg_buffer = NULL;
static size_t ffmpeg_buffer_size = 0;

static void *display = NULL;
static void *window = NULL;

static int pipefd[2];
static int windowpipefd[2];

static int display_width = 0, display_height = 0;
static int frame_width, frame_height, screen_width, screen_height;

static uint64_t fps_time;
static uint64_t fps_time_10;

static struct DISPLAY_CALLBACK *disPtr = NULL;
static struct DISPLAY_CALLBACK *displayCallbacksPtr[] = {
#ifdef HAVE_WAYLAND
                                                     &display_callback_wayland,
#endif
#ifdef HAVE_X11
                                                     &display_callback_x11,
#endif
#ifdef HAVE_DRM
                                                     &display_callback_drm,
#endif
};
static struct RENDER_CALLBACK *renderPtr = NULL;
static struct RENDER_CALLBACK *renderCallbacksPtr[] = {
  &egl_render,
#ifdef HAVE_DRM
  &drm_render,
#endif
#if defined(HAVE_WAYLAND) && defined(HAVE_DRM)
  &wayland_render,
#endif
};

struct Multi_Thread {
  bool created;
  pthread_t decoder_id;
  pthread_t render_id;
  pthread_t display_id;
  pthread_mutex_t mutex;
  void* (*frame_handler)(void *data);
  void* (*decoder_handler)(void *data);
  void* (*display_handler)(void *data);
  sem_t render_sem;
  sem_t decoder_sem;
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

static ssize_t (*ffmpeg_export_images) (AVFrame *frame, struct Render_Image *image, void *descriptor, int render_type,
                                        int(*render_map_buffer)(struct Source_Buffer_Info *buffer,
                                                                int planes, int composeOrSeperate,
                                                                void* *image, int index),
                                        void(*render_unmap_buffer)(void **image, int planes));
static void (*ffmpeg_free_images) (void* *renderImages, void *descriptor, void(*render_unmap_buffer)(void* *image, int planes));
static void noop_void () {};

static void clear_threads() {
  if (threads.created) {
    done = true;
    LiWakeWaitForVideoFrame();

    usleep(fps_time);

    sem_post(&threads.decoder_sem);
    sem_post(&threads.render_sem);
    if (threads.render_id)
      pthread_join(threads.render_id, NULL);
    if (threads.decoder_id)
      pthread_join(threads.decoder_id, NULL);
    if (threads.display_id)
      pthread_join(threads.display_id, NULL);
    sem_destroy(&threads.render_sem);
    sem_destroy(&threads.decoder_sem);
  }
  return;
}

static int window_op_handle (int pipefd, void *data) {
  char *opCode = NULL;
  struct WINDOW_OP op = {0};
  int flags = 0;

  while (read(pipefd, &opCode, sizeof(char *)) > 0);
  if (strcmp(opCode, QUITCODE) == 0) {
    return LOOP_RETURN;
#if defined(HAVE_WAYLAND) || defined(HAVE_X11)
  } else if (strcmp(opCode, GRABCODE) == 0 || strcmp(opCode, FAKEGRABCODE) == 0) {
    flags |= (INPUTING | HIDE_CURSOR);
    op.hide_cursor = true;
    if (strcmp(opCode, FAKEGRABCODE) == 0)
      op.inputing = true;
  } else if (strcmp(opCode, UNGRABCODE) == 0 || strcmp(opCode, UNFAKEGRABCODE) == 0) {
    flags |= ((strcmp(opCode, UNGRABCODE) == 0 ? INPUTING : 0) | HIDE_CURSOR);
    if (strcmp(opCode, UNFAKEGRABCODE) == 0)
      op.inputing = true;
#endif
  }

  disPtr->display_modify_window(&op, flags);

  return LOOP_OK;
}

static inline void clear_frame (void *image_data) {
  struct Render_Image *image = (struct Render_Image *)image_data;
  ffmpeg_free_images(image->images.image_data, image->images.descriptor, renderPtr->render_unmap_buffer);
}

static inline void* draw_frame (struct Render_Image *images, AVFrame* frame, int *res) {
  int imageNum = 0;
  imageNum = ffmpeg_export_images(frame, images, images->images.descriptor, renderPtr->render_type, renderPtr->render_map_buffer, renderPtr->render_unmap_buffer);
  if (imageNum < 1) {
    *res = LOOP_RETURN;
    return NULL;
  }

  if (firstDraw) {
    firstDraw = false;
    if (isYUV444 && (!(frame->linesize[0] == frame->linesize[2] && frame->linesize[1] == frame->linesize[0]))) {
      fprintf(stderr, "There is not yuv444 format. Please try remove -yuv444 option to draw video!\n");
      ffmpeg_free_images(images->images.image_data, images->images.descriptor, renderPtr->render_unmap_buffer);
      *res = LOOP_RETURN;
      return NULL;
    }
    if (ffmpeg_decoder == SOFTWARE && strcmp(disPtr->name, renderPtr->name) == 0) {
      if (convert_init(frame, display_width, display_height) < 0) {
        *res = LOOP_RETURN;
        return NULL;
      }
    }

    struct Render_Config config = {0};
    config.color_space = ffmpeg_get_frame_colorspace(frame);
    config.full_color_range = ffmpeg_is_frame_full_range(frame);
    ffmpeg_get_plane_info(frame, &config.pix_fmt, &config.plane_nums, &config.yuv_order);
    config.image_nums = imageNum;
    for (int i = 0; i < config.plane_nums; i++) {
      config.linesize[i] = frame->linesize[i];
    }
    if (strcmp(disPtr->name, "drm") ==0)
      config.vsync = true;
    if (renderPtr->render_sync_config != NULL) {
      if (renderPtr->render_sync_config(&config) < 0) {
        *res = LOOP_RETURN;
        return NULL;
      }
    }
  }

  int index = renderPtr->render_draw(images);
  if (index < 0) {
    *res = LOOP_RETURN;
    return NULL;
  }

  *res = LOOP_OK;
  return images;
}

static inline void mv_vlist_display_to_decoder() {
  pthread_mutex_lock(&threads.mutex);
  void *image = VLIST_GET_DATA(display);
  clear_frame(image);
  VLIST_ADD(decoder, VLIST_GET_FRAME(display), image);
  VLIST_DEL(display);
  if (threads.created)
    sem_post(&threads.decoder_sem);
  pthread_mutex_unlock(&threads.mutex);

  return;
}

static inline void mv_vlist_render_to_display() {
  pthread_mutex_lock(&threads.mutex);
  VLIST_ADD(display, VLIST_GET_FRAME(render), VLIST_GET_DATA(render));
  VLIST_DEL(render);
  pthread_mutex_unlock(&threads.mutex);
  return;
}

static inline void mv_vlist_decoder_to_render() {
  pthread_mutex_lock(&threads.mutex);
  void *frame = VLIST_GET_FRAME(decoder);
  VLIST_ADD(render, frame, VLIST_GET_DATA(decoder));
  VLIST_DEL(decoder);
  pthread_mutex_unlock(&threads.mutex);
  if (threads.created) {
    sem_post(&threads.render_sem);
  }
  else {
    write(pipefd[1], &frame, sizeof(void*));
  }
  return;
}

static int frame_handle (int pipefd, void *data) {
  AVFrame* frame = NULL;

  if (done) return LOOP_RETURN;
  while (read(pipefd, &frame, sizeof(void*)) > 0);
  if (frame) {
    int res;
    pthread_mutex_lock(&threads.mutex);
    AVFrame *vframe = VLIST_GET_FRAME(render);
    void *image_data = VLIST_GET_DATA(render);
    pthread_mutex_unlock(&threads.mutex);
    if (vframe != frame) {
      fprintf(stderr, "Get frame error.\n");
      return LOOP_RETURN;
    }
    struct Render_Image *image = draw_frame((struct Render_Image *)image_data, frame, &res);
    int dis_res = -1;
    if (res == LOOP_RETURN) {
      return res;
    }
    mv_vlist_render_to_display();

    if (disPtr->display_vsync_loop) {
      dis_res = disPtr->display_vsync_loop(&done, frame_width, frame_height, image->index);
    }
    else {
      dis_res = disPtr->display_put_to_screen(display_width, display_height, image->index);
      if (dis_res == NEED_CHANGE_WINDOW_SIZE) {
        if (renderPtr->render_sync_window_size) {
          disPtr->display_get_resolution(&display_width, &display_height, false);
          renderPtr->render_sync_window_size(display_width, display_height, false);
        }
      }
    }
    if (dis_res < 0) return LOOP_RETURN;

    mv_vlist_display_to_decoder();

    return res;
  }

  return LOOP_OK;
}

static void* frame_handler (void *data) {

  pthread_setname_np(threads.render_id, "m_render_t");

  while (!done) {
    sem_wait(&threads.render_sem);
    if (done) {
      break;
    }
    pthread_mutex_lock(&threads.mutex);
    AVFrame *frame = VLIST_GET_FRAME(render);
    void *image_data = VLIST_GET_DATA(render);
    int renderNum = VLIST_NUM(render);
    if (!frame) {
      fprintf(stderr, "Error: Get NULL frame now.\n");
      break;
    }

    int frameNums = 0;
    sem_getvalue(&threads.render_sem, &frameNums);
    if (renderNum > 2) {
      if (sem_trywait(&threads.render_sem) == 0) {
        VLIST_ADD(decoder, frame, image_data);
        VLIST_DEL(render);
        frame = VLIST_GET_FRAME(render);
        image_data = VLIST_GET_DATA(render);
        sem_post(&threads.decoder_sem);
      }
    }
    pthread_mutex_unlock(&threads.mutex);

    int res;
    draw_frame((struct Render_Image *)image_data, frame, &res);
    if (res == LOOP_RETURN) {
      break;
    }
    mv_vlist_render_to_display();
    if (disPtr->display_vsync_loop == NULL) {
      int dis_res = disPtr->display_put_to_screen(display_width, display_height, ((struct Render_Image *)image_data)->index);
      if (dis_res < 0) {
        break;
      }
      else if (dis_res == NEED_CHANGE_WINDOW_SIZE) {
        if (renderPtr->render_sync_window_size) {
          disPtr->display_get_resolution(&display_width, &display_height, false);
          renderPtr->render_sync_window_size(display_width, display_height, false);
        }
      }
      mv_vlist_display_to_decoder();
    }
  }

  done = true;
  sem_post(&threads.decoder_sem);
  return NULL;
}

static void* display_handler (void *data) {
  pthread_setname_np(threads.render_id, "m_display_t");

  bool start = false;
  // wait for render
  while (!start && !done) {
    usleep(fps_time_10);
    if (VLIST_NUM(display) > 0) {
      start = true;
    }
  }

  while (!done) {
    pthread_mutex_lock(&threads.mutex);
    AVFrame *last_frame = VLIST_GET_FRAME(display);
    struct Render_Image *last_image_data = (struct Render_Image *)VLIST_GET_DATA(display);
    int displayNum = VLIST_NUM(display);
    if (displayNum > (MAX_FB_NUM - 1)) {
      VLIST_DEL(display);
      while (VLIST_NUM(display) > 0) {
        AVFrame *middle_frame = VLIST_GET_FRAME(display);
        struct Render_Image *middle_image_data = (struct Render_Image *)VLIST_GET_DATA(display);
        clear_frame(middle_image_data);
        VLIST_DEL(display);
        VLIST_ADD(decoder, middle_frame, middle_image_data);
        sem_post(&threads.decoder_sem);
      }
      while (VLIST_NUM(display) == 0) {
        pthread_mutex_unlock(&threads.mutex);
        if (done) goto display_exit;
        usleep(fps_time_10);
        pthread_mutex_lock(&threads.mutex);
      }
    }
    else if (displayNum > 1) {
      VLIST_DEL(display);
    }
    struct Render_Image *image_data = (struct Render_Image *)VLIST_GET_DATA(display);
    pthread_mutex_unlock(&threads.mutex);
    if (image_data == NULL) {
      fprintf(stderr, "Error: Get NULL image data.\n");
      goto display_exit;
    }
    if (disPtr->display_vsync_loop(&done, frame_width, frame_height, image_data->index) < 0) {
      fprintf(stderr, "Error: display loop failed.\n");
      goto display_exit;
    }
    if (last_image_data != image_data) {
      pthread_mutex_lock(&threads.mutex);
      clear_frame(last_image_data);
      VLIST_ADD(decoder, last_frame, last_image_data);
      pthread_mutex_unlock(&threads.mutex);
      sem_post(&threads.decoder_sem);
    }
    else {
      if (displayNum > 1) {
        goto display_exit;
      }
    }
  }

display_exit:
  done = true;
  sem_post(&threads.decoder_sem);

  return NULL;
}

// declare funtion here
int x11_submit_decode_unit(PDECODE_UNIT decodeUnit);
static void* decoder_thread(void *data) {
  pthread_setname_np(threads.decoder_id, "m_decoder_t");

  while (!done) {
    VIDEO_FRAME_HANDLE handle;
    PDECODE_UNIT du;
    if (!LiWaitForNextVideoFrame(&handle, &du)) {
      // if false ,need exit now.
      break;
    }

    sem_wait(&threads.decoder_sem);
    if (done) {
      LiCompleteVideoFrame(handle, DR_OK);
      break;
    }

    // blocking in x11_submit_decode_unit();
    int status = x11_submit_decode_unit(du);
    LiCompleteVideoFrame(handle, status);
  }

  done = true;
  sem_post(&threads.render_sem);

  return NULL;
}

int x11_init(const char *displayName, bool vaapi) {
  int res = 0;
  const char *displayDevice;
  // display and decoder may modify supportedVideoFormat
  supportedVideoFormat = (VIDEO_FORMAT_MASK_10BIT | VIDEO_FORMAT_MASK_YUV444 | VIDEO_FORMAT_MASK_H264 | VIDEO_FORMAT_MASK_H265 | VIDEO_FORMAT_MASK_AV1);

  int disIndex = 0;
  struct DISPLAY_CALLBACK *bestDisplay[3] = {0};
  struct RENDER_CALLBACK *bestRender[3] = {0};
  for (int i = 0; i < (sizeof(displayCallbacksPtr) / sizeof(displayCallbacksPtr[0])); i++) {
    disPtr = displayCallbacksPtr[i];

    if (displayName) {
      if (strcmp(disPtr->name, displayName) != 0)
        continue;
    }

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

      if ((displayName && renderPtr && strcmp(displayName, "wayland") == 0 && strcmp(renderPtr->name, "wayland") != 0) ||
          (displayName && renderPtr && strcmp(displayName, "drm") == 0 && strcmp(renderPtr->name, "drm") != 0) ||
          (wantHdr &&  strcmp(disPtr->name, renderPtr->name) != 0)) {
        renderPtr->render_destroy();
        renderPtr = NULL;
        continue;
      }

      if (vaapi) {
        if (renderPtr->is_hardaccel_support) {
    #ifdef HAVE_VAAPI
          int drm_fd = -1;
          void *dis;
          char drmNode[64] = {'\0'};
          drm_fd = get_drm_render_fd(drmNode);
          dis = (void *)&drm_fd;

          if (!vaapi_validate_test(disPtr->name, renderPtr->name, dis)) {
            renderPtr->is_hardaccel_support = false;
          }
          close(drm_fd);
    #endif
        }

        if (renderPtr->is_hardaccel_support) {
          break;
        }

        renderPtr->render_destroy();
        renderPtr = NULL;
      }

      if (renderPtr != NULL)
        break;
    }

    // has disPtr already, just choose render
    break;
  }

  if (disPtr == NULL || renderPtr == NULL) {
    if (!bestDisplay[0] || !bestRender[0]) {
      fprintf(stderr, "No display support! Please try another platform(-platform xxx).\n");
      return 0;
    }
    if (disPtr == NULL)
      disPtr = bestDisplay[0];
    renderPtr = bestRender[0];
    display = disPtr->display_get_display(&displayDevice);
    struct Render_Init_Info renderParas = {0};
    renderParas.display = display;
    renderParas.egl_platform = disPtr->egl_platform;
    renderParas.format = disPtr->format;
    renderPtr->render_create(&renderParas);
    renderPtr->is_hardaccel_support = false;
  }

  // display must report useHdr to decide is support hdr display
  supportedHDR = disPtr->hdr_support;

  if (renderPtr->is_hardaccel_support && vaapi) {
  #ifdef HAVE_VAAPI
    if (vaapi_init_lib(NULL) != -1) {
      supportedVideoFormat &= vaapi_supported_video_format();
      res = INIT_VAAPI;
      return res;
    }
  #endif
  } else if (strcmp(disPtr->name, "drm") == 0) {
  // yuv444 is always supported by software decoder
    res = INIT_DRM;
  }

  supportedVideoFormat &= software_supported_video_format();

  res = res != 0 ? res : INIT_EGL;

  return res;
}

int x11_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  ffmpegArgs.drFlags = drFlags;
  fps_time = ((int)(1000000 / (redrawRate)));
  fps_time_10 = (int) (fps_time / 10);

  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);

  if (disPtr->display_setup(width, height, redrawRate, drFlags | renderPtr->render_type) == -1)
    return -1;
  disPtr->display_get_resolution(&screen_width, &screen_height, true);
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
  avc_flags |= renderPtr->render_type;

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
  renderParas.fixed_resolution = drFlags & FIXED_RESOLUTION;
  renderParas.fill_resolution = drFlags & FILL_RESOLUTION;
  if (renderPtr->display_name == NULL) {
    renderPtr->display_name = disPtr->name;
  }
  if (strcmp(disPtr->name, "drm") == 0)
    renderParas.use_display_buffer = true;
  renderParas.display_exported_buffer = disPtr->display_exported_buffer_info;
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

  pthread_mutexattr_t mattr;
  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT);
  pthread_mutex_init(&threads.mutex, &mattr);
  pthread_mutexattr_destroy(&mattr);
  if (!(CAPABILITY_DIRECT_SUBMIT & decoder_callbacks_x11.capabilities) ||
      !(CAPABILITY_DIRECT_SUBMIT & decoder_callbacks_x11_vaapi.capabilities)) {
    threads.created = true;
    threads.frame_handler = frame_handler;
    threads.decoder_handler = decoder_thread;
    threads.display_handler = display_handler;
    sem_init(&threads.render_sem, 0, 0);
    sem_init(&threads.decoder_sem, 0, MAX_FB_NUM);
    if (disPtr->display_vsync_loop != NULL &&
        pthread_create(&threads.display_id, NULL, threads.display_handler, &pipefd[0]) != 0) {
      fprintf(stderr, "Error: Cannot create dislpay thread! Please try again or try direct submit mode.\n");
      return -1;
    }
    if (pthread_create(&threads.render_id, NULL, threads.frame_handler, &pipefd[0]) != 0 ||
        pthread_create(&threads.decoder_id, NULL, threads.decoder_handler, &pipefd[0]) != 0) {
      clear_threads();
      fprintf(stderr, "Error: Cannot create decoder/render/dislpay thread! Please try again or try direct submit mode.\n");
      return -1;
    }
    pthread_setprio(threads.render_id, 95);
    pthread_setprio(threads.decoder_id, 94);
    if (threads.display_id > 0) pthread_setprio(threads.display_id, 96);
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
  if (strcmp(disPtr->name, "x11") == 0 || strcmp(disPtr->name, "wayland") == 0)
    evdev_pass_mouse_mode(true);

  struct _WINDOW_PROPERTIES window_properties = {0};
  window_properties.fd_p = &windowpipefd[1];
  window_properties.configure = &window_configure;
 
  disPtr->display_setup_post((void *)&window_properties);

  if (ffmpeg_decoder != SOFTWARE) {
    ffmpeg_export_images = &vaapi_export_render_images;
    ffmpeg_free_images = &vaapi_free_render_images;
  } else {
    ffmpeg_export_images = &software_store_frame;
    ffmpeg_free_images = &noop_void;
  }

  memset(renderPtr->images, 0, sizeof(struct Render_Image) * MAX_FB_NUM);

  // file vlist quene
  void **vaapi_descriptors = vaapi_get_descriptors_ptr();
  AVFrame **frames = ffmpeg_get_frames();
  for (int i = 0; i < MAX_FB_NUM; i++) {
    VLIST_ADD(decoder, frames[i], &renderPtr->images[i]);
    renderPtr->images[i].images.descriptor = vaapi_descriptors[i];
    renderPtr->images[i].sframe.frame = frames[i];
    renderPtr->images[i].index = i;
  }

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
    evdev_trans_op_fd(-1);
    loop_remove_fd(windowpipefd[0]);
    close(windowpipefd[1]);
    close(windowpipefd[0]);
    windowpipefd[1] = -1;
    windowpipefd[0] = -1;
  }
  if (pipefd[1] > 0) {
    loop_remove_fd(pipefd[0]);
    close(pipefd[1]);
    close(pipefd[0]);
    pipefd[1] = -1;
    pipefd[0] = -1;
  }
  if (ffmpeg_decoder == SOFTWARE && strcmp(disPtr->name, renderPtr->name) == 0) {
    convert_destroy();
  }
  for (int i = 0; i < MAX_FB_NUM; i++) {
    ffmpeg_free_images(renderPtr->images[i].images.image_data, renderPtr->images[i].images.descriptor, renderPtr->render_unmap_buffer);
  }
  renderPtr->render_destroy();
  ffmpeg_destroy();

  struct _WINDOW_PROPERTIES window_properties = {0};
  window_properties.configure = &window_configure;
  disPtr->display_close_display((void *)&window_properties);

  if (threads.mutex != 0)
    pthread_mutex_destroy(&threads.mutex);
  memset(&threads, 0, sizeof(threads));
  disPtr = NULL;
  renderPtr = NULL;
}

int x11_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  PLENTRY entry = decodeUnit->bufferList;
  int length = 0;

  if (done)
    return DR_OK;

  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);

  while (entry != NULL) {
    memcpy(ffmpeg_buffer+length, entry->data, entry->length);
    length += entry->length;
    entry = entry->next;
  }

  int err = ffmpeg_decode2(ffmpeg_buffer, length, decodeUnit->frameType == FRAME_TYPE_IDR ? AV_PKT_FLAG_KEY : 0);

  if (err < 0)
    goto next_handle;
  
  pthread_mutex_lock(&threads.mutex);
  AVFrame *frame = VLIST_GET_FRAME(decoder);
  pthread_mutex_unlock(&threads.mutex);
  if (frame == NULL)
    goto decode_exit;

  frame = ffmpeg_get_frame(frame, true);
  if (frame == NULL)
    goto decode_exit;

  mv_vlist_decoder_to_render();
  return DR_OK;

next_handle:
  if (threads.created) {
    sem_post(&threads.decoder_sem);
  }
  return DR_NEED_IDR;

decode_exit:
  done = true;
  return DR_OK;
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
