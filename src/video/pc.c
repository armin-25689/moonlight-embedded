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

#include <errno.h>
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
#include "display.h"
#include "video.h"
#include "render.h"

#include "../input/evdev.h"
#include "../platform.h"
#include "../config.h"
#include "../loop.h"
#include "../util.h"

#define X11_VULKAN_ACCELERATION ENABLE_HARDWARE_ACCELERATION_1
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

static void* ffmpeg_buffer = NULL;
static size_t ffmpeg_buffer_size = 0;
static struct Image_Pool image_pools = {0};

static void *display = NULL;
static void *window = NULL;

static int windowpipefd[2];
static const evwcode quitstate = QUITCODE;

static int display_width = 0, display_height = 0;
static int display_feedback = 0;

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
  pthread_t decoder_id;
  pthread_t render_id;
  pthread_t display_id;
  pthread_mutex_t mutex;
  void* (*frame_handler)(void *data);
  void* (*decoder_handler)(void *data);
  void* (*display_handler)(void *data);
  sem_t display_sem;
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

static void clear_threads() {
  LiWakeWaitForVideoFrame();

  sem_post(&threads.decoder_sem);
  sem_post(&threads.render_sem);
  sem_post(&threads.display_sem);
  if (threads.decoder_id)
    pthread_join(threads.decoder_id, NULL);
  if (threads.render_id)
    pthread_join(threads.render_id, NULL);
  if (threads.display_id)
    pthread_join(threads.display_id, NULL);
  sem_destroy(&threads.display_sem);
  sem_destroy(&threads.render_sem);
  sem_destroy(&threads.decoder_sem);
  if (threads.mutex != 0)
    pthread_mutex_destroy(&threads.mutex);
  memset(&threads, 0, sizeof(threads));

  return;
}

static int window_op_handle (int pipefd, void *data) {
  evwcode getedCode = 0;
  struct WINDOW_OP op = {0};
  int flags = 0;

  while (read(pipefd, &getedCode, sizeof(getedCode)) > 0);
  evwcode opCode = getedCode & (~0xC0);
  switch (opCode) {
  case WINDOWSIZECHANGED:
    pthread_mutex_lock(&threads.mutex);
    disPtr->display_get_resolution(&display_width, &display_height, false);
    display_feedback = NEED_CHANGE_WINDOW_SIZE;
    pthread_mutex_unlock(&threads.mutex);
    return LOOP_OK;
  case QUITCODE:
    return LOOP_RETURN;
#if defined(HAVE_WAYLAND) || defined(HAVE_X11)
  case GRABCODE:
  case FAKEGRABCODE:
    flags |= (INPUTING | HIDE_CURSOR);
    op.hide_cursor = true;
    if (opCode == FAKEGRABCODE)
      op.inputing = true;
    break;
  case UNGRABCODE:
  case UNFAKEGRABCODE:
    flags |= ((opCode == UNGRABCODE ? INPUTING : 0) | HIDE_CURSOR);
    if (opCode == UNFAKEGRABCODE)
      op.inputing = true;
    break;
#endif
#if defined(HAVE_DRM)
  case VTF1CODE:
  case VTF2CODE:
  case VTF3CODE:
  case VTF4CODE:
  case VTF5CODE:
  case VTF6CODE:
  case VTF7CODE:
  case VTF8CODE:
  case VTF9CODE:
  case VTFACODE:
  case VTFBCODE:
  case VTFCCODE:
    op.switch_vt = opCode;
    if (getedCode & FROMDISPLAY)
      op.from_display_server = true;
    break;
#endif
  }

  disPtr->display_modify_window(&op, flags);

  return LOOP_OK;
}

static inline void* draw_frame (struct Render_Image *images, AVFrame* frame, int *res, bool *firstDraw) {
  if (*firstDraw) {
    *firstDraw = false;
    if (isYUV444 && (!(frame->linesize[0] == frame->linesize[2] && frame->linesize[1] == frame->linesize[0]))) {
      fprintf(stderr, "There is not yuv444 format. Please try remove -yuv444 option to draw video!\n");
      *res = LOOP_RETURN;
      return NULL;
    }
    if (ffmpeg_decoder == SOFTWARE && strcmp(disPtr->name, renderPtr->name) == 0) {
      if (convert_init(frame, frame->width, frame->height) < 0) {
        *res = LOOP_RETURN;
        return NULL;
      }
    }

    struct Render_Config config = {0};
    config.width = frame->width;
    config.height = frame->height;
    config.color_space = ffmpeg_get_frame_colorspace(frame);
    config.full_color_range = ffmpeg_is_frame_full_range(frame);
    ffmpeg_get_plane_info(frame, &config.pix_fmt, &config.plane_nums, &config.yuv_order);
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

static inline void mv_deled_display_data_todecoder (void *frame, void *image) {
  pthread_mutex_lock(&threads.mutex);
  VLIST_ADD(decoder, frame, image);
  sem_post(&threads.decoder_sem);
  pthread_mutex_unlock(&threads.mutex);
  return;
}

static inline void mv_vlist_render_to_display() {
  pthread_mutex_lock(&threads.mutex);
  VLIST_ADD(display, VLIST_GET_FRAME(render), VLIST_GET_DATA(render));
  VLIST_DEL(render);
  pthread_mutex_unlock(&threads.mutex);
  sem_post(&threads.display_sem);
  return;
}

static inline void mv_vlist_decoder_to_render() {
  pthread_mutex_lock(&threads.mutex);
  void *frame = VLIST_GET_FRAME(decoder);
  VLIST_ADD(render, frame, VLIST_GET_DATA(decoder));
  VLIST_DEL(decoder);
  pthread_mutex_unlock(&threads.mutex);
  sem_post(&threads.render_sem);
  return;
}

#define DISCARD_FRAMES_TO(dstsem, srcsem, dstvlist, srcvlist, max_keep, frame, image) \
  do { \
    int snum = 0; \
    int smax = max_keep; \
    *frame = VLIST_GET_FRAME(srcvlist); \
    *image = VLIST_GET_DATA(srcvlist); \
    snum = VLIST_NUM(srcvlist); \
    while (snum > smax && sem_trywait(srcsem) == 0) { \
      if (*frame == NULL) { \
        smax = 0; \
        sem_getvalue(srcsem, &snum); \
        continue; \
      } \
      VLIST_ADD(dstvlist, *frame, *image); \
      VLIST_DEL(srcvlist); \
      *frame = VLIST_GET_FRAME(srcvlist); \
      *image = VLIST_GET_DATA(srcvlist); \
      sem_post(dstsem); \
      snum--; \
    } \
  } while (0)

static inline void discard_frames_from_render_todecoder (sem_t *dstsem, sem_t *srcsem, uint8_t max_keep, void **frame, void **image) {
  DISCARD_FRAMES_TO(dstsem, srcsem, decoder, render, max_keep, frame, image);
  return;
}

static inline void discard_frames_from_display_todecoder (sem_t *dstsem, sem_t *srcsem, uint8_t max_keep, void **frame, void **image) {
  DISCARD_FRAMES_TO(dstsem, srcsem, decoder, display, max_keep, frame, image);
  return;
}
#undef DISCARD_FRAMES_TO

static void* frame_handler (void *data) {

  pthread_setname_np(threads.render_id, "m_render_t");
  AVFrame *frame = NULL;
  struct Render_Image *image_data = NULL;
  bool firstDraw = true;

  while (!done) {
    sem_wait(&threads.render_sem);
    if (done) {
      break;
    }
    pthread_mutex_lock(&threads.mutex);
    discard_frames_from_render_todecoder(&threads.decoder_sem, &threads.render_sem, 1, (void **)&frame, (void **)&image_data);
    if (display_feedback == NEED_CHANGE_WINDOW_SIZE) {
      if (renderPtr->render_sync_window_size)
        renderPtr->render_sync_window_size(display_width, display_height, false);
      display_feedback = 0;
    }
    pthread_mutex_unlock(&threads.mutex);

    if (!frame)
      continue;

    int res;
    draw_frame((struct Render_Image *)image_data, frame, &res, &firstDraw);
    if (res == LOOP_RETURN) {
      break;
    }
    mv_vlist_render_to_display();
  }

  // unbound context for egl
  if (renderPtr->render_sync_config != NULL)
    renderPtr->render_sync_config(NULL);

  pthread_mutex_lock(&threads.mutex);
  write(windowpipefd[1], &quitstate, sizeof(quitstate));
  pthread_mutex_unlock(&threads.mutex);

  return NULL;
}

static void* display_handler (void *data) {
  pthread_setname_np(threads.display_id, "m_display_t");
  AVFrame *lastframe = NULL;
  AVFrame *frame = NULL;
  struct Render_Image *lastimage = NULL;
  struct Render_Image *image_data = NULL;

  while (!done) {
    sem_wait(&threads.display_sem);

    if (done) goto display_exit;

    pthread_mutex_lock(&threads.mutex);
    discard_frames_from_display_todecoder(&threads.decoder_sem, &threads.display_sem, 1, (void **)&frame, (void **)&image_data);
    while ((sem_trywait(&threads.display_sem)) == 0);
    if (image_data == NULL) {
      pthread_mutex_unlock(&threads.mutex);
      continue;
    }
    VLIST_DEL(display);
    pthread_mutex_unlock(&threads.mutex);
    if (image_data == NULL) {
      fprintf(stderr, "Get image error occur.\n");
      goto display_exit;
    }

    int dis_ret = disPtr->display_put_to_screen(image_data, NULL);
    switch (dis_ret) {
    case 0:
      break;
    case -EBUSY:
    case -EAGAIN:
    case -EINTR:
      if (image_data != lastimage && lastimage != NULL) {
        mv_deled_display_data_todecoder (frame, image_data);
      }
      else {
        lastframe = frame;
        lastimage = image_data;
      }
      continue;
      break;
    default:
      if (dis_ret < 0) {
        fprintf(stderr, "Error: display loop failed.\n");
        goto display_exit;
      }
      break;
    }
    if (image_data != lastimage && lastimage != NULL) {
      mv_deled_display_data_todecoder (lastframe, lastimage);
    }
    lastframe = frame;
    lastimage = image_data;
  }

display_exit:
  pthread_mutex_lock(&threads.mutex);
  write(windowpipefd[1], &quitstate, sizeof(quitstate));
  pthread_mutex_unlock(&threads.mutex);

  return NULL;
}

static int x11_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  PLENTRY entry = decodeUnit->bufferList;
  int length = 0;

  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, decodeUnit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);

  while (entry != NULL) {
    memcpy(ffmpeg_buffer+length, entry->data, entry->length);
    length += entry->length;
    entry = entry->next;
  }

  int err = ffmpeg_decode2(ffmpeg_buffer, length, decodeUnit->frameType == FRAME_TYPE_IDR ? AV_PKT_FLAG_KEY : 0);
  if (done)
    return DR_OK;
  if (err < 0) {
    goto next_handle;
  }

  pthread_mutex_lock(&threads.mutex);
  struct Render_Image *image = (struct Render_Image *)VLIST_GET_DATA(decoder);
  pthread_mutex_unlock(&threads.mutex);
  if (image == NULL)
    goto decode_exit;

  err = ffmpeg_get_frame(image, true);
  if (err < 0)
    goto decode_exit;
  else if (err > 0) {
    if (err == F_TRY_AGAIN) {
      sem_post(&threads.decoder_sem);
      return DR_OK;
    }
    else
      goto next_handle;
  }

  mv_vlist_decoder_to_render();
  return DR_OK;

next_handle:
  sem_post(&threads.decoder_sem);
  return DR_NEED_IDR;

decode_exit:
  return DR_OK;
}

static void* decoder_thread(void *data) {
  pthread_setname_np(threads.decoder_id, "m_decoder_t");
  int laststatus = -3;
  int times = 0;

  while (!done) {

    sem_wait(&threads.decoder_sem);
    if (done)
      break;

    pthread_mutex_lock(&threads.mutex);
    struct Render_Image *image = (struct Render_Image *)VLIST_GET_DATA(decoder);
    pthread_mutex_unlock(&threads.mutex);
    int err = ffmpeg_get_frame(image, true);
    if (err == 0) {
      mv_vlist_decoder_to_render();
      continue;
    }

    VIDEO_FRAME_HANDLE handle;
    PDECODE_UNIT du;
    if (!LiWaitForNextVideoFrame(&handle, &du)) {
      // if false ,need exit now.
      break;
    }

    if (done) {
      LiCompleteVideoFrame(handle, DR_OK);
      break;
    }

    // blocking in x11_submit_decode_unit();
    int status = x11_submit_decode_unit(du);
    LiCompleteVideoFrame(handle, status);
    if (status == DR_NEED_IDR) {
      if (laststatus == status)
        times++;
      else
        times = 0;
      if (times > 3) {
        fprintf(stderr, "Decode failed much times. Try specify -platform.\n");
        break;
      }
    }
    laststatus = status;
  }

  pthread_mutex_lock(&threads.mutex);
  write(windowpipefd[1], &quitstate, sizeof(quitstate));
  pthread_mutex_unlock(&threads.mutex);

  return NULL;
}

int x11_init(const char *displayName, int hwType) {
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

      if (hwType) {
        if (renderPtr->is_hardaccel_support)
          break;

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

  if (renderPtr->is_hardaccel_support && hwType) {
    if (ffmpeg_hw_init_lib(NULL, hwType) != -1) {
      supportedVideoFormat &= ffmpeg_supported_video_format();
      res = hwType;
      return res;
    }
  } else if (strcmp(disPtr->name, "drm") == 0) {
  // yuv444 is always supported by software decoder
    res = INIT_DRM;
  }

  supportedVideoFormat &= ffmpeg_supported_video_format();

  res = res != 0 ? res : INIT_EGL;

  return res;
}

int x11_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  int screen_width, screen_height;
  ffmpegArgs.drFlags = drFlags;

  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);

  if (disPtr->display_setup(width, height, redrawRate, drFlags | renderPtr->render_type) == -1)
    return -1;
  disPtr->display_get_resolution(&screen_width, &screen_height, true);
  window = disPtr->display_get_window();
  printf("Based %s window\n", disPtr->name);

  if (drFlags & DISPLAY_FULLSCREEN && renderPtr->render_type == EGL_RENDER) {
    display_width = screen_width;
    display_height = screen_height;
  } else {
    display_width = width;
    display_height = height;
  }

  int avc_flags;
  if (drFlags & X11_VULKAN_ACCELERATION) {
    avc_flags = VULKAN_ACCELERATION;
  }
  else if (drFlags & X11_VAAPI_ACCELERATION) {
    avc_flags = VAAPI_ACCELERATION;
  }
  else {
    avc_flags = SLICE_THREADING;
  }
  avc_flags |= renderPtr->render_type;

  if (ffmpeg_init(videoFormat, width, height, avc_flags, MAX_FB_NUM, SLICES_PER_FRAME) < 0) {
    fprintf(stderr, "Couldn't initialize video decoding\n");
    return -1;
  }
  ffmpegArgs.videoFormat = videoFormat;
  ffmpegArgs.width = width;
  ffmpegArgs.height = height;
  ffmpegArgs.avc_flags = avc_flags;
  ffmpegArgs.buffer_count = MAX_FB_NUM;
  ffmpegArgs.thread_count = SLICES_PER_FRAME;

  isTenBit = videoFormat & VIDEO_FORMAT_MASK_10BIT;

  // egl not need filter
  if (renderPtr->render_type == EGL_RENDER)
    ffmpeg_remove_filter(FILTER_FLAGS);
  else if (renderPtr->render_type == DRM_RENDER)
    ffmpeg_need_filter(FILTER_TONEMAP_FORCE_BT2020);

  struct Render_Init_Info renderParas = {0};
  renderParas.window = window;
  renderParas.frame_width = width;
  renderParas.frame_height = height;
  renderParas.screen_width = screen_width;
  renderParas.screen_height = screen_height;
  renderParas.is_full_screen = drFlags & DISPLAY_FULLSCREEN;
  renderParas.is_yuv444 = isYUV444;
  renderPtr->decoder_type = ffmpeg_decoder;
  renderParas.fixed_resolution = drFlags & FIXED_RESOLUTION;
  renderParas.fill_resolution = drFlags & FILL_RESOLUTION;
  // drm can use fmt scale flag to convert yuv420p to bgr0 instead of nv12
  renderParas.use_filter = drFlags & FILTER_FLAGS;
  if (renderPtr->display_name == NULL) {
    renderPtr->display_name = disPtr->name;
  }
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
  loop_add_fd(windowpipefd[0], &window_op_handle, 0);
  fcntl(windowpipefd[0], F_SETFL, O_NONBLOCK);

  memset(renderPtr->images, 0, sizeof(struct Render_Image) * MAX_FB_NUM);

  image_pools.image_bufs = calloc(MAX_POOLS_COUNT, sizeof(void *) * MAX_PLANE_NUM);
  image_pools.frame_bufs = calloc(MAX_POOLS_COUNT, sizeof(uint8_t *));
  if (image_pools.image_bufs == NULL || image_pools.frame_bufs == NULL) {
    fprintf(stderr, "Alloc pools for image pools failed.\n");
    return -1;
  }
  // file vlist quene
  AVFrame **frames = ffmpeg_get_frames();
  for (int i = 0; i < MAX_FB_NUM; i++) {
    VLIST_ADD(decoder, frames[i], &renderPtr->images[i]);
    renderPtr->images[i].images.pools = &image_pools;
    renderPtr->images[i].images.image_data = image_pools.image_bufs[i];
    renderPtr->images[i].images.free = renderPtr->render_unmap_buffer;
    renderPtr->images[i].images.create = renderPtr->render_map_buffer;
    renderPtr->images[i].sframe.frame = frames[i];
    renderPtr->images[i].index = i;
  }

  evdev_trans_op_fd(windowpipefd[1]);
  if (strcmp(disPtr->name, "x11") == 0 || strcmp(disPtr->name, "wayland") == 0)
    evdev_pass_mouse_mode(true);

  struct _WINDOW_PROPERTIES window_properties = {0};
  window_properties.fd_p = &windowpipefd[1];
  window_properties.configure = &window_configure;
  disPtr->display_setup_post((void *)&window_properties);

  pthread_mutexattr_t mattr;
  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT);
  pthread_mutex_init(&threads.mutex, &mattr);
  pthread_mutexattr_destroy(&mattr);
  threads.frame_handler = frame_handler;
  threads.decoder_handler = decoder_thread;
  threads.display_handler = display_handler;
  sem_init(&threads.decoder_sem, 0, MAX_FB_NUM);
  sem_init(&threads.render_sem, 0, 0);
  sem_init(&threads.display_sem, 0, 0);
  int p_ret = pthread_create(&threads.display_id, NULL, threads.display_handler, "display_thread");
  p_ret += pthread_create(&threads.render_id, NULL, threads.frame_handler, "render_thread");
  p_ret += pthread_create(&threads.decoder_id, NULL, threads.decoder_handler, "decoder_thread");
  if (p_ret != 0) {
    clear_threads();
    fprintf(stderr, "Error: Cannot create decoder/render/dislpay thread!.\n");
    return -1;
  }
  pthread_setprio(threads.display_id, 96);
  pthread_setprio(threads.render_id, 95);
  pthread_setprio(threads.decoder_id, 94);

  return 0;
}

int x11_setup_vulkan(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  return x11_setup(videoFormat, width, height, redrawRate, context, drFlags | X11_VULKAN_ACCELERATION);
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

  if (renderPtr) {
    struct Render_Image *image = &renderPtr->images[0];
    if (image->images.free && image->images.layers > 0) {
      for (int i = 0; i < MAX_POOLS_COUNT; i++) {
        if (i < MAX_FB_NUM || image_pools.stat[i] != 0)
          image->images.free(image_pools.image_bufs[i], image->images.layers);
      }
    }
    renderPtr->render_destroy();
  }

  if (disPtr) {
    struct _WINDOW_PROPERTIES window_properties = {0};
    window_properties.configure = &window_configure;
    disPtr->display_close_display((void *)&window_properties);
  }

  if (disPtr && renderPtr) {
    if (ffmpeg_decoder == SOFTWARE && strcmp(disPtr->name, renderPtr->name) == 0) {
      convert_destroy();
    }
  }
  ffmpeg_destroy();

  if (image_pools.image_bufs)
    free(image_pools.image_bufs);
  if (image_pools.frame_bufs)
    free(image_pools.frame_bufs);
  memset(&image_pools, 0, sizeof(image_pools));
  if (renderPtr)
    memset(renderPtr->images, 0, sizeof(struct Render_Image) * MAX_FB_NUM);

  disPtr = NULL;
  renderPtr = NULL;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11 = {
  .setup = x11_setup,
  .cleanup = x11_cleanup,
  .submitDecodeUnit = NULL,
  .capabilities = CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME) | CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC | CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1 | CAPABILITY_PULL_RENDERER,
};

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vulkan = {
  .setup = x11_setup_vulkan,
  .cleanup = x11_cleanup,
  .submitDecodeUnit = NULL,
  .capabilities = CAPABILITY_PULL_RENDERER | CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC | CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1,
};

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11_vaapi = {
  .setup = x11_setup_vaapi,
  .cleanup = x11_cleanup,
  .submitDecodeUnit = NULL,
  .capabilities = CAPABILITY_PULL_RENDERER,
};
