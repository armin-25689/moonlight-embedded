--- src/video/ffmpeg.c.orig	2024-02-20 04:01:31 UTC
+++ src/video/ffmpeg.c
@@ -118,8 +118,17 @@ int ffmpeg_init(int videoFormat, int width, int height
 
     decoder_ctx->width = width;
     decoder_ctx->height = height;
-    decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
 
+    if (videoFormat & VIDEO_FORMAT_MASK_YUV444)
+      decoder_ctx->pix_fmt = AV_PIX_FMT_YUV444P;
+    else
+      decoder_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
+
+    #ifdef HAVE_VAAPI
+    if (ffmpeg_decoder == VAAPI)
+      vaapi_init(decoder_ctx);
+    #endif
+
     int err = avcodec_open2(decoder_ctx, decoder, NULL);
     if (err < 0) {
       printf("Couldn't open codec: %s\n", decoder->name);
@@ -148,13 +157,21 @@ int ffmpeg_init(int videoFormat, int width, int height
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
 
@@ -170,7 +187,11 @@ void ffmpeg_destroy(void) {
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
