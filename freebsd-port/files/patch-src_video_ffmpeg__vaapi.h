--- src/video/ffmpeg_vaapi.h.orig	2024-08-01 13:37:02 UTC
+++ src/video/ffmpeg_vaapi.h
@@ -17,9 +17,12 @@
  * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
  */
 
-#include <va/va.h>
-#include <X11/Xlib.h>
-
-int vaapi_init_lib();
+int vaapi_init_lib(const char *device);
 int vaapi_init(AVCodecContext* decoder_ctx);
-void vaapi_queue(AVFrame* dec_frame, Window win, int width, int height);
+#ifdef HAVE_X11
+int vaapi_queue(AVFrame* dec_frame, void *window, int width, int height);
+#endif
+bool vaapi_can_export_surface_handle(bool isTenBit);
+bool vaapi_is_can_direct_render();
+void *vaapi_get_display(bool isXDisplay);
+bool vaapi_is_support_yuv444(int needyuv444);
