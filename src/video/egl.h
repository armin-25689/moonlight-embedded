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
#define MAX_EGL_ATTRS_NUM 66

#ifndef EGL_PLATFORM_X11_KHR
#define EGL_PLATFORM_X11_KHR 0x31D5
#endif
#ifndef EGL_PLATFORM_WAYLAND_KHR
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#endif
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#ifndef EGL_CONTEXT_MAJOR_VERSION_KHR
#define EGL_CONTEXT_MAJOR_VERSION_KHR 0x3098
#endif
#ifndef EGL_CONTEXT_MINOR_VERSION_KHR
#define EGL_CONTEXT_MINOR_VERSION_KHR 0x30FB
#endif
#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x0040
#endif

struct EXTSTATE {
  bool eglIsSupportExtDmaBuf;
  bool eglIsSupportExtDmaBufMod;
  bool eglIsSupportExtSurfaceless;
  bool eglIsSupportExtImageOES;
};

struct Import_Buffer_Info {
  EGLImage image[4];
  GLuint texture[4];
  GLuint framebuffer;
};

struct EGL_Base {
  int displayBufferPlaneNum;
  int displayBufferNum;
  int current;
  int next;
  int width;
  int height;
  int screen_width;
  int screen_height;
  float cutwidth;
  GLuint VAO;
  GLuint shader_program_packed;
  GLuint shader_program_nv12;
  GLuint shader_program_yuv;
  GLuint common_vertex_shader;
  GLuint yuv_fragment_shader;
  GLuint nv12_fragment_shader;
  GLuint packed_fragment_shader;
  bool eglVSync;
  bool display_buffer;
  struct Import_Buffer_Info *back_out_fb;
};

struct EGLImage_Attrs_Slot {
  EGLint fd[4];
  EGLint offset[4];
  EGLint pitch[4];
  EGLint lo_modi[4];
  EGLint hi_modi[4];
};

struct EGLImage_Attrs_Slot eglImageAttrsSlot = {
  .fd = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT},
  .offset = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT},
  .pitch = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT},
  .lo_modi = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT},
  .hi_modi = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
};
