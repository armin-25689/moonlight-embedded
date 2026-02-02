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

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <libavcodec/avcodec.h>

#include <Limelight.h>

// include glsl
#include "egl.h"
#include "egl_glsl.h"
#include "video_internal.h"
#include "render.h"

static struct EGL_Base egl_base = {0};
static struct Import_Buffer_Info out_fb[MAX_FB_NUM] = {0};
static struct EXTSTATE ExtState = {0};

static EGLDisplay display = EGL_NO_DISPLAY;
static EGLSurface surface = EGL_NO_SURFACE;
static EGLContext context = EGL_NO_CONTEXT;
static EGLConfig config;
static EGLSync eglsync = EGL_NO_SYNC;

static GLuint texture_id[4], texture_uniform[7];

static const float *colorOffsets;
static const float *colorspace;
static int egl_max_planes = 4;
static int planeNum = 4, imageNum = 0;
static const int *yuvOrder;
struct Window_Size {
  int frame_width;
  int frame_height;
  int view_width;
  int view_height;
} static window_info = {0};

static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;
static PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC glEGLImageTargetTexStorageEXT;

// for planeNum 0, 1, 2, 3, 4
static GLuint *shaders[5] = { &egl_base.shader_program_nv12, &egl_base.shader_program_packed, &egl_base.shader_program_nv12, &egl_base.shader_program_yuv, &egl_base.shader_program_nv12 };
static const EGLint context_attributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0,  EGL_NONE };

static void egl_unmap_eglimage(EGLImage *eglImage, int bufferPlaneNum) {
  for (int i = 0; i < bufferPlaneNum; i++) {
    if (eglImage[i]) {
      eglDestroyImage(display, eglImage[i]);
    }
  }
  memset(eglImage, 0, sizeof(EGLImage) * bufferPlaneNum);
}

static int egl_map_buffer_to_eglimage(struct Source_Buffer_Info *buffer, int bufferPlaneNum, int composeOrSeperate, EGLImage *eglImage, int index) {
  // layers
  int planes = composeOrSeperate == COMPOSE_PLANE ? 1 : bufferPlaneNum;
  // planes in layers
  int planesi = composeOrSeperate == COMPOSE_PLANE ? bufferPlaneNum : 1;

  for (int j = 0; j < planes; j++) {
    int attrsNum = 0;
    EGLAttrib attrs[MAX_EGL_ATTRS_NUM] = {0};
    attrs[attrsNum++] = EGL_WIDTH;
    attrs[attrsNum++] = buffer->width[j];
    attrs[attrsNum++] = EGL_HEIGHT;
    attrs[attrsNum++] = buffer->height[j];
    attrs[attrsNum++] = EGL_LINUX_DRM_FOURCC_EXT,
    attrs[attrsNum++] = buffer->format[j];

    for (int k = 0; k < planesi; k++) {
      attrs[attrsNum++] = eglImageAttrsSlot.fd[k];
      attrs[attrsNum++] = buffer->fd[j + k];
      attrs[attrsNum++] = eglImageAttrsSlot.pitch[k];
      attrs[attrsNum++] = buffer->stride[j + k];
      attrs[attrsNum++] = eglImageAttrsSlot.offset[k];
      attrs[attrsNum++] = buffer->offset[j + k];
      if (ExtState.eglIsSupportExtDmaBufMod) {
        attrs[attrsNum++] = eglImageAttrsSlot.lo_modi[k];
        attrs[attrsNum++] = (EGLint)(buffer->modifiers[j + k] & 0xFFFFFFFF);
        attrs[attrsNum++] = eglImageAttrsSlot.hi_modi[k];
        attrs[attrsNum++] = (EGLint)(buffer->modifiers[j + k] >> 32);
      }
    }

    attrs[attrsNum++] = EGL_NONE;

    eglImage[j] = eglCreateImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                 NULL, attrs);
    if (eglImage[j] == EGL_NO_IMAGE) {
      fprintf(stderr, "Failed to make image from buffer object: %x\n", eglGetError());
      egl_unmap_eglimage(eglImage, j);
      return -1;
    }
  }

  return 0;
}

static int map_display_buffer_framebuffer(struct Import_Buffer_Info out_fb[MAX_FB_NUM], void(*display_exported_buffer)(struct Source_Buffer_Info *buffer, int *buffersNum, int *planesNum)) {
  struct Source_Buffer_Info buffers[MAX_FB_NUM] = {0};

  if (!display_exported_buffer) {
    printf("EGL: Has no display_exported_buffer\n");
    return -1;
  }
  display_exported_buffer(buffers, &egl_base.displayBufferNum, &egl_base.displayBufferPlaneNum);

  if (egl_base.displayBufferNum > MAX_FB_NUM) {
    printf("EGL: Buffers num for display is so large\n");
    return -1;
  }

  for (int i = 0; i < egl_base.displayBufferNum; i++) {
    glGenFramebuffers(1, &out_fb[i].framebuffer);

    if (egl_map_buffer_to_eglimage(&buffers[i], egl_base.displayBufferPlaneNum, SEPERATE_PLANE, out_fb[i].image, i) < 0) {
      // may be not needed
      glDeleteFramebuffers(1, &out_fb[i].framebuffer);
      return -1;
    }

    for (int j = 0; j < egl_base.displayBufferPlaneNum; j++) {
      glGenTextures(1, &out_fb[i].texture[j]);
      glBindTexture(GL_TEXTURE_2D, out_fb[i].texture[j]);
      glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, out_fb[i].image[j]);
      glBindTexture(GL_TEXTURE_2D, 0);

      glBindFramebuffer(GL_FRAMEBUFFER, out_fb[i].framebuffer);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, out_fb[i].texture[j], 0);

      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Failed framebuffer check for created target buffer: %x\n",
                glCheckFramebufferStatus(GL_FRAMEBUFFER));
        // test if clear use egl_destroy when falied1111111111111111111111111111
        glDeleteFramebuffers(1, &out_fb[i].framebuffer);
        glDeleteTextures(1, &out_fb[i].texture[j]);
        eglDestroyImage(display, out_fb[i].image[j]);
        return -1;
      }
    }
  }
  return 0;
}

static EGLBoolean choose_gbm_config (EGLDisplay display, EGLint const * attrib_list, EGLConfig *dst_config, EGLint *config_nums, int format) {
  EGLint num_configs;
  if (!eglGetConfigs(display, NULL, 0, &num_configs)) {
    return EGL_FALSE;
  }
  EGLConfig *configs = malloc(num_configs * sizeof(EGLConfig));
  if (!eglChooseConfig(display, attrib_list,
                       configs, num_configs, &num_configs)) {
    return EGL_FALSE;
  }
  if (num_configs == 0) {
    return EGL_FALSE;
  }
  *config_nums = num_configs;
  // Find a config whose native visual ID is the desired GBM format.
  for (int i = 0; i < num_configs; ++i) {
    EGLint gbm_format;

    if (!eglGetConfigAttrib(display, configs[i],
                            EGL_NATIVE_VISUAL_ID, &gbm_format)) {
      return EGL_FALSE;
    }

    if (gbm_format == format) {
      memcpy(dst_config, &configs[i], sizeof(EGLConfig));
      free(configs);
      return EGL_TRUE;
    }
  }
  free(configs);
  return EGL_FALSE;
}

static bool egl_test_display_extension (int platform) {
  // test egl support with gbm surface
  const char *client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  if (!client_extensions)
    return false;
  switch (platform) {
  case EGL_PLATFORM_GBM_KHR:
    if (!strstr(client_extensions, "EGL_KHR_platform_gbm")) {
      return false;
    }
    break;
  case EGL_PLATFORM_X11_KHR:
    if (!strstr(client_extensions, "EGL_KHR_platform_x11")) {
      return false;
    }
    break;
  case EGL_PLATFORM_WAYLAND_KHR:
    if (!strstr(client_extensions, "EGL_KHR_platform_wayland")) {
      return false;
    }
    break;
  }
  return true;
}

static bool is_gl_extension_support(const char *extensionname) {
  char firstChar = *extensionname;

  if (firstChar == 'E') {
    const char *allExtensions = eglQueryString(display, EGL_EXTENSIONS);
    if (allExtensions && strstr(allExtensions, extensionname))
      return true;
  } else {
    int glExtensionsNum = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &glExtensionsNum);
    if (glExtensionsNum > 0) {
      const char *glExtensionName = NULL;
      for (GLuint i = 0; i < glExtensionsNum; i++) {
        glExtensionName = glGetStringi(GL_EXTENSIONS, i);
        if (glExtensionName != NULL) {
          if (strcmp(extensionname, glExtensionName) == 0) {
            return true;
          }
        }
      }
    }
  }

  return false;
}

static bool is_use_ext(struct EXTSTATE *extState) {
  // egl_khr_image_base or egl_khr_image is at leaset one exist
  if (!is_gl_extension_support("EGL_KHR_image_base") && !is_gl_extension_support("EGL_KHR_image")) {
    fprintf(stderr, "EGL: could not support rendering by egl because of no EGL_KHR_image_base or EGL_KHR_image extensions\n" );
    return false;
  }

  // gl_oes_egl_image require egl_khr_image_base or egl_khr_image,and needed for gleglimagetargettexture2does function
  if (!is_gl_extension_support("GL_OES_EGL_image")) {
    fprintf(stderr, "EGL: could not support rendering by egl because of no GL_OES_EGL_image extensions\n" );
    return false;
  }
  else {
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (glEGLImageTargetTexture2DOES == NULL) {
      return false;
    }
    extState->eglIsSupportExtImageOES = true;
  }

  if (!is_gl_extension_support("GL_EXT_EGL_image_storage")) {
    fprintf(stderr, "EGL: could not support rendering by egl because of no GL_EXT_EGL_image_storage extensions\n" );
  }
  else {
    glEGLImageTargetTexStorageEXT = (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)eglGetProcAddress("glEGLImageTargetTexStorageEXT");
  }

  if (is_gl_extension_support("EGL_KHR_surfaceless_context") && is_gl_extension_support("GL_OES_surfaceless_context")) {
    extState->eglIsSupportExtSurfaceless = true;
  }
  
  // For vaapi-EGL or gbm
  if (is_gl_extension_support("EGL_EXT_image_dma_buf_import")) {
    extState->eglIsSupportExtDmaBuf = true;
  }
  if (is_gl_extension_support("EGL_EXT_image_dma_buf_import_modifiers")) {
    extState->eglIsSupportExtDmaBufMod = true; 
  }

  return true;
}

static int generate_shader(GLuint *dst_shader, const char* dst_source, int gl_shader_type) {
  *dst_shader = glCreateShader(gl_shader_type);
  glShaderSource(*dst_shader, 1, &dst_source, NULL);
  glCompileShader(*dst_shader);
  // try to display compile errpr
  GLint status;
  char szLog[1024] = {0};
  GLsizei logLen = 0;
  glGetShaderiv(*dst_shader, GL_COMPILE_STATUS, &status);
  glGetShaderInfoLog(*dst_shader, 1024, &logLen,szLog);
  if(!status) {
   printf("Shader(type:%d) compile error:%s\n", gl_shader_type, szLog);
   return -1;
  }
  return 0;
}

static int egl_create(struct Render_Init_Info *paras) {
  memset(&egl_base, 0, sizeof(egl_base));

  if (!egl_test_display_extension(paras->egl_platform)) {
    fprintf( stderr, "EGL: Cannot start egl because of no EGL_KHR_platform_xxx support\n");
    return -1;
  }

  // get an EGL display connection
  PFNEGLGETPLATFORMDISPLAYPROC eglGetPlatformDisplayProc;
  eglGetPlatformDisplayProc = (typeof(eglGetPlatformDisplayProc)) eglGetProcAddress("eglGetPlatformDisplay");
  if (eglGetPlatformDisplayProc != NULL) {
    display = eglGetPlatformDisplayProc(paras->egl_platform, paras->display, NULL); //EGL_PLATFORM_X11_KHR for x11;EGL_PLATFORM_GBM_KHR for drm
  }
  if (display == EGL_NO_DISPLAY) {
    fprintf( stderr, "EGL: error get display\n" );
    return -1;
  }

  // initialize the EGL display connection
  int major, minor;
  EGLBoolean result = eglInitialize(display, &major, &minor);
  if (result == EGL_FALSE) {
    fprintf( stderr, "EGL: error initialising display\n");
    return -1;
  }

  // get our config from the config class
  const EGLint *attribute_list;
  static const EGLint attribute_list_8bit[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, EGL_DONT_CARE, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_DEPTH_SIZE, 24, EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER, EGL_NONE };
  static const EGLint attribute_list_10bit[] = { EGL_RED_SIZE, 10, EGL_GREEN_SIZE, 10, EGL_BLUE_SIZE, 10, EGL_ALPHA_SIZE, EGL_DONT_CARE, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_DEPTH_SIZE, 24, EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER, EGL_NONE };
  if (useHdr) {
    attribute_list = attribute_list_10bit;
  }
  else {
    attribute_list = attribute_list_8bit;
  }

  EGLint totalConfigsFound = 0;
  if (paras->format > 0) {
    result = choose_gbm_config(display, attribute_list, &config, &totalConfigsFound, paras->format);
  }
  else {
    result = eglChooseConfig(display, attribute_list, &config, 1, &totalConfigsFound);
  }
  if (result != EGL_TRUE || totalConfigsFound == 0) {
    fprintf(stderr, "EGL: Unable to query for available configs, found %d.\n", totalConfigsFound);
    return -1;
  }

  // bind the OpenGL API to the EGL
  result = eglBindAPI(EGL_OPENGL_ES_API);
  if (result == EGL_FALSE) {
    fprintf(stderr, "EGL: error binding API\n");
    return -1;
  }

  // create an EGL rendering context
  context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
  if (context == EGL_NO_CONTEXT) {
    fprintf(stderr, "EGL: couldn't get a valid context\n");
    return -1;
  }

  eglMakeCurrent(display, surface, surface, context);

  // test egl and opengl es version
  double egl_version = atof(eglQueryString(display, EGL_VERSION));
  GLint gles_version;
  eglQueryContext(display, context, EGL_CONTEXT_CLIENT_VERSION, &gles_version);
  if (is_use_ext(&ExtState) && (egl_version >= 1.5 && gles_version >= 3)) {
    egl_render.is_hardaccel_support = true;
  }
  else {
    fprintf(stderr, "WARNING:(EGL) couldn't initialize opengl es context for vaapi because of lacking extensions or egl version(%s) or gles version(%d)\n", eglQueryString(display, EGL_VERSION), gles_version);
    egl_render.is_hardaccel_support = false;
  }

  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return 0;
}

static void egl_sync_window_size(int width, int height, bool isfullscreen) {
  // for software render ,because we want cut the not needed view range
  if (width == 0 || height == 0)
    return;

  bool isFullScreen = isfullscreen;
  if (egl_base.screen_height == height && egl_base.screen_width == width)
    isFullScreen = true;

  int screen_w, screen_h, offset_x = 0, offset_y = 0;
  float multi_w = (window_info.frame_width / (float)egl_base.screen_width);
  float multi_h = (window_info.frame_height / (float)egl_base.screen_height);
  float multi = multi_w > multi_h ? multi_w : multi_h;

  if (isFullScreen) {
    screen_w = egl_base.screen_width;
    screen_h = egl_base.screen_height;
  }
  else {
    screen_w = width;
    screen_h = height;
  }

  // for fixed resolution ratio
  if (egl_base.fixed_resolution) {
    if ((width / (float)height) < (window_info.frame_width / (float)window_info.frame_height)) {
      screen_w = width;
      screen_h = (int)width * window_info.frame_height / (float)window_info.frame_width;
    }
    else {
      screen_w = (int)height * window_info.frame_width / (float)window_info.frame_height;
      screen_h = height;
    }
  }

  if (egl_base.fill_resolution) {
    screen_w = (int)(screen_w * (egl_base.screen_height / (float)window_info.frame_height)) * multi;
    screen_h = (int)(screen_h * (egl_base.screen_width / (float)window_info.frame_width)) * multi;
  }

  offset_x = (int)((isFullScreen ? egl_base.screen_width : width) - screen_w) / 2;
  offset_y = (int)((isFullScreen ? egl_base.screen_height : height) - screen_h) / 2 ;

  if (egl_render.decoder_type == SOFTWARE && window_info.frame_width != egl_base.width) {
    // cut display area,importent for software decoder
    screen_w = (int)screen_w * (multi_w > multi_h ? (egl_base.fill_resolution ? (1 / multi) : (egl_base.width / (float)window_info.frame_width)) : (egl_base.width / (float)window_info.frame_width));
  }
/*
  else if (egl_base.display_buffer) {
    window_info.view_width = width;
    window_info.view_height = height;
  }
*/

  window_info.view_width = screen_w;
  window_info.view_height = screen_h;

  glViewport(offset_x, offset_y, window_info.view_width, window_info.view_height);

  return;
}

static int egl_init(struct Render_Init_Info *paras) {
  if (paras->frame_width == 0 || paras->frame_height == 0 ||
      paras->screen_width == 0 || paras->screen_height == 0) {
    return -1;
  }
  
  int frame_width = paras->frame_width;
  int frame_height = paras->frame_height;
  window_info.frame_width = paras->frame_width;
  window_info.frame_height = paras->frame_height;
  egl_base.screen_width = paras->screen_width;
  egl_base.screen_height = paras->screen_height;
  bool is_full_screen = paras->is_full_screen;
  egl_base.display_buffer = paras->use_display_buffer;
  egl_base.fixed_resolution = paras->fixed_resolution;
  egl_base.fill_resolution = paras->fill_resolution;

  // glteximage2d needs the width that must to be a multiple of 64 because it has a 
  // minimum width of 64
  // also can use frame->linesize[0] to set width
  int widthMulti = isYUV444 ? 64 : 128;
  if (egl_render.decoder_type == SOFTWARE) {
    if (frame_width % widthMulti != 0)
      egl_base.width = ((int)(frame_width / widthMulti)) * widthMulti + widthMulti;
    else
      egl_base.width = frame_width;
    egl_base.height = frame_height;
  }
  else {
    egl_base.width = frame_width;
    egl_base.height = frame_height;
  }

  // finally we can create a new surface using this config and window
  if (!egl_base.display_buffer || (egl_base.display_buffer && !ExtState.eglIsSupportExtSurfaceless)) {
    surface = eglCreatePlatformWindowSurface(display, config, paras->window, NULL);
    if (surface == EGL_NO_SURFACE) {
      fprintf(stderr, "EGL: couldn't get a valid egl surface\n");
      return -1;
    }
  }

  eglMakeCurrent(display, surface, surface, context);

  if (egl_base.display_buffer) {
    if (glEGLImageTargetTexture2DOES == NULL) {
      fprintf(stderr, "EGL: extension glEGLImageTargetTexture2DOES not found\n");
      return -1;
    }
    if (map_display_buffer_framebuffer(out_fb, paras->display_exported_buffer) < 0) {
      fprintf(stderr, "EGL: map_gbm_bo_to_framebuffer failed\n");
      return -1;
    }
  }

  glEnable(GL_TEXTURE_2D);
  
  if (generate_shader(&egl_base.common_vertex_shader, vertex_source, GL_VERTEX_SHADER) < 0 ||
      generate_shader(&egl_base.yuv_fragment_shader, fragment_source_3plane, GL_FRAGMENT_SHADER) < 0 ||
      generate_shader(&egl_base.nv12_fragment_shader, fragment_source_nv12, GL_FRAGMENT_SHADER) < 0 ||
      generate_shader(&egl_base.packed_fragment_shader, fragment_source_packed, GL_FRAGMENT_SHADER) < 0)
    return -1;

  egl_base.shader_program_yuv = glCreateProgram();
  egl_base.shader_program_packed = glCreateProgram();
  egl_base.shader_program_nv12 = glCreateProgram();

  glAttachShader(egl_base.shader_program_yuv, egl_base.common_vertex_shader);
  glAttachShader(egl_base.shader_program_packed, egl_base.common_vertex_shader);
  glAttachShader(egl_base.shader_program_nv12, egl_base.common_vertex_shader);
  glAttachShader(egl_base.shader_program_packed, egl_base.packed_fragment_shader);
  glAttachShader(egl_base.shader_program_yuv, egl_base.yuv_fragment_shader);
  glAttachShader(egl_base.shader_program_nv12, egl_base.nv12_fragment_shader);
  glLinkProgram(egl_base.shader_program_yuv);
  glLinkProgram(egl_base.shader_program_packed);
  glLinkProgram(egl_base.shader_program_nv12);

  glGenVertexArrays(1, &egl_base.VAO);
  glBindVertexArray(egl_base.VAO);

  GLuint vbo;
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  GLuint ebo;
  glGenBuffers(1, &ebo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);

  if (!egl_base.display_buffer) {
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  }
  else {
    glBufferData(GL_ARRAY_BUFFER, sizeof(buffer_vertices), buffer_vertices, GL_STATIC_DRAW);
  }
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(elements), elements, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof (float)));
  glEnableVertexAttribArray(1);
  glBindBuffer(GL_ARRAY_BUFFER,0);
  glBindVertexArray(0);
  glDeleteBuffers(1,&vbo);
  glDeleteBuffers(1,&ebo);

  texture_uniform[FRAG_PARAM_YUVMAT] = glGetUniformLocation(egl_base.shader_program_yuv, "yuvmat");
  texture_uniform[FRAG_PARAM_OFFSET] = glGetUniformLocation(egl_base.shader_program_yuv, "offset");
  texture_uniform[FRAG_PARAM_PLANE0] = glGetUniformLocation(egl_base.shader_program_yuv, "ymap");
  texture_uniform[FRAG_PARAM_PLANE1] = glGetUniformLocation(egl_base.shader_program_yuv, "umap");
  texture_uniform[FRAG_PARAM_PLANE2] = glGetUniformLocation(egl_base.shader_program_yuv, "vmap");
  texture_uniform[FRAG_PARAM_CUTWIDTH] = glGetUniformLocation(egl_base.shader_program_yuv, "cutwidth");
  texture_uniform[FRAG_PARAM_YUVMAT] = glGetUniformLocation(egl_base.shader_program_packed, "yuvmat");
  texture_uniform[FRAG_PARAM_OFFSET] = glGetUniformLocation(egl_base.shader_program_packed, "offset");
  texture_uniform[FRAG_PARAM_YUVORDER] = glGetUniformLocation(egl_base.shader_program_packed, "yuvorder");
  texture_uniform[FRAG_PARAM_PLANE0] = glGetUniformLocation(egl_base.shader_program_packed, "packedmap");
  texture_uniform[FRAG_PARAM_YUVMAT] = glGetUniformLocation(egl_base.shader_program_nv12, "yuvmat");
  texture_uniform[FRAG_PARAM_OFFSET] = glGetUniformLocation(egl_base.shader_program_nv12, "offset");
  texture_uniform[FRAG_PARAM_PLANE0] = glGetUniformLocation(egl_base.shader_program_nv12, "plane1");
  texture_uniform[FRAG_PARAM_PLANE1] = glGetUniformLocation(egl_base.shader_program_nv12, "plane2");

  glGenTextures(egl_max_planes, texture_id);
  for (int i = 0; i < egl_max_planes; i++) {
      glBindTexture(GL_TEXTURE_2D, texture_id[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      if (egl_render.decoder_type == SOFTWARE) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, (i > 0 && !isYUV444) ? egl_base.width / 2 : egl_base.width, (i > 0 && !isYUV444) ? egl_base.height / 2 : egl_base.height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0);
      }
  }

  // for software render ,because we want cut the not needed view range
  egl_base.cutwidth = egl_render.decoder_type != SOFTWARE ? 1 : ((float)frame_width / egl_base.width - ((isYUV444 || frame_width == egl_base.width) ? 0 : 0.0002));
  egl_sync_window_size(egl_base.width, egl_base.height, is_full_screen);

  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return 0;
}

static inline void draw_texture() {
  // Bind parameters for the shaders
  glUniformMatrix3fv(texture_uniform[FRAG_PARAM_YUVMAT], 1, GL_FALSE, colorspace);
  glUniform3fv(texture_uniform[FRAG_PARAM_OFFSET], 1, colorOffsets);
  glUniform4iv(texture_uniform[FRAG_PARAM_YUVORDER], 1, yuvOrder);
  glUniform1i(texture_uniform[FRAG_PARAM_PLANE0], 0);
  if (planeNum >= 2)
    glUniform1i(texture_uniform[FRAG_PARAM_PLANE1], 1);
  if (planeNum >= 3) {
    glUniform1i(texture_uniform[FRAG_PARAM_PLANE2], 2);
    glUniform1f(texture_uniform[FRAG_PARAM_CUTWIDTH], egl_base.cutwidth);
  }

  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

static inline void egl_draw_soft(uint8_t* image[3]) {
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, texture_id[i]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (i > 0 && !isYUV444) ? egl_base.width / 2 : egl_base.width, (i > 0 && !isYUV444) ? egl_base.height / 2 : egl_base.height, GL_LUMINANCE, GL_UNSIGNED_BYTE, image[i]);
  }

  draw_texture();
}

static inline void egl_draw_vaapi(EGLImage image[4]) {
#ifdef HAVE_VAAPI
  if (imageNum < 0) {
      printf("Error in egl_drawing frame\n");
      return;
  }
  
  for (int i = 0; i < imageNum; i++) {
     glActiveTexture(GL_TEXTURE0 + i);
     glBindTexture(GL_TEXTURE_2D, texture_id[i]);
     glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image[i]);
     //glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, image[i], NULL);
  }

  draw_texture();
#endif
}

static int egl_choose_config_from_frame(struct Render_Config *config) {
  eglMakeCurrent(display, surface, surface, context);

  egl_base.eglVSync = config->vsync;
  eglSwapInterval(display, 0);

  bool fullRange = false;
  if (config->full_color_range) {
    fullRange = true;
    colorOffsets = fullOffsets;
  } else {
    colorOffsets = limitedOffsets;
  }
  switch (config->color_space) {
  case COLORSPACE_REC_601:
    colorspace = fullRange ? bt601Full : bt601Lim;
    break;
  case COLORSPACE_REC_709:
    colorspace = fullRange ? bt709Full : bt709Lim;
    break;
  case COLORSPACE_REC_2020:
    colorspace = fullRange ? bt2020Full : bt2020Lim;
    break;
  default:
    colorspace = bt601Lim;
    break;
  }

  if (egl_render.decoder_type != SOFTWARE) {
    planeNum = config->plane_nums;
    yuvOrder = plane_order[config->yuv_order];
    imageNum = config->image_nums == 0 ? planeNum : config->image_nums;
  }
  else {
    // only x11 platform
    yuvOrder = plane_order[YUVX_ORDER]; 
    planeNum = 3;
  }

  return 0;
}

static int egl_draw(struct Render_Image *images) {
  if (eglsync != EGL_NO_SYNC)
    eglDestroySync(display, eglsync);

  if (egl_base.display_buffer) {
    egl_base.back_out_fb = &out_fb[images->index];
    glBindFramebuffer(GL_FRAMEBUFFER, egl_base.back_out_fb->framebuffer);
  }

  glClear(GL_COLOR_BUFFER_BIT);

  glUseProgram(*shaders[planeNum]);
  glBindVertexArray(egl_base.VAO);

  if (egl_render.decoder_type != SOFTWARE) {
    egl_draw_vaapi(images->images.image_data);
  }
  else {
    egl_draw_soft(images->sframe.frame_data);
  }

  glBindVertexArray(0);

  eglsync = eglCreateSync(display, EGL_SYNC_FENCE, NULL);

  if (!egl_base.display_buffer) {
    eglSwapBuffers(display, surface);
  }
  else {
    glFlush();
  }

  if (egl_base.eglVSync) {
    if (eglsync != EGL_NO_SYNC) {
      // wait 5 second,will exit when expired
      if (eglClientWaitSync(display, eglsync, EGL_SYNC_FLUSH_COMMANDS_BIT, 5000000000) == EGL_TIMEOUT_EXPIRED) {
        return -1;
      }
    }
  }

  return images->index;
}

static void egl_destroy() {
  if (!egl_base.eglVSync) {
    if (eglsync != EGL_NO_SYNC) {
      eglClientWaitSync(display, eglsync, EGL_SYNC_FLUSH_COMMANDS_BIT, EGL_FOREVER);
    }
  }
  if (eglsync != EGL_NO_SYNC)
    eglDestroySync(display, eglsync);
  if (egl_base.width != 0) {
    eglMakeCurrent(display, surface, surface, context);
    if (egl_base.display_buffer) {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      for (int i = 0; i < egl_base.displayBufferNum; i++) {
        glDeleteFramebuffers(1, &out_fb[i].framebuffer);
        for (int j = 0; j < egl_base.displayBufferPlaneNum; j++) {
            glDeleteTextures(1, &out_fb[i].texture[j]);
            eglDestroyImage(display, out_fb[i].image[j]);
        }
      }
    }
    glDeleteTextures(egl_max_planes, texture_id);
    glBindVertexArray(0);
    glDeleteBuffers(1,&egl_base.VAO);
    glDeleteShader(egl_base.common_vertex_shader);
    glDeleteShader(egl_base.packed_fragment_shader);
    glDeleteShader(egl_base.nv12_fragment_shader);
    glDeleteShader(egl_base.yuv_fragment_shader);
    glDeleteProgram(egl_base.shader_program_packed);
    glDeleteProgram(egl_base.shader_program_nv12);
    glDeleteProgram(egl_base.shader_program_yuv);
    glFinish();
    memset(&egl_base, 0, sizeof(egl_base));
  }

  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (surface != EGL_NO_SURFACE)
    eglDestroySurface(display, surface);
  if (context != EGL_NO_CONTEXT)
    eglDestroyContext(display, context);
  if (display != EGL_NO_DISPLAY)
    eglTerminate(display);
  display = EGL_NO_DISPLAY;
  surface = EGL_NO_SURFACE;
  context = EGL_NO_CONTEXT;
  memset(out_fb, 0, sizeof(out_fb));
  egl_render.is_hardaccel_support = false;
  return;
}

struct RENDER_CALLBACK egl_render = {
  .name = "egl",
  .display_name = NULL,
  .is_hardaccel_support = false,
  .render_type = EGL_RENDER,
  .decoder_type = SOFTWARE,
  .data = NULL,
  .render_create = egl_create,
  .render_init = egl_init,
  .render_sync_config = egl_choose_config_from_frame,
  .render_draw = egl_draw,
  .render_destroy = egl_destroy,
  .render_map_buffer = egl_map_buffer_to_eglimage,
  .render_unmap_buffer = egl_unmap_eglimage,
  .render_sync_window_size = egl_sync_window_size,
};
