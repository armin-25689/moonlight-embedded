--- src/video/ffmpeg.c.orig	2024-08-01 13:37:02 UTC
+++ src/video/ffmpeg.c
@@ -17,13 +17,6 @@
  * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
  */
 
-#include "ffmpeg.h"
-
-#ifdef HAVE_VAAPI
-#include "ffmpeg_vaapi.h"
-#endif
-
-#include <Limelight.h>
 #include <libavcodec/avcodec.h>
 
 #include <stdlib.h>
@@ -31,6 +24,12 @@
 #include <stdio.h>
 #include <stdbool.h>
 
+#include <Limelight.h>
+#include "ffmpeg.h"
+#ifdef HAVE_VAAPI
+#include "ffmpeg_vaapi.h"
+#endif
+
 // General decoder and renderer state
 static AVPacket* pkt;
 static const AVCodec* decoder;
@@ -40,6 +39,9 @@ static int current_frame, next_frame;
 static int dec_frames_cnt;
 static int current_frame, next_frame;
 
+bool isSupportYuv444 = false;
+bool wantYuv444 = false;
+bool isYUV444 = false;
 enum decoders ffmpeg_decoder;
 
 #define BYTES_PER_PIXEL 4
@@ -60,6 +62,17 @@ int ffmpeg_init(int videoFormat, int width, int height
   }
 
   ffmpeg_decoder = perf_lvl & VAAPI_ACCELERATION ? VAAPI : SOFTWARE;
+  if (wantYuv444 && !(videoFormat & VIDEO_FORMAT_MASK_YUV444)) {
+    if (isSupportYuv444) {
+      printf("Could not start yuv444 stream because of server support, fallback to yuv420 format\n");
+    }
+    else {
+      printf("Could not start yuv444 stream because of client support, fallback to yuv420 format\n");
+    }
+  }
+  if (videoFormat & VIDEO_FORMAT_MASK_YUV444 && isSupportYuv444) {
+    isYUV444 = true;
+  }
 
   for (int try = 0; try < 6; try++) {
     if (videoFormat & VIDEO_FORMAT_MASK_H264) {
@@ -118,8 +131,22 @@ int ffmpeg_init(int videoFormat, int width, int height
 
     decoder_ctx->width = width;
     decoder_ctx->height = height;
-    decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
 
+    if (videoFormat & VIDEO_FORMAT_MASK_YUV444)
+      decoder_ctx->pix_fmt = AV_PIX_FMT_YUV444P;
+    else
+      decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
+
+    #ifdef HAVE_VAAPI
+    if (ffmpeg_decoder == VAAPI) {
+      vaapi_init(decoder_ctx);
+      #define NEED_YUV444 1
+      if (videoFormat & VIDEO_FORMAT_MASK_YUV444)
+        isSupportYuv444 = vaapi_is_support_yuv444(NEED_YUV444);
+      #undef NEED_YUV444
+    }
+    #endif
+
     int err = avcodec_open2(decoder_ctx, decoder, NULL);
     if (err < 0) {
       printf("Couldn't open codec: %s\n", decoder->name);
@@ -148,13 +175,21 @@ int ffmpeg_init(int videoFormat, int width, int height
       fprintf(stderr, "Couldn't allocate frame");
       return -1;
     }
+    if (ffmpeg_decoder == SOFTWARE) {
+      int widthMulti = isYUV444 ? 64 : 128;
+      if (width % widthMulti != 0) {
+        dec_frames[i]->format = decoder_ctx->pix_fmt;
+        dec_frames[i]->width = decoder_ctx->width;
+        dec_frames[i]->height = decoder_ctx->height;
+        // glteximage2d need 64 type least
+        if (av_frame_get_buffer(dec_frames[i], widthMulti) < 0) {
+          fprintf(stderr, "Couldn't allocate frame buffer");
+          return -1;
+        }
+      }
+    }
   }
 
-  #ifdef HAVE_VAAPI
-  if (ffmpeg_decoder == VAAPI)
-    vaapi_init(decoder_ctx);
-  #endif
-
   return 0;
 }
 
@@ -170,7 +205,11 @@ void ffmpeg_destroy(void) {
       if (dec_frames[i])
         av_frame_free(&dec_frames[i]);
     }
+    free(dec_frames);
+    dec_frames = NULL;
   }
+  decoder_ctx = NULL;
+  decoder = NULL;
 }
 
 AVFrame* ffmpeg_get_frame(bool native_frame) {
@@ -205,4 +244,23 @@ int ffmpeg_decode(unsigned char* indata, int inlen) {
   }
 
   return err < 0 ? err : 0;
+}
+
+int ffmpeg_is_frame_full_range(const AVFrame* frame) {
+  return frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
+}
+
+int ffmpeg_get_frame_colorspace(const AVFrame* frame) {
+  switch (frame->colorspace) {
+  case AVCOL_SPC_SMPTE170M:
+  case AVCOL_SPC_BT470BG:
+    return COLORSPACE_REC_601;
+  case AVCOL_SPC_BT709:
+    return COLORSPACE_REC_709;
+  case AVCOL_SPC_BT2020_NCL:
+  case AVCOL_SPC_BT2020_CL:
+    return COLORSPACE_REC_2020;
+  default:
+    return COLORSPACE_REC_601;
+  }
 }
