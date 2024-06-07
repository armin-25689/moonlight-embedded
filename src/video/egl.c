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
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif
#ifdef HAVE_WAYLAND
#include "wayland.h"
#endif
#include "x11.h"

#include <Limelight.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_X11_KHR
#define EGL_PLATFORM_X11_KHR 0x31D5
#endif
#ifndef EGL_PLATFORM_WAYLAND_KHR
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#endif
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#define SOFTWARE 0
#define FRAG_PARAM_YUVMAT 0
#define FRAG_PARAM_OFFSET 1
#define FRAG_PARAM_YUVORDER 2
#define FRAG_PARAM_PLANE0 3
#define FRAG_PARAM_PLANE1 4
#define FRAG_PARAM_PLANE2 5
#define FRAG_PARAM_CUTWIDTH 6

bool isUseGlExt = false;
#ifndef HAVE_VAAPI
static bool isYUV444 = false;
static enum AVPixelFormat sharedFmt = AV_PIX_FMT_NONE;
#endif
static bool isWayland = false;
static const int egl_max_planes = 4;
static enum AVPixelFormat last_pix_fmt = AV_PIX_FMT_NONE;

static struct EXTSTATE {
  bool eglIsSupportExtDmaBuf;
  bool eglIsSupportExtDmaBufMod;
  bool eglIsSupportExtKHR;
  bool eglIsSupportImageOES;
  bool eglIsSupportCreateImage;
} ExtState;

static const EGLint context_attributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0,  EGL_NONE };

static const char* vertex_source = {
"#version 300 es\n"
"\n"
"layout (location = 0) in vec2 position;\n"
"layout (location = 1) in vec2 aTexCoord;\n"
"out vec2 tex_position;\n"
"\n"
"void main() {\n"
"  gl_Position = vec4(position, 0, 1);\n"
"  tex_position = aTexCoord;\n\n"
"}\n"
};

static const char* fragment_source_3plane = {
"#version 300 es\n"
"precision mediump float;\n"
"out vec4 FragColor;\n"
"\n"
"in vec2 tex_position;\n"
"\n"
"uniform mat3 yuvmat;\n"
"uniform vec3 offset;\n"
"uniform ivec4 yuvorder;\n"
"uniform sampler2D ymap;\n"
"uniform sampler2D umap;\n"
"uniform sampler2D vmap;\n"
"uniform float cutwidth;\n"
"\n"
"void main() {\n"
"  if (tex_position.x > cutwidth) {\n"
"    FragColor = vec4(0.0f,0.0f,0.0f,1.0f);\n"
"    return;\n"
"  }\n"
"  vec3 YCbCr = vec3(\n"
"    texture2D(ymap, tex_position).r,\n"
"    texture2D(umap, tex_position).r,\n"
"    texture2D(vmap, tex_position).r\n"
"  );\n"
"\n"
"  YCbCr -= offset;\n"
"  FragColor = vec4(clamp(yuvmat * YCbCr, 0.0, 1.0), 1.0f);\n"
"}\n"
};

static const char* fragment_source_nv12 = {
"#version 300 es\n"
"precision mediump float;\n"
"out vec4 FragColor;\n"
"\n"
"in vec2 tex_position;\n"
"\n"
"uniform mat3 yuvmat;\n"
"uniform vec3 offset;\n"
"uniform ivec4 yuvorder;\n"
"uniform sampler2D plane1;\n"
"uniform sampler2D plane2;\n"
"\n"
"void main() {\n"
"	vec3 YCbCr = vec3(\n"
"		texture2D(plane1, tex_position)[0],\n"
"		texture2D(plane2, tex_position).xy\n"
"	);\n"
"\n"
"	YCbCr -= offset;\n"
"	FragColor = vec4(clamp(yuvmat * YCbCr, 0.0, 1.0), 1.0f);\n"
"}\n"
};

static const char* fragment_source_packed = {
"#version 300 es\n"
"precision mediump float;\n"
"out vec4 FragColor;\n"
"\n"
"in vec2 tex_position;\n"
"\n"
"uniform mat3 yuvmat;\n"
"uniform vec3 offset;\n"
"uniform ivec4 yuvorder;\n"
"uniform sampler2D vuyamap;\n"
"\n"
"void main() {\n"
"  vec4 vuya = texture2D(vuyamap, tex_position);\n"
"  vec3 YCbCr = vec3(vuya[yuvorder[0]], vuya[yuvorder[1]], vuya[yuvorder[2]]);"
"\n"
"  YCbCr -= offset;\n"
"  FragColor = vec4(clamp(yuvmat * YCbCr, 0.0, 1.0), 1.0f);\n"
"}\n"
};

static const float vertices[] = {
 // pos .... // tex coords
 1.0f, 1.0f, 1.0f, 0.0f,
 1.0f, -1.0f, 1.0f, 1.0f,
 -1.0f, -1.0f, 0.0f, 1.0f,
 -1.0f, 1.0f, 0.0f, 0.0f,
};

static const GLuint elements[] = {
  0, 1, 2,
  2, 3, 0
};

static const float limitedOffsets[] = { 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f };
static const float fullOffsets[] = { 0.0f, 128.0f / 255.0f, 128.0f / 255.0f };
static const float bt601Lim[] = {
  1.1644f, 1.1644f, 1.1644f,
  0.0f, -0.3917f, 2.0172f,
  1.5960f, -0.8129f, 0.0f
};
static const float bt601Full[] = {
    1.0f, 1.0f, 1.0f,
    0.0f, -0.3441f, 1.7720f,
    1.4020f, -0.7141f, 0.0f
};
static const float bt709Lim[] = {
  1.1644f, 1.1644f, 1.1644f,
  0.0f, -0.2132f, 2.1124f,
  1.7927f, -0.5329f, 0.0f
};
static const float bt709Full[] = {
  1.0f, 1.0f, 1.0f,
  0.0f, -0.1873f, 1.8556f,
  1.5748f, -0.4681f, 0.0f
};
static const float bt2020Lim[] = {
  1.1644f, 1.1644f, 1.1644f,
  0.0f, -0.1874f, 2.1418f,
  1.6781f, -0.6505f, 0.0f
};
static const float bt2020Full[] = {
  1.0f, 1.0f, 1.0f,
  0.0f, -0.1646f, 1.8814f,
  1.4746f, -0.5714f, 0.0f
};
static const int vuyx[] = { 2, 1, 0, 3 };
// xv30 xv36
static const int xvyu[] = { 1, 0, 2, 3 };
static const int yuvx[] = { 0, 1, 2, 3 };

static EGLDisplay display;
static EGLSurface surface;
static EGLContext context;

static float cutwidth;
static int width, height;
static bool current;

static GLuint VAO;
static GLuint texture_id[4], texture_uniform[7];
static GLuint shader_program_yuv,shader_program_packed,shader_program_nv12;

static const float *colorOffsets;
static const float *colorspace;
static const int *yuvOrder;
static int planeNum = 4;

static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

static void chooseColorConfig(const AVFrame* frame)
{
  bool fullRange = false;
#ifdef HAVE_VAAPI
  if (isFrameFullRange(frame)) {
    fullRange = true;
    colorOffsets = fullOffsets;
  } else {
#endif
    colorOffsets = limitedOffsets;
#ifdef HAVE_VAAPI
  }
  switch (getFrameColorspace(frame)) {
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
  planeNum = get_plane_number(sharedFmt);
  const char *yuvorderchar = get_yuv_order(-1);
  if (strcmp("yuvx",yuvorderchar) == 0 ||
      strcmp("yuva",yuvorderchar) == 0 ||
      strcmp("ayuv",yuvorderchar) == 0) // yuv order
    yuvOrder = yuvx; 
  else if (strcmp("vuyx",yuvorderchar) == 0 ||
           strcmp("vuya",yuvorderchar) == 0) {// vuyx
    yuvOrder = vuyx; 
  }
  else if (strcmp("xvyu",yuvorderchar) == 0 ||
           strcmp("avyu",yuvorderchar) == 0) // xv30
    yuvOrder = xvyu; 
  else
    yuvOrder = yuvx; 
#else
  // default config
  colorspace = bt601Lim;
  planeNum = 3; 
  yuvOrder = yuvx; 
#endif
}

static bool isGlExtensionSupport(const char *extensionname) {
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
        if (glExtensionName != NULL)
          if (strcmp(extensionname, glExtensionName) == 0)
            return true;
      }
    }
  }

  return false;
}

static bool isUseExt(struct EXTSTATE *extState) {
  if (isGlExtensionSupport("EGL_KHR_image_base") || isGlExtensionSupport("EGL_KHR_image")) {
    extState->eglIsSupportExtKHR = true;
  }

  if (isGlExtensionSupport("GL_OES_EGL_image")) {
    extState->eglIsSupportImageOES = true;
  }

  glEGLImageTargetTexture2DOES = (typeof(glEGLImageTargetTexture2DOES))eglGetProcAddress("glEGLImageTargetTexture2DOES");
  if (glEGLImageTargetTexture2DOES == NULL) {
    extState->eglIsSupportImageOES = false;
  }
  
  // For vaapi-EGL
  if (isGlExtensionSupport("EGL_EXT_image_dma_buf_import")) {
    extState->eglIsSupportExtDmaBuf = true;
  }

  if (isGlExtensionSupport("EGL_EXT_image_dma_buf_import_modifiers")) {
    extState->eglIsSupportExtDmaBufMod = true; 
  }

  if (eglGetProcAddress("eglCreateImage") == NULL) {
    extState->eglIsSupportCreateImage = false;
  } else {
    extState->eglIsSupportCreateImage = true;
  }

  if (extState->eglIsSupportExtDmaBuf && extState->eglIsSupportExtDmaBufMod &&
       extState->eglIsSupportExtKHR && extState->eglIsSupportImageOES &&
       extState->eglIsSupportCreateImage)
    return true;

  return false;
}

static int generate_vertex_shader(GLuint *vertex_shader, const char* vertex_source) {
  *vertex_shader = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(*vertex_shader, 1, &vertex_source, NULL);
  glCompileShader(*vertex_shader);
  // try to display compile errpr
  GLint status;
  char szLog[1024] = {0};
  GLsizei logLen = 0;
  glGetShaderiv(*vertex_shader,GL_COMPILE_STATUS,&status);
  glGetShaderInfoLog(*vertex_shader,1024,&logLen,szLog);
  if(!status) {
   printf("vertex_shader Compile error:%s\n", szLog);
   return -1;
  }
  return 0;
}
static int generate_fragment_shader(GLuint *fragment_shader, const char* fragment_source) {
  *fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(*fragment_shader, 1, &fragment_source, NULL);
  glCompileShader(*fragment_shader);
  // try to display compile errpr
  GLint status;
  char szLog[1024] = {0};
  GLsizei logLen = 0;
  glGetShaderiv(*fragment_shader,GL_COMPILE_STATUS,&status);
  glGetShaderInfoLog(*fragment_shader,1024,&logLen,szLog);
  if(!status) {
   printf("fragment_shader Compile error:%s\n", szLog);
   return -1;
  }
  return 0;
}

void egl_init(void *native_display, int frame_width, int frame_height, int screen_width, int screen_height, int dcFlag) {
  // glteximage2d needs the width that must to be a multiple of 64 because it has a 
  // minimum width of 64
  int widthMulti = isYUV444 ? 64 : 128;
  if (frame_width % widthMulti != 0)
    width = ((int)(frame_width / widthMulti)) * widthMulti + widthMulti;
  else
    width = frame_width;
  height = frame_height;
  int ffmpeg_decoder = dcFlag;
  current = false;
  #define FULLSCREEN 0x08
  bool isFullScreen = dcFlag & FULLSCREEN ? true : false;
  ffmpeg_decoder = isFullScreen ? (ffmpeg_decoder - FULLSCREEN) : ffmpeg_decoder;
  #undef FULLSCREEN

#ifdef HAVE_WAYLAND
  isWayland = (dcFlag & WAYLAND) == WAYLAND ? true : false;
  ffmpeg_decoder = isWayland ? (ffmpeg_decoder - WAYLAND) : ffmpeg_decoder;
#else
  isWayland = false;
#endif

  // get an EGL display connection
  PFNEGLGETPLATFORMDISPLAYPROC eglGetPlatformDisplayProc;
  eglGetPlatformDisplayProc = (typeof(eglGetPlatformDisplayProc)) eglGetProcAddress("eglGetPlatformDisplay");
  if (eglGetPlatformDisplayProc != NULL) {
    int platformFlag = isWayland ? EGL_PLATFORM_WAYLAND_KHR : EGL_PLATFORM_X11_KHR;
    display = eglGetPlatformDisplayProc(platformFlag, native_display, NULL); //EGL_PLATFORM_X11_KHR for x11;EGL_PLATFORM_GBM_KHR for drm
  }
  if (display == EGL_NO_DISPLAY) {
#ifdef HAVE_WAYLAND
    display = isWayland ? wl_get_egl_display() : x_get_egl_display();
#else
    display = x_get_egl_display();
#endif
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
  static const EGLint attribute_list_8bit[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_DEPTH_SIZE, 24, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER, EGL_NONE };
  //static const EGLint attribute_list_10bit[] = { EGL_RED_SIZE, 10, EGL_GREEN_SIZE, 10, EGL_BLUE_SIZE, 10, EGL_ALPHA_SIZE, 2, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_DEPTH_SIZE, 32, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER, EGL_NONE };
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

  // finally we can create a new surface using this config and window
#ifdef HAVE_WAYLAND
  surface = isWayland ? wl_get_egl_surface(display, config, NULL) : x_get_egl_surface(display, config, NULL);
#else
  surface = x_get_egl_surface(display, config, NULL);
#endif
  if (surface == EGL_NO_SURFACE) {
    fprintf(stderr, "EGL: couldn't get a valid egl surface\n");
    exit (EXIT_FAILURE);
  }

  eglMakeCurrent(display, surface, surface, context);

  // try to point if can use extensions for egl
  if (ffmpeg_decoder != SOFTWARE && isUseExt(&ExtState)) {
    isUseGlExt = true;
  }
  // test render resolution is supported
  int maxTextureSize;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
  if (width > maxTextureSize || height > maxTextureSize) {
    fprintf(stderr, "EGL: couldn't specified resolusion because of max resolution %dx%d\n", maxTextureSize, maxTextureSize);
    exit (-1);
  }

#ifdef HAVE_WAYLAND
  // for wayland
  if (isWayland)
    eglSwapInterval(display, 0);
#endif

  glEnable(GL_TEXTURE_2D);
  
  GLuint common_vertex_shader;
  GLuint yuv_fragment_shader,nv12_fragment_shader,packed_fragment_shader;
  if (generate_vertex_shader(&common_vertex_shader, vertex_source) < 0 ||
      generate_fragment_shader(&yuv_fragment_shader, fragment_source_3plane) < 0 ||
      generate_fragment_shader(&nv12_fragment_shader, fragment_source_nv12) < 0 ||
      generate_fragment_shader(&packed_fragment_shader, fragment_source_packed) < 0)
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
  cutwidth = (float)frame_width / width - ((isYUV444 || frame_width == width) ? 0 : 0.001);
  if (ffmpeg_decoder == SOFTWARE && frame_width != width) {
    if (isFullScreen)
      glViewport(0, 0, ((int)(screen_width / widthMulti)) * widthMulti + widthMulti, screen_height);
    else
      glViewport(0, 0, width, height);
      //glViewport(frame_width > width ? (int)((frame_width - width) / 2) : (int)((width - frame_width) / 2), 0, width, height);
  }

  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void egl_draw(AVFrame* frame, uint8_t* image[3]) {
  if (!current) {
    chooseColorConfig(frame);
    eglMakeCurrent(display, surface, surface, context);
    current = true;
  }

  glUseProgram(shader_program_yuv);
  glBindVertexArray(VAO);

  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, texture_id[i]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (i > 0 && !isYUV444) ? width / 2 : width, (i > 0 && !isYUV444) ? height / 2 : height, GL_LUMINANCE, GL_UNSIGNED_BYTE, image[i]);
  }

  glUniformMatrix3fv(texture_uniform[FRAG_PARAM_YUVMAT], 1, GL_FALSE, colorspace);
  glUniform3fv(texture_uniform[FRAG_PARAM_OFFSET], 1, colorOffsets);
  glUniform4iv(texture_uniform[FRAG_PARAM_YUVORDER], 1, yuvOrder);
  glUniform1i(texture_uniform[FRAG_PARAM_PLANE0], 0);
  glUniform1i(texture_uniform[FRAG_PARAM_PLANE1], 1);
  glUniform1i(texture_uniform[FRAG_PARAM_PLANE2], 2);
  glUniform1f(texture_uniform[FRAG_PARAM_CUTWIDTH], cutwidth);

  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
  glBindVertexArray(0);

  eglSwapBuffers(display, surface);
}

#ifdef HAVE_VAAPI
void egl_draw_frame(AVFrame* frame) {
  if (last_pix_fmt != sharedFmt) {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    chooseColorConfig(frame);
    last_pix_fmt = sharedFmt;
    eglMakeCurrent(display, surface, surface, context);
  }

  EGLImage image[4];

  bool twoplane = false,threeplane = false;
  switch (planeNum) {
  case 1:
    glUseProgram(shader_program_packed);
    break;
  case 2:
    glUseProgram(shader_program_nv12);
    twoplane = true;
    break;
  case 3:
    glUseProgram(shader_program_yuv);
    twoplane = true;
    threeplane = true;
    break;
  default:
    glUseProgram(shader_program_nv12);
    twoplane = true;
    break;
  }
  glBindVertexArray(VAO);

  ssize_t plane_count = exportEGLImages(frame, display, ExtState.eglIsSupportExtDmaBufMod,
                                        image);
  if (plane_count < 0) {
      printf("Error in egl_drawing frame\n");
      return;
  }
  
  for (int i = 0; i < plane_count; i++) {
     glActiveTexture(GL_TEXTURE0 + i);
     glBindTexture(GL_TEXTURE_2D, texture_id[i]);
     glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image[i]);
  }

  // Bind parameters for the shaders
  glUniformMatrix3fv(texture_uniform[FRAG_PARAM_YUVMAT], 1, GL_FALSE, colorspace);
  glUniform3fv(texture_uniform[FRAG_PARAM_OFFSET], 1, colorOffsets);
  glUniform4iv(texture_uniform[FRAG_PARAM_YUVORDER], 1, yuvOrder);
  glUniform1i(texture_uniform[FRAG_PARAM_PLANE0], 0);
  if (twoplane)
    glUniform1i(texture_uniform[FRAG_PARAM_PLANE1], 1);
  if (threeplane) {
    glUniform1i(texture_uniform[FRAG_PARAM_PLANE2], 2);
    glUniform1f(texture_uniform[FRAG_PARAM_CUTWIDTH], 1);
  }

  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
  glBindVertexArray(0);

  eglSwapBuffers(display, surface);

  freeEGLImages(display, image);
}
#endif

void egl_destroy() {
  eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(display, surface);
  eglDestroyContext(display, context);
  eglTerminate(display);
}
