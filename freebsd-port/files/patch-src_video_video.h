--- src/video/video.h.orig	2024-08-01 13:37:02 UTC
+++ src/video/video.h
@@ -48,3 +48,6 @@ extern DECODER_RENDERER_CALLBACKS decoder_callbacks_sd
 #ifdef HAVE_SDL
 extern DECODER_RENDERER_CALLBACKS decoder_callbacks_sdl;
 #endif
+
+extern bool isSupportYuv444;
+extern bool wantYuv444;
