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

#include "egl.h"
#include "ffmpeg.h"
#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif
#include "video.h"

#include "x11.h"
#ifdef HAVE_WAYLAND
#include "wayland.h"
#endif

#include "../input/evdev.h"
#include "../loop.h"
#include "../util.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#define X11_VDPAU_ACCELERATION ENABLE_HARDWARE_ACCELERATION_1
#define X11_VAAPI_ACCELERATION ENABLE_HARDWARE_ACCELERATION_2
#define SLICES_PER_FRAME 4

extern bool isUseGlExt;
static bool isTenBit;
static bool isWayland = false;
static bool isYUV444;

static void* ffmpeg_buffer = NULL;
static size_t ffmpeg_buffer_size = 0;

static void *display = NULL;

static int pipefd[2];
static int windowpipefd[2];

static int display_width = 0;
static int display_height = 0;

static int window_op_handle (int pipefd) {
  char *opCode = NULL;

  while (read(pipefd, &opCode, sizeof(char *)) > 0);
  if (strcmp(opCode, QUITCODE) == 0) {
    return LOOP_RETURN;
  } else if (strcmp(opCode, GRABCODE) == 0) {
  } else if (strcmp(opCode, UNGRABCODE) == 0) {
  }

  return LOOP_OK;
}

static int (*render_handler) (AVFrame* frame);

static int frame_handle (int pipefd) {
  AVFrame* frame = NULL;
  while (read(pipefd, &frame, sizeof(void*)) > 0);
  if (frame)
    return render_handler(frame);

  return LOOP_OK;
}

static int software_draw (AVFrame* frame) {
  egl_draw(frame->data);
  return LOOP_OK;
}

static int vaapi_egl_draw (AVFrame* frame) {
#ifdef HAVE_VAAPI
  egl_draw_frame(frame);
  #ifdef HAVE_WAYLAND
  if (isWayland)
    wl_dispatch_event();
  #endif
  return LOOP_OK;
#endif
  return LOOP_RETURN;
}

static int vaapi_va_put (AVFrame* frame) {
  #ifdef HAVE_VAAPI
  x_vaapi_draw(frame, display_width, display_height);
  return LOOP_OK;
  #endif
  return LOOP_RETURN;
}

static int test_vaapi_va_put (AVFrame* frame) {
  #ifdef HAVE_VAAPI
  static int successTimes = 0;
  if (!isWayland && x_test_vaapi_draw(frame, display_width, display_height))
    successTimes++;
  else
    successTimes--;

  if (successTimes <= 0) {
    int dcFlag;
    dcFlag = isYUV444 ? (ffmpeg_decoder | YUV444) : ffmpeg_decoder;
#ifdef HAVE_WAYLAND
    dcFlag = isWayland ? (dcFlag | WAYLAND) : dcFlag;
#endif
    egl_init(display, display_width, display_height, dcFlag);

    if (!canExportSurfaceHandle(isTenBit) ||
        !isVaapiCanDirectRender() || isTenBit) {
      isUseGlExt = false;
    }

    if (isUseGlExt) {
#ifdef HAVE_WAYLAND
      if (isWayland)
        wl_setup_post();
#endif

      render_handler = vaapi_egl_draw;
    } else {
      fprintf(stderr, "Render failed and Please try another platform!\n");
      return LOOP_RETURN;
    }
  } else if (successTimes > 5) {
    render_handler = vaapi_va_put;
  }
  return LOOP_OK;
  #endif
  return LOOP_RETURN;
}

int x11_init(bool vdpau, bool vaapi) {
#ifdef HAVE_WAYLAND
  isWayland = getenv("WAYLAND_DISPLAY") == NULL ? false : true;
#endif
  const char *displayDevice = getenv("DISPLAY");

  if (vaapi) {
  #ifdef HAVE_VAAPI
    if (!isWayland)
      x_muilti_threads();
    if (vaapi_init_lib(isWayland ? NULL : displayDevice) != -1) {
#ifdef HAVE_WAYLAND
      display = isWayland ? wl_get_display(NULL) : x_get_display(displayDevice);
#else
      display = x_get_display(displayDevice);
#endif
      if (display != NULL) {
        return INIT_VAAPI;
      }
    }
  #endif
  }
  isWayland = false;
  x_muilti_threads();

  display = x_get_display(displayDevice);
  if (!display)
    return 0;

  return INIT_EGL;
}

int x11_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  ensure_buf_size(&ffmpeg_buffer, &ffmpeg_buffer_size, INITIAL_DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);

  if (!isWayland) {
    if (x_setup(width, height, drFlags) == -1)
      return -1;
    x_get_resolution(&display_width, &display_height);
  } else {
#ifdef HAVE_WAYLAND
    if (wayland_setup(width, height, drFlags) == -1)
      return -1;
    wl_get_resolution(&display_width, &display_height);
#endif
  }
  if (display_width == 0 || display_height == 0) {
    display_width = width;
    display_height = height;
  }

  int avc_flags;
  if (drFlags & X11_VDPAU_ACCELERATION)
    avc_flags = VDPAU_ACCELERATION;
  else if (drFlags & X11_VAAPI_ACCELERATION)
    avc_flags = VAAPI_ACCELERATION;
  else
    avc_flags = SLICE_THREADING;

  if (ffmpeg_init(videoFormat, width, height, avc_flags, 2, SLICES_PER_FRAME) < 0) {
    fprintf(stderr, "Couldn't initialize video decoding\n");
    return -1;
  }

  if (ffmpeg_decoder == SOFTWARE)
    render_handler = software_draw;
  else {
    render_handler = test_vaapi_va_put;
    isTenBit = videoFormat & VIDEO_FORMAT_MASK_10BIT;
  }

  if (pipe(pipefd) == -1 || pipe(windowpipefd) == -1) {
    fprintf(stderr, "Can't create communication channel between threads\n");
    return -2;
  }

  loop_add_fd(pipefd[0], &frame_handle, POLLIN);
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

  loop_add_fd(windowpipefd[0], &window_op_handle, POLLIN);
  fcntl(windowpipefd[0], F_SETFL, O_NONBLOCK);

  evdev_trans_op_fd(windowpipefd[1]);
#ifdef HAVE_WAYLAND
  if (isWayland)
    wl_trans_op_fd(windowpipefd[1]);
#endif

  return 0;
}

int x11_setup_vdpau(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  return x11_setup(videoFormat, width, height, redrawRate, context, drFlags | X11_VDPAU_ACCELERATION);
}

int x11_setup_vaapi(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  return x11_setup(videoFormat, width, height, redrawRate, context, drFlags | X11_VAAPI_ACCELERATION);
}

void x11_cleanup() {
  #ifdef HAVE_VAAPI
  if (render_handler != vaapi_va_put)
    egl_destroy();
  #else
  egl_destroy();
  #endif
  ffmpeg_destroy();
  if (ffmpeg_decoder == SOFTWARE)
    x_close_display();
#ifdef HAVE_WAYLAND
  if (isWayland)
    wl_close_display();
#endif
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

  ffmpeg_decode(ffmpeg_buffer, length);

  AVFrame* frame = ffmpeg_get_frame(true);

  if (frame != NULL)
    write(pipefd[1], &frame, sizeof(void*));

  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11 = {
  .setup = x11_setup,
  .cleanup = x11_cleanup,
  .submitDecodeUnit = x11_submit_decode_unit,
  .capabilities = CAPABILITY_SLICES_PER_FRAME(SLICES_PER_FRAME) | CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC | CAPABILITY_DIRECT_SUBMIT,
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
