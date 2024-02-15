--- src/video/egl.c.orig	2023-11-03 06:08:34 UTC
+++ src/video/egl.c
@@ -18,17 +18,51 @@
  */
 
 #include "egl.h"
+#include <GLES2/gl2.h>
+#include <GLES2/gl2ext.h>
 
+#include "ffmpeg_vaapi.h"
+#ifdef HAVE_WAYLAND
+#include "wayland.h"
+#endif
+#include "x11.h"
+
 #include <Limelight.h>
 
-#include <GLES2/gl2.h>
-
 #include <stdlib.h>
 #include <stdio.h>
 #include <stdbool.h>
+#include <string.h>
 #include <pthread.h>
 #include <unistd.h>
 
+#ifndef EGL_PLATFORM_X11_KHR
+#define EGL_PLATFORM_X11_KHR 0x31D5
+#endif
+#ifndef EGL_PLATFORM_WAYLAND_KHR
+#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
+#endif
+#ifndef EGL_PLATFORM_GBM_KHR
+#define EGL_PLATFORM_GBM_KHR 0x31D7
+#endif
+#define SOFTWARE 0
+#define NV12_PARAM_YUVMAT 0
+#define NV12_PARAM_OFFSET 1
+#define NV12_PARAM_PLANE1 2
+#define NV12_PARAM_PLANE2 3
+
+bool isUseGlExt = false;
+static bool isWayland = false;
+static bool isYUV444 = false;
+
+static struct EXTSTATE {
+  bool eglIsSupportExtDmaBuf;
+  bool eglIsSupportExtDmaBufMod;
+  bool eglIsSupportExtKHR;
+  bool eglIsSupportImageOES;
+  bool eglIsSupportCreateImage;
+} ExtState;
+
 static const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
 static const char* texture_mappings[] = { "ymap", "umap", "vmap" };
 static const char* vertex_source = "\
@@ -41,23 +75,43 @@ void main() {\
 }\
 ";
 
-static const char* fragment_source = "\
+static const char* fragment_source_420p = "\
 uniform lowp sampler2D ymap;\
 uniform lowp sampler2D umap;\
 uniform lowp sampler2D vmap;\
 varying mediump vec2 tex_position;\
 \
 void main() {\
-  mediump float y = texture2D(ymap, tex_position).r;\
-  mediump float u = texture2D(umap, tex_position).r - .5;\n\
-  mediump float v = texture2D(vmap, tex_position).r - .5;\n\
-  lowp float r = y + 1.28033 * v;\
-  lowp float g = y - .21482 * u - .38059 * v;\
-  lowp float b = y + 2.12798 * u;\
+  mediump float y = texture2D(ymap, tex_position).r - .0627;\
+  mediump float u = texture2D(umap, tex_position).r - .5;\
+  mediump float v = texture2D(vmap, tex_position).r - .5;\
+  lowp float r = clamp(y * 1.1644 + 1.5960 * v, 0.0, 1.0);\
+  lowp float g = clamp(y * 1.1644 - .3917 * u - .8129 * v, 0.0, 1.0);\
+  lowp float b = clamp(y * 1.1644 + 2.0172 * u, 0.0 ,1.0);\
   gl_FragColor = vec4(r, g, b, 1.0);\
 }\
 ";
 
+static const char* fragment_source_nv12 = "\
+precision mediump float;\
+\
+uniform mat3 yuvmat;\
+uniform vec3 offset;\
+uniform lowp sampler2D plane1;\
+uniform lowp sampler2D plane2;\
+varying vec2 tex_position;\
+\
+void main() {\
+	vec3 YCbCr = vec3(\
+		texture2D(plane1, tex_position)[0],\
+		texture2D(plane2, tex_position).xy\
+	);\
+\
+	YCbCr -= offset;\
+	gl_FragColor = vec4(clamp(yuvmat * YCbCr, 0.0, 1.0), 1.0f);\
+}\
+";
+
 static const float vertices[] = {
   -1.f,  1.f,
   -1.f, -1.f,
@@ -70,6 +124,39 @@ static const GLuint elements[] = {
   2, 3, 0
 };
 
+static const float limitedOffsets[] = { 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f };
+static const float fullOffsets[] = { 0.0f, 128.0f / 255.0f, 128.0f / 255.0f };
+static const float bt601Lim[] = {
+  1.1644f, 1.1644f, 1.1644f,
+  0.0f, -0.3917f, 2.0172f,
+  1.5960f, -0.8129f, 0.0f
+};
+static const float bt601Full[] = {
+    1.0f, 1.0f, 1.0f,
+    0.0f, -0.3441f, 1.7720f,
+    1.4020f, -0.7141f, 0.0f
+};
+static const float bt709Lim[] = {
+  1.1644f, 1.1644f, 1.1644f,
+  0.0f, -0.2132f, 2.1124f,
+  1.7927f, -0.5329f, 0.0f
+};
+static const float bt709Full[] = {
+  1.0f, 1.0f, 1.0f,
+  0.0f, -0.1873f, 1.8556f,
+  1.5748f, -0.4681f, 0.0f
+};
+static const float bt2020Lim[] = {
+  1.1644f, 1.1644f, 1.1644f,
+  0.0f, -0.1874f, 2.1418f,
+  1.6781f, -0.6505f, 0.0f
+};
+static const float bt2020Full[] = {
+  1.0f, 1.0f, 1.0f,
+  0.0f, -0.1646f, 1.8814f,
+  1.4746f, -0.5714f, 0.0f
+};
+
 static EGLDisplay display;
 static EGLSurface surface;
 static EGLContext context;
@@ -77,16 +164,137 @@ static bool current;
 static int width, height;
 static bool current;
 
-static GLuint texture_id[3], texture_uniform[3];
+static GLint vaapi_texture_uniform[4];
+static GLuint texture_id[4], texture_uniform[3];
 static GLuint shader_program;
 
-void egl_init(EGLNativeDisplayType native_display, NativeWindowType native_window, int display_width, int display_height) {
+static const float *colorOffsets;
+static const float *colorspace;
+
+static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
+
+static void chooseColorConfig(const AVFrame* frame)
+{
+  bool fullRange = false;
+  if (isFrameFullRange(frame)) {
+    fullRange = true;
+    colorOffsets = fullOffsets;
+  } else
+    colorOffsets = limitedOffsets;
+  switch (getFrameColorspace(frame)) {
+  case COLORSPACE_REC_601:
+    colorspace = fullRange ? bt601Full : bt601Lim;
+    break;
+  case COLORSPACE_REC_709:
+    colorspace = fullRange ? bt709Full : bt709Lim;
+    break;
+  case COLORSPACE_REC_2020:
+    colorspace = fullRange ? bt2020Full : bt2020Lim;
+    break;
+  default:
+    colorspace = bt601Lim;
+    break;
+  }
+}
+
+static bool isGlExtensionSupport(const char *extensionname) {
+  char firstChar = *extensionname;
+  const char *allExtensions = NULL;
+
+  if (firstChar == 'E') {
+    allExtensions = eglQueryString(display, EGL_EXTENSIONS);
+  } else {
+    // Only for GLES2
+    allExtensions = (char *)glGetString(GL_EXTENSIONS);
+  }
+  int len = strlen(allExtensions);
+  char splitstring[len + 1];
+  strcpy(splitstring, allExtensions);
+  char *token = strtok(splitstring, " ");
+  while (token != NULL) {
+    if (strcmp(extensionname, token) == 0) {
+      token = strtok(token, " ");
+      allExtensions = NULL;
+      return true;
+    }
+    token = strtok(NULL, " ");
+  }
+
+  return false;
+}
+
+static bool isUseExt(struct EXTSTATE *extState) {
+/*
+  if (isGlExtensionSupport("EGL_KHR_image_base")) {
+    extState->eglIsSupportExtKHR = true;
+  }
+  if (!isGlExtensionSupport("EGL_KHR_image")) {
+    extState->eglIsSupportExtKHR = false;
+  }
+*/
+  if (isGlExtensionSupport("EGL_KHR_image_base") || isGlExtensionSupport("EGL_KHR_image")) {
+    extState->eglIsSupportExtKHR = true;
+  }
+
+  if (isGlExtensionSupport("GL_OES_EGL_image")) {
+    extState->eglIsSupportImageOES = true;
+  }
+
+  glEGLImageTargetTexture2DOES = (typeof(glEGLImageTargetTexture2DOES))eglGetProcAddress("glEGLImageTargetTexture2DOES");
+  if (glEGLImageTargetTexture2DOES == NULL) {
+    extState->eglIsSupportImageOES = false;
+  }
+  
+  // For vaapi-EGL
+  if (isGlExtensionSupport("EGL_EXT_image_dma_buf_import")) {
+    extState->eglIsSupportExtDmaBuf = true;
+  }
+
+  if (isGlExtensionSupport("EGL_EXT_image_dma_buf_import_modifiers")) {
+    extState->eglIsSupportExtDmaBufMod = true; 
+  }
+
+  if (eglGetProcAddress("eglCreateImage") == NULL) {
+    extState->eglIsSupportCreateImage = false;
+  } else {
+    extState->eglIsSupportCreateImage = true;
+  }
+
+  if (extState->eglIsSupportExtDmaBuf && extState->eglIsSupportExtDmaBufMod &&
+       extState->eglIsSupportExtKHR && extState->eglIsSupportImageOES &&
+       extState->eglIsSupportCreateImage)
+    return true;
+
+  return false;
+}
+
+void egl_init(void *native_display, int display_width, int display_height, int dcFlag) {
   width = display_width;
   height = display_height;
+  int ffmpeg_decoder = 0;
 
+  isYUV444 = (dcFlag & YUV444) == YUV444 ? true : false;
+  ffmpeg_decoder = isYUV444 ? (dcFlag - YUV444) : dcFlag;
+#ifdef HAVE_WAYLAND
+  isWayland = (dcFlag & WAYLAND) == WAYLAND ? true : false;
+  ffmpeg_decoder = isWayland ? (ffmpeg_decoder - WAYLAND) : ffmpeg_decoder;
+#endif
+
   // get an EGL display connection
-  display = eglGetDisplay(native_display);
+  PFNEGLGETPLATFORMDISPLAYPROC eglGetPlatformDisplayProc;
+  eglGetPlatformDisplayProc = (typeof(eglGetPlatformDisplayProc)) eglGetProcAddress("eglGetPlatformDisplay");
+  if (eglGetPlatformDisplayProc != NULL) {
+    int platformFlag = isWayland ? EGL_PLATFORM_WAYLAND_KHR : EGL_PLATFORM_X11_KHR;
+    display = eglGetPlatformDisplayProc(platformFlag, native_display, NULL); //EGL_PLATFORM_X11_KHR for x11;EGL_PLATFORM_GBM_KHR for drm
+  }
   if (display == EGL_NO_DISPLAY) {
+#ifdef HAVE_WAYLAND
+    display = isWayland ? wl_get_egl_display() : x_get_egl_display();
+#else
+    display = x_get_egl_display();
+#endif
+  }
+  if (display == EGL_NO_DISPLAY) {
     fprintf( stderr, "EGL: error get display\n" );
     exit(EXIT_FAILURE);
   }
@@ -125,27 +333,41 @@ void egl_init(EGLNativeDisplayType native_display, Nat
   }
 
   // finally we can create a new surface using this config and window
-  surface = eglCreateWindowSurface(display, config, (NativeWindowType) native_window, NULL);
+#ifdef HAVE_WAYLAND
+  surface = isWayland ? wl_get_egl_surface(display, config, NULL) : x_get_egl_surface(display, config, NULL);
+#else
+  surface = x_get_egl_surface(display, config, NULL);
+#endif
+  if (surface == EGL_NO_SURFACE) {
+    fprintf(stderr, "EGL: couldn't get a valid egl surface\n");
+    exit (EXIT_FAILURE);
+  }
+
   eglMakeCurrent(display, surface, surface, context);
 
-  glEnable(GL_TEXTURE_2D);
+  // try to point if can use extensions for egl
+  if (isUseExt(&ExtState) && ffmpeg_decoder != SOFTWARE) {
+    isUseGlExt = true;
+  }
 
-  GLuint vbo;
-  glGenBuffers(1, &vbo);
-  glBindBuffer(GL_ARRAY_BUFFER, vbo);
-  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
+#ifdef HAVE_WAYLAND
+  // for wayland
+  if (isWayland)
+    eglSwapInterval(display, 0);
+#endif
 
-  GLuint ebo;
-  glGenBuffers(1, &ebo);
-  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
-  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(elements), elements, GL_STATIC_DRAW);
+  glEnable(GL_TEXTURE_2D);
 
   GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
+  GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
+
   glShaderSource(vertex_shader, 1, &vertex_source, NULL);
-  glCompileShader(vertex_shader);
+  if (ffmpeg_decoder != SOFTWARE)
+    glShaderSource(fragment_shader, 1, &fragment_source_nv12, NULL);
+  else
+    glShaderSource(fragment_shader, 1, &fragment_source_420p, NULL);
 
-  GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
-  glShaderSource(fragment_shader, 1, &fragment_source, NULL);
+  glCompileShader(vertex_shader);
   glCompileShader(fragment_shader);
 
   shader_program = glCreateProgram();
@@ -153,17 +375,42 @@ void egl_init(EGLNativeDisplayType native_display, Nat
   glAttachShader(shader_program, fragment_shader);
 
   glLinkProgram(shader_program);
+
+  GLuint vbo;
+  glGenBuffers(1, &vbo);
+  glBindBuffer(GL_ARRAY_BUFFER, vbo);
+
+  GLuint ebo;
+  glGenBuffers(1, &ebo);
+  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
+
+  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
+  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(elements), elements, GL_STATIC_DRAW);
   glBindAttribLocation(shader_program, 0, "position");
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), 0);
 
-  glGenTextures(3, texture_id);
-  for (int i = 0; i < 3; i++) {
-    glBindTexture(GL_TEXTURE_2D, texture_id[i]);
-    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
-    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
-    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, i > 0 ? width / 2 : width, i > 0 ? height / 2 : height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0);
+  int egl_max_planes;
+  if (ffmpeg_decoder != SOFTWARE) {
+    vaapi_texture_uniform[NV12_PARAM_YUVMAT] = glGetUniformLocation(shader_program, "yuvmat");
+    vaapi_texture_uniform[NV12_PARAM_OFFSET] = glGetUniformLocation(shader_program, "offset");
+    vaapi_texture_uniform[NV12_PARAM_PLANE1] = glGetUniformLocation(shader_program, "plane1");
+    vaapi_texture_uniform[NV12_PARAM_PLANE2] = glGetUniformLocation(shader_program, "plane2");
+    egl_max_planes = 4;
+  } else {
+    egl_max_planes = 3;
+  }
 
-    texture_uniform[i] = glGetUniformLocation(shader_program, texture_mappings[i]);
+  glGenTextures(egl_max_planes, texture_id);
+  for (int i = 0; i < egl_max_planes; i++) {
+      glBindTexture(GL_TEXTURE_2D, texture_id[i]);
+      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
+      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
+      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
+      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
+      if (ffmpeg_decoder == SOFTWARE) {
+        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, i > 0 ? width / 2 : width, i > 0 ? height / 2 : height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0);
+        texture_uniform[i] = glGetUniformLocation(shader_program, texture_mappings[i]);
+      }
   }
 
   eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
@@ -182,12 +429,52 @@ void egl_draw(uint8_t* image[3]) {
     glActiveTexture(GL_TEXTURE0 + i);
     glBindTexture(GL_TEXTURE_2D, texture_id[i]);
     glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, i > 0 ? width / 2 : width, i > 0 ? height / 2 : height, GL_LUMINANCE, GL_UNSIGNED_BYTE, image[i]);
+
     glUniform1i(texture_uniform[i], i);
   }
 
   glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
 
   eglSwapBuffers(display, surface);
+}
+
+void egl_draw_frame(AVFrame* frame) {
+  if (!current) {
+    chooseColorConfig(frame);
+    eglMakeCurrent(display, surface, surface, context);
+    current = true;
+  }
+  EGLImage image[4];
+
+  glUseProgram(shader_program);
+  glEnableVertexAttribArray(0);
+
+  ssize_t plane_count = exportEGLImages(frame, display, ExtState.eglIsSupportExtDmaBufMod,
+                                        image);
+  if (plane_count < 0) {
+      printf("Error in egl_drawing frame\n");
+      return;
+  }
+  
+  for (int i = 0; i < plane_count; i++) {
+     glActiveTexture(GL_TEXTURE0 + i);
+     glBindTexture(GL_TEXTURE_2D, texture_id[i]);
+     glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image[i]);
+  }
+
+
+  // Bind parameters for the shaders
+  // for nv12 or p101
+  glUniformMatrix3fv(vaapi_texture_uniform[NV12_PARAM_YUVMAT], 1, GL_FALSE, colorspace);
+  glUniform3fv(vaapi_texture_uniform[NV12_PARAM_OFFSET], 1, colorOffsets);
+  glUniform1i(vaapi_texture_uniform[NV12_PARAM_PLANE1], 0);
+  glUniform1i(vaapi_texture_uniform[NV12_PARAM_PLANE2], 1);
+
+  glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
+
+  eglSwapBuffers(display, surface);
+
+  freeEGLImages(display, image);
 }
 
 void egl_destroy() {
