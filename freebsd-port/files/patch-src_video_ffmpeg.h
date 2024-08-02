--- src/video/ffmpeg.h.orig	2024-08-01 13:37:02 UTC
+++ src/video/ffmpeg.h
@@ -17,8 +17,6 @@
  * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
  */
 
-#include <stdbool.h>
-
 #include <libavcodec/avcodec.h>
 
 // Enable multi-threaded decoding
@@ -29,10 +27,11 @@ extern enum decoders ffmpeg_decoder;
 
 enum decoders {SOFTWARE, VDPAU, VAAPI};
 extern enum decoders ffmpeg_decoder;
+extern bool isYUV444;
 
 int ffmpeg_init(int videoFormat, int width, int height, int perf_lvl, int buffer_count, int thread_count);
 void ffmpeg_destroy(void);
-
-int ffmpeg_draw_frame(AVFrame *pict);
 AVFrame* ffmpeg_get_frame(bool native_frame);
 int ffmpeg_decode(unsigned char* indata, int inlen);
+int ffmpeg_is_frame_full_range(const AVFrame* frame);
+int ffmpeg_get_frame_colorspace(const AVFrame* frame);
