--- src/video/ffmpeg.c.orig	2024-02-20 04:01:31 UTC
+++ src/video/ffmpeg.c
@@ -42,6 +42,9 @@ enum decoders ffmpeg_decoder;
 
 enum decoders ffmpeg_decoder;
 
+#ifndef HAVE_VAAPI
+static bool isYUV444 = false;
+#endif
 #define BYTES_PER_PIXEL 4
 
 // This function must be called before
@@ -60,6 +63,11 @@ int ffmpeg_init(int videoFormat, int width, int height
   }
 
   ffmpeg_decoder = perf_lvl & VAAPI_ACCELERATION ? VAAPI : SOFTWARE;
+  if (ffmpeg_decoder == SOFTWARE) {
+    if (isYUV444)
+      printf("Software decoder cannot use YUV444 format.Use YUV444P instead.\n");
+    isYUV444 = false;
+  }
 
   for (int try = 0; try < 6; try++) {
     if (videoFormat & VIDEO_FORMAT_MASK_H264) {
@@ -118,7 +126,11 @@ int ffmpeg_init(int videoFormat, int width, int height
 
     decoder_ctx->width = width;
     decoder_ctx->height = height;
-    decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
+
+    if (isYUV444)
+      decoder_ctx->pix_fmt = AV_PIX_FMT_YUV444P;
+    else
+      decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
 
     int err = avcodec_open2(decoder_ctx, decoder, NULL);
     if (err < 0) {
