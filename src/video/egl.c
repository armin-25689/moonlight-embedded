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
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <libavcodec/avcodec.h>

#include <Limelight.h>

#include "egl.h"
#include "ffmpeg.h"
#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi_egl.h"
#endif
// include glsl
#include "egl_glsl.h"

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

bool isUseGlExt = false;

static struct EXTSTATE {
  bool eglIsSupportExtDmaBuf;
  bool eglIsSupportExtDmaBufMod;
  bool eglIsSupportExtKHR;
  bool eglIsSupportImageOES;
} ExtState;

static const EGLint context_attributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0,  EGL_NONE };
//static const EGLint context_attributes[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 0,  EGL_NONE };
static EGLDisplay display;
static EGLSurface surface;
static EGLContext context;

static float cutwidth;
static int width, height;
static bool current;

static GLuint VAO;
static GLuint texture_id[4], texture_uniform[7];
static GLuint shader_program_packed, shader_program_nv12, shader_program_yuv;
// for planeNum 0, 1, 2, 3, 4
static GLuint *shaders[5] = { &shader_program_nv12, &shader_program_packed, &shader_program_nv12, &shader_program_yuv, &shader_program_nv12 };

static const float *colorOffsets;
static const float *colorspace;
static int egl_max_planes = 4;
static int planeNum = 4;
static const int *yuvOrder;
// initialize differet format for soft_pix_fmt and last_pix_fmt
static enum AVPixelFormat soft_pix_fmt = AV_PIX_FMT_YUV420P;
static enum AVPixelFormat last_pix_fmt = AV_PIX_FMT_NONE;
static enum AVPixelFormat *sharedFmt = &soft_pix_fmt;

static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
static PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC glEGLImageTargetTexStorageEXT;
// (GLenum target, GLeglImageOES image, const GLint* attrib_list);

static void choose_color_config(const AVFrame* frame)
{
  bool fullRange = false;
  if (ffmpeg_is_frame_full_range(frame)) {
    fullRange = true;
    colorOffsets = fullOffsets;
  } else {
    colorOffsets = limitedOffsets;
  }
  switch (ffmpeg_get_frame_colorspace(frame)) {
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
#ifdef HAVE_VAAPI
  if (ffmpeg_decoder != SOFTWARE) {
    enum PixelFormatOrder planeOrder;
    vaapi_get_plane_info(&sharedFmt, &planeNum, &planeOrder);
    yuvOrder = plane_order[planeOrder];
    return;
  }
#endif
  // only x11 platform
  *sharedFmt = isYUV444 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P;
  yuvOrder = plane_order[YUVX_ORDER]; 
  planeNum = 3;
  return;
}

static bool is_gl_extension_support(const char *extensionname) {
  char firstChar = *extensionname;
  const char *allExtensions = NULL;

  if (firstChar == 'E') {
    allExtensions = eglQueryString(display, EGL_EXTENSIONS);
    int len = strlen(allExtensions);
    char splitstring[len + 1];
    strcpy(splitstring, allExtensions);
    char *token = strtok(splitstring, " ");
    while (token != NULL) {
      if (strcmp(extensionname, token) == 0) {
        token = strtok(token, " ");
        allExtensions = NULL;
        return true;
      }
      token = strtok(NULL, " ");
    }
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

  // gl_oes_egl_image require egl_khr_image_base or egl_khr_image,and needed for glegliamgetargettexture2does function
  if (!is_gl_extension_support("GL_OES_EGL_image")) {
    fprintf(stderr, "EGL: could not support rendering by egl because of no GL_OES_EGL_image extensions\n" );
    return false;
  }
  else {
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (glEGLImageTargetTexture2DOES == NULL) {
      return false;
    }
  }

  if (!is_gl_extension_support("GL_EXT_EGL_image_storage")) {
    fprintf(stderr, "EGL: could not support rendering by egl because of no GL_EXT_EGL_image_storage extensions\n" );
  }
  else {
    glEGLImageTargetTexStorageEXT = (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)eglGetProcAddress("glEGLImageTargetTexStorageEXT");
  }
  
  // For vaapi-EGL
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

void egl_init(void *native_display, void *native_window, int frame_width, int frame_height, int screen_width, int screen_height, int dcFlag) {
  // glteximage2d needs the width that must to be a multiple of 64 because it has a 
  // minimum width of 64
  // also can use frame->linesize[0] to set width
  int widthMulti = isYUV444 ? 64 : 128;
  if (frame_width % widthMulti != 0)
    width = ((int)(frame_width / widthMulti)) * widthMulti + widthMulti;
  else
    width = frame_width;
  height = frame_height;
  current = false;
  #define FULLSCREEN 0x08
  bool isFullScreen = dcFlag & FULLSCREEN ? true : false;
  #undef FULLSCREEN

  // get an EGL display connection
  PFNEGLGETPLATFORMDISPLAYPROC eglGetPlatformDisplayProc;
  eglGetPlatformDisplayProc = (typeof(eglGetPlatformDisplayProc)) eglGetProcAddress("eglGetPlatformDisplay");
  if (eglGetPlatformDisplayProc != NULL) {
    int platformFlag = windowType & WAYLAND_WINDOW ? EGL_PLATFORM_WAYLAND_KHR : (windowType & GBM_WINDOW ? EGL_PLATFORM_GBM_KHR : EGL_PLATFORM_X11_KHR);
    display = eglGetPlatformDisplayProc(platformFlag, native_display, NULL); //EGL_PLATFORM_X11_KHR for x11;EGL_PLATFORM_GBM_KHR for drm
  }
  if (display == EGL_NO_DISPLAY) {
    fprintf( stderr, "EGL: error get display\n" );
    exit(EXIT_FAILURE);
  }

  // initialize the EGL display connection
  int major, minor;
  EGLBoolean result = eglInitialize(display, &major, &minor);
  if (result == EGL_FALSE) {
    fprintf( stderr, "EGL: error initialising display\n");
    exit(EXIT_FAILURE);
  }

  // get our config from the config class
  EGLConfig config = NULL;
  const EGLint *attribute_list;
  //static const EGLint attribute_list_8bit[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_DEPTH_SIZE, 24, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER, EGL_NONE };
  static const EGLint attribute_list_8bit[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_DEPTH_SIZE, 24, EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER, EGL_NONE };
  //static const EGLint attribute_list_10bit[] = { EGL_RED_SIZE, 10, EGL_GREEN_SIZE, 10, EGL_BLUE_SIZE, 10, EGL_ALPHA_SIZE, 2, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_DEPTH_SIZE, 32, EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER, EGL_NONE };
  attribute_list = attribute_list_8bit;

  EGLint totalConfigsFound = 0;
  result = eglChooseConfig(display, attribute_list, &config, 1, &totalConfigsFound);
  if (result != EGL_TRUE || totalConfigsFound == 0) {
    fprintf(stderr, "EGL: Unable to query for available configs, found %d.\n", totalConfigsFound);
    exit(EXIT_FAILURE);
  }

  // bind the OpenGL API to the EGL
  result = eglBindAPI(EGL_OPENGL_ES_API);
  if (result == EGL_FALSE) {
    fprintf(stderr, "EGL: error binding API\n");
    exit(EXIT_FAILURE);
  }

  // create an EGL rendering context
  context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
  if (context == EGL_NO_CONTEXT) {
    fprintf(stderr, "EGL: couldn't get a valid context\n");
    exit(EXIT_FAILURE);
  }

  // test egl and opengl es version
  double egl_version = atof(eglQueryString(display, EGL_VERSION));
  GLint gles_version;
  eglQueryContext(display, context, EGL_CONTEXT_CLIENT_VERSION, &gles_version);
  if (egl_version < 1.5 || (ffmpeg_decoder != SOFTWARE && gles_version < 3)) {
    fprintf(stderr, "EGL: couldn't initialize egl and opengl es context because of egl version(%s) and gles version(%d)\n", eglQueryString(display, EGL_VERSION), gles_version);
    exit (-1);
  }

  // finally we can create a new surface using this config and window
  surface = eglCreatePlatformWindowSurface(display, config, native_window, NULL);
  if (surface == EGL_NO_SURFACE) {
    fprintf(stderr, "EGL: couldn't get a valid egl surface\n");
    exit (EXIT_FAILURE);
  }

  eglMakeCurrent(display, surface, surface, context);

  // test render resolution is supported
  int maxTextureSize;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
  if (width > maxTextureSize || height > maxTextureSize) {
    fprintf(stderr, "EGL: couldn't specified resolusion because of max resolution %dx%d\n", maxTextureSize, maxTextureSize);
    exit (-1);
  }

  // try to point if can use extensions for egl
  if (ffmpeg_decoder != SOFTWARE && is_use_ext(&ExtState)) {
    isUseGlExt = true;
  }

#ifdef HAVE_WAYLAND
  // for wayland
  if (windowType & WAYLAND_WINDOW)
    eglSwapInterval(display, 0);
#endif

  glEnable(GL_TEXTURE_2D);
  
  GLuint common_vertex_shader;
  GLuint yuv_fragment_shader,nv12_fragment_shader,packed_fragment_shader;
  if (generate_shader(&common_vertex_shader, vertex_source, GL_VERTEX_SHADER) < 0 ||
      generate_shader(&yuv_fragment_shader, fragment_source_3plane, GL_FRAGMENT_SHADER) < 0 ||
      generate_shader(&nv12_fragment_shader, fragment_source_nv12, GL_FRAGMENT_SHADER) < 0 ||
      generate_shader(&packed_fragment_shader, fragment_source_packed, GL_FRAGMENT_SHADER) < 0)
    exit(-1);

  shader_program_yuv = glCreateProgram();
  shader_program_packed = glCreateProgram();
  shader_program_nv12 = glCreateProgram();

  glAttachShader(shader_program_yuv, common_vertex_shader);
  glAttachShader(shader_program_packed, common_vertex_shader);
  glAttachShader(shader_program_nv12, common_vertex_shader);
  glAttachShader(shader_program_packed, packed_fragment_shader);
  glAttachShader(shader_program_yuv, yuv_fragment_shader);
  glAttachShader(shader_program_nv12, nv12_fragment_shader);
  glLinkProgram(shader_program_yuv);
  glLinkProgram(shader_program_packed);
  glLinkProgram(shader_program_nv12);

  glGenVertexArrays(1, &VAO);
  glBindVertexArray(VAO);

  GLuint vbo;
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);

  GLuint ebo;
  glGenBuffers(1, &ebo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);

  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(elements), elements, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof (float)));
  glEnableVertexAttribArray(1);
  glBindBuffer(GL_ARRAY_BUFFER,0);
  glBindVertexArray(0);
  glDeleteBuffers(1,&vbo);
  glDeleteBuffers(1,&ebo);

  texture_uniform[FRAG_PARAM_YUVMAT] = glGetUniformLocation(shader_program_yuv, "yuvmat");
  texture_uniform[FRAG_PARAM_OFFSET] = glGetUniformLocation(shader_program_yuv, "offset");
  texture_uniform[FRAG_PARAM_PLANE0] = glGetUniformLocation(shader_program_yuv, "ymap");
  texture_uniform[FRAG_PARAM_PLANE1] = glGetUniformLocation(shader_program_yuv, "umap");
  texture_uniform[FRAG_PARAM_PLANE2] = glGetUniformLocation(shader_program_yuv, "vmap");
  texture_uniform[FRAG_PARAM_CUTWIDTH] = glGetUniformLocation(shader_program_yuv, "cutwidth");
  texture_uniform[FRAG_PARAM_YUVMAT] = glGetUniformLocation(shader_program_packed, "yuvmat");
  texture_uniform[FRAG_PARAM_OFFSET] = glGetUniformLocation(shader_program_packed, "offset");
  texture_uniform[FRAG_PARAM_YUVORDER] = glGetUniformLocation(shader_program_packed, "yuvorder");
  texture_uniform[FRAG_PARAM_PLANE0] = glGetUniformLocation(shader_program_packed, "packedmap");
  texture_uniform[FRAG_PARAM_YUVMAT] = glGetUniformLocation(shader_program_nv12, "yuvmat");
  texture_uniform[FRAG_PARAM_OFFSET] = glGetUniformLocation(shader_program_nv12, "offset");
  texture_uniform[FRAG_PARAM_PLANE0] = glGetUniformLocation(shader_program_nv12, "plane1");
  texture_uniform[FRAG_PARAM_PLANE1] = glGetUniformLocation(shader_program_nv12, "plane2");

  glGenTextures(egl_max_planes, texture_id);
  for (int i = 0; i < egl_max_planes; i++) {
      glBindTexture(GL_TEXTURE_2D, texture_id[i]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      if (ffmpeg_decoder == SOFTWARE) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, (i > 0 && !isYUV444) ? width / 2 : width, (i > 0 && !isYUV444) ? height / 2 : height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0);
      }
  }

  // for software render ,because we want cut the not needed view range
  cutwidth = ffmpeg_decoder != SOFTWARE ? 1 : ((float)frame_width / width - ((isYUV444 || frame_width == width) ? 0 : 0.0002));
  if (ffmpeg_decoder == SOFTWARE && frame_width != width) {
    // cut display area,importent for software decoder
    if (isFullScreen)
      glViewport(0, 0, (int)((screen_width / (float)frame_width) * width), screen_height);
    else
      glViewport(0, 0, width, height);
  }

  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
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
    glUniform1f(texture_uniform[FRAG_PARAM_CUTWIDTH], cutwidth);
  }

  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
  glBindVertexArray(0);

  eglSwapBuffers(display, surface);
}

static inline void egl_draw_soft(AVFrame* frame, uint8_t* image[3]) {

  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, texture_id[i]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (i > 0 && !isYUV444) ? width / 2 : width, (i > 0 && !isYUV444) ? height / 2 : height, GL_LUMINANCE, GL_UNSIGNED_BYTE, image[i]);
  }

  draw_texture();
}

static inline void egl_draw_vaapi(AVFrame* frame) {
#ifdef HAVE_VAAPI
  EGLImage image[4];

  ssize_t plane_count = vaapi_export_egl_images(frame, display,
                                                ExtState.eglIsSupportExtDmaBufMod, image);
  if (plane_count < 0) {
      printf("Error in egl_drawing frame\n");
      return;
  }
  
  for (int i = 0; i < plane_count; i++) {
     glActiveTexture(GL_TEXTURE0 + i);
     glBindTexture(GL_TEXTURE_2D, texture_id[i]);
     glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image[i]);
     //glEGLImageTargetTexStorageEXT(GL_TEXTURE_2D, image[i], NULL);
  }

  draw_texture();

  vaapi_free_egl_images(display, image);
#endif
}

void egl_draw(AVFrame* frame) {
  if (last_pix_fmt != *sharedFmt) {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    choose_color_config(frame);
    last_pix_fmt = *sharedFmt;
    eglMakeCurrent(display, surface, surface, context);
  }

  glUseProgram(*shaders[planeNum]);
  glBindVertexArray(VAO);

  if (ffmpeg_decoder != SOFTWARE) {
    egl_draw_vaapi(frame);
  }
  else {
    egl_draw_soft(frame, frame->data);
  }
  return;
}

void egl_destroy() {
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(display, surface);
  eglDestroyContext(display, context);
  eglTerminate(display);
}
