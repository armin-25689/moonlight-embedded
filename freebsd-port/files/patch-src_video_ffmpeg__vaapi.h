--- src/video/ffmpeg_vaapi.h.orig	2024-02-20 04:01:31 UTC
+++ src/video/ffmpeg_vaapi.h
@@ -17,9 +17,22 @@
  * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
  */
 
+#include <EGL/egl.h>
 #include <va/va.h>
 #include <X11/Xlib.h>
+#include <stdbool.h>
 
-int vaapi_init_lib();
+extern bool isYUV444;
+
+int vaapi_init_lib(const char *device);
 int vaapi_init(AVCodecContext* decoder_ctx);
 void vaapi_queue(AVFrame* dec_frame, Window win, int width, int height);
+bool test_vaapi_queue(AVFrame* dec_frame, Window win, int width, int height);
+void freeEGLImages(EGLDisplay dpy, EGLImage images[4]);
+ssize_t exportEGLImages(AVFrame *frame, EGLDisplay dpy, bool eglIsSupportExtDmaBufMod,
+                        EGLImage images[4]);
+bool canExportSurfaceHandle(bool isTenBit);
+bool isVaapiCanDirectRender();
+bool isFrameFullRange(const AVFrame* frame);
+int getFrameColorspace(const AVFrame* frame);
+void *get_display_from_vaapi(bool isXDisplay);
